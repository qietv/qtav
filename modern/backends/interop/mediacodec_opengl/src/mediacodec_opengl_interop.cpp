// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/mediacodec_opengl_interop.h>

#include "mediacodec_image_epoch.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace qtav {
namespace {

constexpr std::int64_t TimestampToleranceNanoseconds = 2'000'000;
constexpr std::int64_t MaximumPresentationLagNanoseconds = 250'000'000;

void closeDescriptor(int& descriptor) noexcept
{
    if (descriptor >= 0) {
        close(descriptor);
        descriptor = -1;
    }
}

bool hasExtension(const char* extensions, const char* name) noexcept
{
    if (!extensions || !name || !*name) {
        return false;
    }
    const std::size_t length = std::strlen(name);
    const char* current = extensions;
    while ((current = std::strstr(current, name))) {
        const bool left = current == extensions || current[-1] == ' ';
        const bool right = current[length] == '\0'
            || current[length] == ' ';
        if (left && right) {
            return true;
        }
        current += length;
    }
    return false;
}

struct PendingImage {
    AImage* image = nullptr;
    int acquireFence = -1;
    std::int64_t timestampNanoseconds = 0;
    detail::MediaCodecImageFrameKey frameKey;
    std::uint64_t producerEpoch = 0;
};

void discardImage(PendingImage& pending) noexcept
{
    if (!pending.image) {
        closeDescriptor(pending.acquireFence);
        return;
    }
    if (pending.acquireFence >= 0) {
        AImage_deleteAsync(pending.image, pending.acquireFence);
        pending.acquireFence = -1;
    } else {
        AImage_delete(pending.image);
    }
    pending.image = nullptr;
}

detail::MediaCodecImageFrameKey frameKey(
    const VideoFrame& frame) noexcept
{
    detail::MediaCodecImageFrameKey result;
    if (!frame || !frame.hasHardwareFrame()) {
        return result;
    }
    const NativeHandle output = frame.hardwareFrame().nativeHandle(
        HardwareHandleType::Frame);
    result.buffer = output.value;
    result.surfaceGeneration = output.subresource;
    result.timestampMilliseconds = frame.timestamp();
    return result;
}

void updateMaximum(
    std::atomic<std::uint64_t>& maximum,
    std::uint64_t value) noexcept
{
    std::uint64_t current = maximum.load(std::memory_order_relaxed);
    while (current < value
           && !maximum.compare_exchange_weak(
               current,
               value,
               std::memory_order_relaxed)) {
    }
}

struct AtomicStatistics {
    std::atomic<std::uint64_t> codecOutputsQueued { 0 };
    std::atomic<std::uint64_t> imagesAcquired { 0 };
    std::atomic<std::uint64_t> eglImagesImported { 0 };
    std::atomic<std::uint64_t> eglImagesDestroyed { 0 };
    std::atomic<std::uint64_t> rawTextureBindings { 0 };
    std::atomic<std::uint64_t> redrawSignals { 0 };
    std::atomic<std::uint64_t> staleFramesDropped { 0 };
    std::atomic<std::uint64_t> maximumPendingFrames { 0 };
    std::atomic<std::int64_t> lastTimestampNanoseconds { 0 };
    std::atomic<std::uint32_t> textureName { 0 };
    std::atomic<std::uint64_t> acquireFencesWaited { 0 };
    std::atomic<std::uint64_t> releaseFencesReturned { 0 };
    std::atomic<std::uint64_t> releaseFenceFallbacks { 0 };
    std::atomic<std::uint64_t> rawYcbcrImports { 0 };
    std::atomic<std::uint32_t> lastHardwareBufferFormat { 0 };
};

struct SharedState {
    ~SharedState()
    {
        if (reader) {
            AImageReader_setImageListener(reader, nullptr);
        }
        std::deque<PendingImage> pending;
        {
            std::lock_guard<std::mutex> lock(mutex);
            shuttingDown = true;
            pending.swap(images);
            frameAvailable = {};
        }
        for (auto& image : pending) {
            discardImage(image);
        }
        surface = {};
        if (reader) {
            AImageReader_delete(reader);
            reader = nullptr;
        }
    }

    MediaCodecOpenGLInteropConfig config;
    AImageReader* reader = nullptr;
    MediaCodecSurface surface;
    mutable std::mutex mutex;
    std::deque<PendingImage> images;
    detail::MediaCodecImageEpochTracker producerEpoch {
        64,
        TimestampToleranceNanoseconds,
    };
    OpenGLHardwareFrameInterop::FrameAvailableCallback frameAvailable;
    std::string asyncError;
    std::string lastRuntimeError;
    bool shuttingDown = false;
    AtomicStatistics statistics;
};

void onImageAvailable(void* context, AImageReader* reader) noexcept
{
    auto* state = static_cast<SharedState*>(context);
    if (!state || !reader) {
        return;
    }
    bool acquiredAny = false;
    for (;;) {
        PendingImage pending;
        const media_status_t status =
            AImageReader_acquireNextImageAsync(
                reader,
                &pending.image,
                &pending.acquireFence);
        if (status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE
            || status == AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED) {
            break;
        }
        if (status != AMEDIA_OK || !pending.image) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->shuttingDown) {
                state->asyncError =
                    "AImageReader_acquireNextImageAsync failed: "
                    + std::to_string(status);
            }
            break;
        }
        if (AImage_getTimestamp(
                pending.image,
                &pending.timestampNanoseconds)
                != AMEDIA_OK
            || pending.timestampNanoseconds < 0) {
            discardImage(pending);
            state->statistics.staleFramesDropped.fetch_add(
                1,
                std::memory_order_relaxed);
            continue;
        }
        acquiredAny = true;
        state->statistics.imagesAcquired.fetch_add(
            1,
            std::memory_order_relaxed);
        PendingImage discarded;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->shuttingDown) {
                discarded = pending;
            } else {
                const auto association =
                    state->producerEpoch.associateImage(
                        pending.timestampNanoseconds);
                if (!association.matched || !association.current) {
                    discarded = pending;
                    state->statistics.staleFramesDropped.fetch_add(
                        1,
                        std::memory_order_relaxed);
                } else {
                    pending.frameKey = association.key;
                    pending.producerEpoch = association.epoch;
                    state->images.push_back(pending);
                    if (static_cast<int>(state->images.size())
                        > state->config.maximumPendingFrames) {
                        discarded = state->images.front();
                        state->images.pop_front();
                        state->statistics.staleFramesDropped.fetch_add(
                            1,
                            std::memory_order_relaxed);
                    }
                    updateMaximum(
                        state->statistics.maximumPendingFrames,
                        state->images.size());
                }
            }
        }
        // Do not retain evicted acquisitions until the callback has finished
        // draining. Holding those AImages can hit maxImages and leave an
        // already-queued producer buffer without a future listener edge.
        discardImage(discarded);
    }

    OpenGLHardwareFrameInterop::FrameAvailableCallback callback;
    bool hasError = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        callback = state->frameAvailable;
        hasError = !state->asyncError.empty();
    }
    if ((acquiredAny || hasError) && callback) {
        state->statistics.redrawSignals.fetch_add(
            1,
            std::memory_order_relaxed);
        callback();
    }
}

template <typename Function>
Function eglFunction(const char* name) noexcept
{
    return reinterpret_cast<Function>(eglGetProcAddress(name));
}

struct EGLFunctions {
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC getNativeClientBuffer = nullptr;
    PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture = nullptr;
    PFNEGLCREATESYNCKHRPROC createSync = nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroySync = nullptr;
    PFNEGLWAITSYNCKHRPROC waitSync = nullptr;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC duplicateFence = nullptr;

    bool isValid() const noexcept
    {
        return getNativeClientBuffer && createImage && destroyImage
            && imageTargetTexture && createSync && destroySync
            && waitSync && duplicateFence;
    }
};

struct CurrentImage {
    AImage* image = nullptr;
    int acquireFence = -1;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLImageKHR eglImage = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
    OpenGLExternalTextureFrame frame;
};

} // namespace

class MediaCodecOpenGLInterop::Impl {
public:
    explicit Impl(MediaCodecOpenGLInteropConfig config)
        : state_(std::make_shared<SharedState>())
    {
        state_->config = config;
        state_->config.width = std::max(1, state_->config.width);
        state_->config.height = std::max(1, state_->config.height);
        state_->config.maximumPendingFrames = std::clamp(
            state_->config.maximumPendingFrames,
            4,
            16);
        initialize();
    }

    ~Impl()
    {
        // The renderer normally releases every imported image with its EGL
        // context current. Return an exceptional leftover image without
        // touching stale GL/EGL objects.
        std::lock_guard<std::mutex> lock(currentMutex_);
        if (current_.image) {
            if (current_.acquireFence >= 0) {
                AImage_deleteAsync(
                    current_.image,
                    current_.acquireFence);
                current_.acquireFence = -1;
            } else {
                AImage_delete(current_.image);
            }
            current_.image = nullptr;
        }
    }

    void initialize()
    {
        // Keep two acquisition slots outside the timestamp-correlation
        // window. The listener may run while one matched image is being
        // rendered, and it must still be able to acquire-and-retire a stale
        // producer buffer without reaching MAX_IMAGES_ACQUIRED.
        const int readerMaximumImages =
            state_->config.maximumPendingFrames + 2;
        const media_status_t status = AImageReader_newWithUsage(
            state_->config.width,
            state_->config.height,
            AIMAGE_FORMAT_PRIVATE,
            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
            readerMaximumImages,
            &state_->reader);
        if (status != AMEDIA_OK || !state_->reader) {
            error_ =
                "AImageReader_newWithUsage(PRIVATE, GPU_SAMPLED_IMAGE) failed: "
                + std::to_string(status);
            return;
        }
        ANativeWindow* window = nullptr;
        if (AImageReader_getWindow(state_->reader, &window) != AMEDIA_OK
            || !window) {
            error_ = "AImageReader_getWindow failed";
            return;
        }
        state_->surface = MediaCodecSurface(window);
        if (!state_->surface) {
            error_ =
                "Could not retain the private AImageReader producer window";
            return;
        }
        AImageReader_ImageListener listener {};
        listener.context = state_.get();
        listener.onImageAvailable = &onImageAvailable;
        if (AImageReader_setImageListener(state_->reader, &listener)
            != AMEDIA_OK) {
            error_ = "AImageReader_setImageListener failed";
            return;
        }
        valid_ = true;
    }

    bool supports(const HardwareFrame& frame) const noexcept
    {
        if (!valid_ || !frame
            || frame.deviceType() != HardwareDeviceType::MediaCodec) {
            return false;
        }
        const NativeHandle output = frame.nativeHandle(
            HardwareHandleType::Frame);
        const NativeHandle sourceSurface = frame.nativeHandle(
            HardwareHandleType::Surface);
        return output && sourceSurface
            && output.subresource == state_->surface.generation()
            && sourceSurface.subresource == state_->surface.generation()
            && sourceSurface.value
                == reinterpret_cast<std::uintptr_t>(
                    state_->surface.nativeWindow());
    }

    OpenGLHardwareImportStatus queueFrame(
        const VideoFrame& frame,
        std::string& detail)
    {
        const detail::MediaCodecImageFrameKey key = frameKey(frame);
        if (key.buffer == 0 || key.surfaceGeneration == 0) {
            detail = "The frame is not a MediaCodec surface output";
            return OpenGLHardwareImportStatus::Unsupported;
        }
        if (!supports(frame.hardwareFrame())) {
            detail =
                "The MediaCodec frame belongs to a stale or foreign AImageReader surface";
            return OpenGLHardwareImportStatus::Stale;
        }

        std::deque<PendingImage> discarded;
        bool imageReady = false;
        detail::MediaCodecProducerQueueResult queueResult;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->asyncError.empty()) {
                detail = std::move(state_->asyncError);
                state_->asyncError.clear();
                return OpenGLHardwareImportStatus::Error;
            }
            const std::int64_t expected =
                frame.timestamp() * 1'000'000LL;
            const std::uint64_t currentEpoch =
                state_->producerEpoch.currentEpoch();
            for (auto iterator = state_->images.begin();
                 iterator != state_->images.end();) {
                if (iterator->producerEpoch != currentEpoch
                    || iterator->timestampNanoseconds
                        < expected - MaximumPresentationLagNanoseconds) {
                    discarded.push_back(*iterator);
                    iterator = state_->images.erase(iterator);
                    state_->statistics.staleFramesDropped.fetch_add(
                        1,
                        std::memory_order_relaxed);
                    continue;
                }
                if (iterator->frameKey == key) {
                    imageReady = true;
                }
                ++iterator;
            }
            if (!imageReady) {
                queueResult = state_->producerEpoch.begin(key);
            }
        }
        for (auto& image : discarded) {
            discardImage(image);
        }
        if (imageReady) {
            return OpenGLHardwareImportStatus::Ready;
        }
        if (queueResult.status
            == detail::MediaCodecProducerQueueStatus::Retired) {
            return OpenGLHardwareImportStatus::Stale;
        }
        if (queueResult.status
                == detail::MediaCodecProducerQueueStatus::Pending
            || queueResult.status
                == detail::MediaCodecProducerQueueStatus::CapacityReached) {
            return OpenGLHardwareImportStatus::Pending;
        }

        MediaCodecFrame output = mediaCodecFrame(frame, state_->surface);
        const bool released =
            output && output.isPending() && output.present();
        if (!released) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->producerEpoch.cancel(key, queueResult.epoch);
            detail =
                "Could not release the MediaCodec output into the private AImageReader";
            return OpenGLHardwareImportStatus::Error;
        }
        state_->statistics.codecOutputsQueued.fetch_add(
            1,
            std::memory_order_relaxed);
        return OpenGLHardwareImportStatus::Pending;
    }

    bool queueFrameForPresentation(
        const VideoFrame& frame,
        std::string& detail)
    {
        const OpenGLHardwareImportStatus status = queueFrame(frame, detail);
        return status == OpenGLHardwareImportStatus::Pending
            || status == OpenGLHardwareImportStatus::Ready;
    }

    PendingImage takeImage(const VideoFrame& frame)
    {
        PendingImage matched;
        std::deque<PendingImage> discarded;
        const detail::MediaCodecImageFrameKey key = frameKey(frame);
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            const std::int64_t expected =
                frame.timestamp() * 1'000'000LL;
            const std::uint64_t currentEpoch =
                state_->producerEpoch.currentEpoch();
            for (auto iterator = state_->images.begin();
                 iterator != state_->images.end();) {
                if (iterator->producerEpoch != currentEpoch
                    || iterator->timestampNanoseconds
                        < expected - MaximumPresentationLagNanoseconds) {
                    discarded.push_back(*iterator);
                    iterator = state_->images.erase(iterator);
                    state_->statistics.staleFramesDropped.fetch_add(
                        1,
                        std::memory_order_relaxed);
                    continue;
                }
                ++iterator;
            }
            const auto exact = std::find_if(
                state_->images.begin(),
                state_->images.end(),
                [&key, currentEpoch](const PendingImage& image) {
                    return image.producerEpoch == currentEpoch
                        && image.frameKey == key;
                });
            if (exact != state_->images.end()) {
                matched = *exact;
                state_->images.erase(exact);
            }
        }
        for (auto& image : discarded) {
            discardImage(image);
        }
        return matched;
    }

    bool loadEGL(std::string& detail)
    {
        const EGLDisplay display = eglGetCurrentDisplay();
        const EGLContext context = eglGetCurrentContext();
        if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT) {
            detail =
                "AHardwareBuffer OpenGL import requires a current EGL context";
            return false;
        }
        if (eglDisplay_ == display && eglContext_ == context
            && functions_.isValid()) {
            return true;
        }
        const char* eglExtensions = eglQueryString(display, EGL_EXTENSIONS);
        const char* glExtensions = reinterpret_cast<const char*>(
            glGetString(GL_EXTENSIONS));
        if (!hasExtension(
                eglExtensions,
                "EGL_ANDROID_get_native_client_buffer")
            || !hasExtension(
                eglExtensions,
                "EGL_ANDROID_image_native_buffer")
            || !hasExtension(eglExtensions, "EGL_KHR_image_base")
            || !hasExtension(
                eglExtensions,
                "EGL_ANDROID_native_fence_sync")
            || !hasExtension(eglExtensions, "EGL_KHR_wait_sync")
            || !hasExtension(
                glExtensions,
                "GL_OES_EGL_image_external_essl3")
            || !hasExtension(glExtensions, "GL_EXT_YUV_target")) {
            detail =
                "Raw AHardwareBuffer OpenGL import requires EGL native-buffer/image/native-fence/wait-sync and GL external-YUV extensions";
            return false;
        }
        EGLFunctions loaded;
        loaded.getNativeClientBuffer =
            eglFunction<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
                "eglGetNativeClientBufferANDROID");
        loaded.createImage = eglFunction<PFNEGLCREATEIMAGEKHRPROC>(
            "eglCreateImageKHR");
        loaded.destroyImage = eglFunction<PFNEGLDESTROYIMAGEKHRPROC>(
            "eglDestroyImageKHR");
        loaded.imageTargetTexture =
            eglFunction<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
                "glEGLImageTargetTexture2DOES");
        loaded.createSync = eglFunction<PFNEGLCREATESYNCKHRPROC>(
            "eglCreateSyncKHR");
        loaded.destroySync = eglFunction<PFNEGLDESTROYSYNCKHRPROC>(
            "eglDestroySyncKHR");
        loaded.waitSync = eglFunction<PFNEGLWAITSYNCKHRPROC>(
            "eglWaitSyncKHR");
        loaded.duplicateFence =
            eglFunction<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(
                "eglDupNativeFenceFDANDROID");
        if (!loaded.isValid()) {
            detail =
                "The EGL implementation did not expose the required AHardwareBuffer and native-fence entry points";
            return false;
        }
        functions_ = loaded;
        eglDisplay_ = display;
        eglContext_ = context;
        return true;
    }

    bool waitForAcquireFence(CurrentImage& current, std::string& detail)
    {
        if (current.acquireFence < 0) {
            return true;
        }
        const EGLint attributes[] {
            EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
            current.acquireFence,
            EGL_NONE,
        };
        EGLSyncKHR sync = functions_.createSync(
            current.display,
            EGL_SYNC_NATIVE_FENCE_ANDROID,
            attributes);
        if (sync == EGL_NO_SYNC_KHR) {
            detail = "eglCreateSyncKHR(AImage acquire fence) failed";
            return false;
        }
        // A successful native-fence sync import takes ownership of the fd.
        current.acquireFence = -1;
        const EGLint waitResult = functions_.waitSync(
            current.display,
            sync,
            0);
        functions_.destroySync(current.display, sync);
        if (waitResult != EGL_TRUE) {
            detail = "eglWaitSyncKHR(AImage acquire fence) failed";
            return false;
        }
        state_->statistics.acquireFencesWaited.fetch_add(
            1,
            std::memory_order_relaxed);
        return true;
    }

    bool importImage(
        PendingImage pending,
        OpenGLExternalTextureFrame& output,
        std::string& detail)
    {
        std::lock_guard<std::mutex> lock(currentMutex_);
        if (current_.image) {
            detail =
                "The previous AHardwareBuffer OpenGL image has not been released";
            discardImage(pending);
            return false;
        }
        if (!loadEGL(detail)) {
            discardImage(pending);
            return false;
        }

        CurrentImage current;
        current.image = pending.image;
        current.acquireFence = pending.acquireFence;
        current.display = eglDisplay_;
        pending = {};
        if (!waitForAcquireFence(current, detail)) {
            cleanupCurrent(current, false);
            return false;
        }

        AHardwareBuffer* hardwareBuffer = nullptr;
        if (AImage_getHardwareBuffer(current.image, &hardwareBuffer)
                != AMEDIA_OK
            || !hardwareBuffer) {
            detail = "AImage_getHardwareBuffer failed";
            cleanupCurrent(current, false);
            return false;
        }
        AHardwareBuffer_Desc description {};
        AHardwareBuffer_describe(hardwareBuffer, &description);
        if (description.layers != 1
            || (description.usage
                & AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE)
                == 0U
            || (description.usage
                & AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT)
                != 0U) {
            detail =
                "The acquired AHardwareBuffer is not an unprotected single-layer sampled image";
            cleanupCurrent(current, false);
            return false;
        }
        AImageCropRect crop {};
        if (AImage_getCropRect(current.image, &crop) != AMEDIA_OK
            || crop.left < 0 || crop.top < 0
            || crop.right <= crop.left || crop.bottom <= crop.top
            || crop.right > static_cast<std::int32_t>(description.width)
            || crop.bottom > static_cast<std::int32_t>(description.height)) {
            detail =
                "AImage returned a crop rectangle outside its AHardwareBuffer";
            cleanupCurrent(current, false);
            return false;
        }
        std::int64_t timestamp = 0;
        if (AImage_getTimestamp(current.image, &timestamp) != AMEDIA_OK
            || timestamp < 0) {
            detail = "AImage returned an invalid timestamp";
            cleanupCurrent(current, false);
            return false;
        }

        const EGLClientBuffer clientBuffer =
            functions_.getNativeClientBuffer(hardwareBuffer);
        const EGLint imageAttributes[] {
            EGL_IMAGE_PRESERVED_KHR,
            EGL_TRUE,
            EGL_NONE,
        };
        current.eglImage = functions_.createImage(
            current.display,
            EGL_NO_CONTEXT,
            EGL_NATIVE_BUFFER_ANDROID,
            clientBuffer,
            imageAttributes);
        if (!clientBuffer || current.eglImage == EGL_NO_IMAGE_KHR) {
            detail =
                "Could not create an EGLImage from the decoded AHardwareBuffer";
            cleanupCurrent(current, false);
            return false;
        }
        glGenTextures(1, &current.texture);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, current.texture);
        glTexParameteri(
            GL_TEXTURE_EXTERNAL_OES,
            GL_TEXTURE_MIN_FILTER,
            GL_LINEAR);
        glTexParameteri(
            GL_TEXTURE_EXTERNAL_OES,
            GL_TEXTURE_MAG_FILTER,
            GL_LINEAR);
        glTexParameteri(
            GL_TEXTURE_EXTERNAL_OES,
            GL_TEXTURE_WRAP_S,
            GL_CLAMP_TO_EDGE);
        glTexParameteri(
            GL_TEXTURE_EXTERNAL_OES,
            GL_TEXTURE_WRAP_T,
            GL_CLAMP_TO_EDGE);
        functions_.imageTargetTexture(
            GL_TEXTURE_EXTERNAL_OES,
            reinterpret_cast<GLeglImageOES>(current.eglImage));
        const GLenum glError = glGetError();
        if (!current.texture || glError != GL_NO_ERROR) {
            detail =
                "glEGLImageTargetTexture2DOES(raw AHardwareBuffer) failed: "
                + std::to_string(glError);
            cleanupCurrent(current, false);
            return false;
        }

        current.frame.texture = current.texture;
        current.frame.rawYcbcr = true;
        current.frame.bitDepth = description.format
                == AHARDWAREBUFFER_FORMAT_YCbCr_P010
            ? 10
            : 8;
        current.frame.timestampNanoseconds = timestamp;
        current.frame.generation = state_->surface.generation();
        current.frame.transform.fill(0.0F);
        current.frame.transform[0] =
            static_cast<float>(crop.right - crop.left)
            / static_cast<float>(description.width);
        // AHardwareBuffer-backed EGLImages use OpenGL's bottom-left sampling
        // origin, while AImage crop rectangles use top-left image coordinates.
        // Fold that origin conversion into the crop matrix. The subsequent
        // FBO normalization remains marked flipped for libplacebo.
        current.frame.transform[5] =
            -static_cast<float>(crop.bottom - crop.top)
            / static_cast<float>(description.height);
        current.frame.transform[10] = 1.0F;
        current.frame.transform[12] = static_cast<float>(crop.left)
            / static_cast<float>(description.width);
        current.frame.transform[13] = static_cast<float>(crop.bottom)
            / static_cast<float>(description.height);
        current.frame.transform[15] = 1.0F;

        output = current.frame;
        current_ = current;
        state_->statistics.eglImagesImported.fetch_add(
            1,
            std::memory_order_relaxed);
        state_->statistics.rawTextureBindings.fetch_add(
            1,
            std::memory_order_relaxed);
        state_->statistics.rawYcbcrImports.fetch_add(
            1,
            std::memory_order_relaxed);
        state_->statistics.lastTimestampNanoseconds.store(
            timestamp,
            std::memory_order_relaxed);
        state_->statistics.textureName.store(
            current.texture,
            std::memory_order_relaxed);
        state_->statistics.lastHardwareBufferFormat.store(
            description.format,
            std::memory_order_relaxed);
        return true;
    }

    OpenGLHardwareImportResult prepareFrame(const VideoFrame& frame)
    {
        if (!valid_) {
            return {
                OpenGLHardwareImportStatus::Error,
                {},
                error_,
            };
        }
        std::string detail;
        const OpenGLHardwareImportStatus status = queueFrame(frame, detail);
        if (status != OpenGLHardwareImportStatus::Ready) {
            return { status, {}, std::move(detail) };
        }
        PendingImage pending = takeImage(frame);
        if (!pending.image) {
            return { OpenGLHardwareImportStatus::Pending, {}, {} };
        }
        OpenGLExternalTextureFrame texture;
        if (!importImage(pending, texture, detail)) {
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                state_->lastRuntimeError = detail;
            }
            return {
                OpenGLHardwareImportStatus::Error,
                {},
                std::move(detail),
            };
        }
        return { OpenGLHardwareImportStatus::Ready, texture, {} };
    }

    void cleanupCurrent(CurrentImage& current, bool commandsSubmitted) noexcept
    {
        if (commandsSubmitted) {
            glFinish();
        }
        if (current.texture) {
            glDeleteTextures(1, &current.texture);
            current.texture = 0;
        }
        if (current.eglImage != EGL_NO_IMAGE_KHR
            && current.display != EGL_NO_DISPLAY
            && functions_.destroyImage) {
            functions_.destroyImage(current.display, current.eglImage);
            current.eglImage = EGL_NO_IMAGE_KHR;
            state_->statistics.eglImagesDestroyed.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        if (current.image) {
            if (current.acquireFence >= 0) {
                AImage_deleteAsync(
                    current.image,
                    current.acquireFence);
                current.acquireFence = -1;
            } else {
                AImage_delete(current.image);
            }
            current.image = nullptr;
        }
        current.frame = {};
    }

    bool releaseFrame(
        const OpenGLExternalTextureFrame& frame,
        std::string& detail) noexcept
    {
        std::lock_guard<std::mutex> lock(currentMutex_);
        if (!current_.image
            || current_.frame.texture != frame.texture
            || current_.frame.timestampNanoseconds
                != frame.timestampNanoseconds
            || current_.frame.generation != frame.generation) {
            detail =
                "The OpenGL AHardwareBuffer release does not match the current image";
            return false;
        }
        const EGLint attributes[] {
            EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
            EGL_NO_NATIVE_FENCE_FD_ANDROID,
            EGL_NONE,
        };
        EGLSyncKHR sync = functions_.createSync(
            current_.display,
            EGL_SYNC_NATIVE_FENCE_ANDROID,
            attributes);
        int releaseFence = -1;
        if (sync != EGL_NO_SYNC_KHR) {
            glFlush();
            releaseFence = functions_.duplicateFence(
                current_.display,
                sync);
            functions_.destroySync(current_.display, sync);
        }
        if (current_.texture) {
            glDeleteTextures(1, &current_.texture);
            current_.texture = 0;
        }
        if (current_.eglImage != EGL_NO_IMAGE_KHR) {
            functions_.destroyImage(
                current_.display,
                current_.eglImage);
            current_.eglImage = EGL_NO_IMAGE_KHR;
            state_->statistics.eglImagesDestroyed.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        if (releaseFence >= 0) {
            AImage_deleteAsync(current_.image, releaseFence);
            state_->statistics.releaseFencesReturned.fetch_add(
                1,
                std::memory_order_relaxed);
        } else {
            glFinish();
            AImage_delete(current_.image);
            state_->statistics.releaseFenceFallbacks.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        current_.image = nullptr;
        current_.frame = {};
        state_->statistics.textureName.store(
            0,
            std::memory_order_relaxed);
        return true;
    }

    void releaseCurrentContextResources() noexcept
    {
        std::lock_guard<std::mutex> lock(currentMutex_);
        if (current_.image) {
            cleanupCurrent(current_, true);
        }
        functions_ = {};
        eglDisplay_ = EGL_NO_DISPLAY;
        eglContext_ = EGL_NO_CONTEXT;
    }

    void flush() noexcept
    {
        std::deque<PendingImage> pending;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            pending.swap(state_->images);
            state_->producerEpoch.invalidate();
            state_->asyncError.clear();
        }
        for (auto& image : pending) {
            discardImage(image);
        }
    }

    std::shared_ptr<SharedState> state_;
    std::string error_;
    bool valid_ = false;
    std::mutex currentMutex_;
    EGLFunctions functions_;
    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    CurrentImage current_;
};

MediaCodecOpenGLInterop::MediaCodecOpenGLInterop(
    MediaCodecOpenGLInteropConfig config)
    : impl_(std::make_unique<Impl>(config))
{
}

MediaCodecOpenGLInterop::~MediaCodecOpenGLInterop() = default;
MediaCodecOpenGLInterop::MediaCodecOpenGLInterop(
    MediaCodecOpenGLInterop&&) noexcept = default;
MediaCodecOpenGLInterop& MediaCodecOpenGLInterop::operator=(
    MediaCodecOpenGLInterop&&) noexcept = default;

MediaCodecOpenGLInterop::operator bool() const noexcept
{
    return isValid();
}

bool MediaCodecOpenGLInterop::isValid() const noexcept
{
    return impl_ && impl_->valid_;
}

std::string MediaCodecOpenGLInterop::lastError() const
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->state_->mutex);
    return !impl_->state_->lastRuntimeError.empty()
        ? impl_->state_->lastRuntimeError
        : impl_->error_;
}

MediaCodecSurface MediaCodecOpenGLInterop::surface() const noexcept
{
    return impl_ ? impl_->state_->surface : MediaCodecSurface {};
}

HardwareInteropCapabilities MediaCodecOpenGLInterop::capabilities() const
{
    HardwareInteropCapabilities result;
    if (isValid()) {
        result.sourceDevices = { HardwareDeviceType::MediaCodec };
        result.targetDevice = HardwareDeviceType::OpenGL;
        result.zeroCopy = true;
        result.cpuFallback = false;
    }
    return result;
}

bool MediaCodecOpenGLInterop::supports(
    const HardwareFrame& frame) const noexcept
{
    return impl_ && impl_->supports(frame);
}

bool MediaCodecOpenGLInterop::queueFrame(
    const VideoFrame& frame,
    std::string& detail)
{
    if (!impl_) {
        detail = "The MediaCodec OpenGL ES interop object is empty";
        return false;
    }
    return impl_->queueFrameForPresentation(frame, detail);
}

OpenGLHardwareImportResult MediaCodecOpenGLInterop::prepareFrame(
    const VideoFrame& frame)
{
    if (!impl_) {
        return {
            OpenGLHardwareImportStatus::Error,
            {},
            "The MediaCodec OpenGL ES interop object is empty",
        };
    }
    return impl_->prepareFrame(frame);
}

bool MediaCodecOpenGLInterop::releaseFrame(
    const OpenGLExternalTextureFrame& frame,
    std::string& detail) noexcept
{
    return impl_ && impl_->releaseFrame(frame, detail);
}

void MediaCodecOpenGLInterop::releaseCurrentContextResources() noexcept
{
    if (impl_) {
        impl_->releaseCurrentContextResources();
    }
}

void MediaCodecOpenGLInterop::setFrameAvailableCallback(
    FrameAvailableCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->state_->mutex);
    impl_->state_->frameAvailable = std::move(callback);
}

void MediaCodecOpenGLInterop::flush() noexcept
{
    if (impl_) {
        impl_->flush();
    }
}

void MediaCodecOpenGLInterop::invalidatePendingFrames() noexcept
{
    flush();
}

MediaCodecOpenGLInteropStatistics
MediaCodecOpenGLInterop::statistics() const noexcept
{
    MediaCodecOpenGLInteropStatistics result;
    if (!impl_) {
        return result;
    }
    const AtomicStatistics& source = impl_->state_->statistics;
    result.codecOutputsQueued = source.codecOutputsQueued.load(
        std::memory_order_relaxed);
    result.imagesLatched = source.imagesAcquired.load(
        std::memory_order_relaxed);
    result.textureAttachments = source.eglImagesImported.load(
        std::memory_order_relaxed);
    result.textureDetaches = source.eglImagesDestroyed.load(
        std::memory_order_relaxed);
    result.textureUpdates = source.rawTextureBindings.load(
        std::memory_order_relaxed);
    result.redrawSignals = source.redrawSignals.load(
        std::memory_order_relaxed);
    result.staleFramesDropped = source.staleFramesDropped.load(
        std::memory_order_relaxed);
    result.maximumPendingFrames = source.maximumPendingFrames.load(
        std::memory_order_relaxed);
    result.lastTimestampNanoseconds =
        source.lastTimestampNanoseconds.load(std::memory_order_relaxed);
    result.textureName = source.textureName.load(std::memory_order_relaxed);
    result.hdrSamplingStatus = result.rawYcbcrImports > 0
        ? MediaCodecOpenGLHdrSamplingStatus::Supported
        : MediaCodecOpenGLHdrSamplingStatus::Unchecked;
    result.acquireFencesWaited = source.acquireFencesWaited.load(
        std::memory_order_relaxed);
    result.releaseFencesReturned = source.releaseFencesReturned.load(
        std::memory_order_relaxed);
    result.releaseFenceFallbacks = source.releaseFenceFallbacks.load(
        std::memory_order_relaxed);
    result.rawYcbcrImports = source.rawYcbcrImports.load(
        std::memory_order_relaxed);
    result.lastHardwareBufferFormat =
        source.lastHardwareBufferFormat.load(std::memory_order_relaxed);
    // The implementation contains no decoded-pixel map, software transfer,
    // CPU staging, or renderer upload operation.
    result.cpuMapCalls = 0;
    result.softwareTransferCalls = 0;
    result.stagingCopies = 0;
    result.rendererUploads = 0;
    result.lastDataSpace = 0;
    return result;
}

} // namespace qtav
