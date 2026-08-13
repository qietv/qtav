// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/ohcodec_opengl_interop.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <native_image/native_image.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace qtav {
namespace {

constexpr std::int64_t TimestampToleranceNanoseconds = 2'000'000;
constexpr std::int64_t MaximumPresentationLagNanoseconds = 250'000'000;
constexpr std::size_t MaximumRetiredFrameKeys = 64;
constexpr int MinimumPendingFrames = 1;
constexpr int MaximumPendingFrames = 16;

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

void clearGLErrors() noexcept
{
    for (int attempt = 0; attempt != 16; ++attempt) {
        if (glGetError() == GL_NO_ERROR) {
            break;
        }
    }
}

struct FrameKey {
    std::uintptr_t buffer = 0;
    std::uint32_t generation = 0;
    std::int64_t timestampMilliseconds = 0;

    bool operator==(const FrameKey& other) const noexcept
    {
        return buffer == other.buffer
            && generation == other.generation
            && timestampMilliseconds == other.timestampMilliseconds;
    }
};

FrameKey frameKey(const VideoFrame& frame) noexcept
{
    FrameKey result;
    if (!frame || !frame.hasHardwareFrame()) {
        return result;
    }
    const NativeHandle output = frame.hardwareFrame().nativeHandle(
        HardwareHandleType::Frame);
    result.buffer = output.value;
    result.generation = output.subresource;
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

int sourceBitDepth(const VideoFrame& frame) noexcept
{
    if (!frame) {
        return 0;
    }
    const PixelFormat format = frame.format();
    if (format == PixelFormat::P010
        || format == PixelFormat::YUV420P10) {
        return 10;
    }
    if (frame.hasHardwareFrame()) {
        const PixelFormat softwareFormat =
            frame.hardwareFrame().softwareFormat();
        if (softwareFormat == PixelFormat::P010
            || softwareFormat == PixelFormat::YUV420P10) {
            return 10;
        }
    }
    const VideoColorSpace color = frame.colorSpaceInfo();
    if (color.transfer == ColorTransfer::BT2020_12) {
        return 12;
    }
    if (color.isHdr()
        || color.transfer == ColorTransfer::BT2020_10
        || frame.masteringDisplayMetadata().isValid()
        || frame.contentLightMetadata().isValid()
        || frame.hasDolbyVisionMetadata()) {
        return 10;
    }
    return 8;
}

struct QueuedFrame {
    FrameKey key;
    VideoFrame frame;
};

struct AtomicStatistics {
    std::atomic<std::uint64_t> codecOutputsQueued { 0 };
    std::atomic<std::uint64_t> surfaceImagesUpdated { 0 };
    std::atomic<std::uint64_t> frameAvailableSignals { 0 };
    std::atomic<std::uint64_t> redrawSignals { 0 };
    std::atomic<std::uint64_t> transformQueries { 0 };
    std::atomic<std::uint64_t> timestampMatches { 0 };
    std::atomic<std::uint64_t> dolbyVisionFramesQueued { 0 };
    std::atomic<std::uint64_t> dolbyVisionTimestampMatches { 0 };
    std::atomic<std::uint64_t> dolbyVisionFramesReleased { 0 };
    std::atomic<std::uint64_t> microsecondTimestampsNormalized { 0 };
    std::atomic<std::uint64_t> staleFramesDropped { 0 };
    std::atomic<std::uint64_t> maximumPendingFrames { 0 };
    std::atomic<std::uint64_t> contextAttachments { 0 };
    std::atomic<std::uint64_t> contextDetaches { 0 };
    std::atomic<std::uint64_t> framesReleased { 0 };
    std::atomic<std::uint64_t> unsupportedFrames { 0 };
    std::atomic<std::uint64_t> rawYcbcrImages { 0 };
    std::atomic<std::int64_t> lastTimestampNanoseconds { 0 };
    std::atomic<std::uint32_t> textureName { 0 };
    std::atomic<std::uint32_t> surfaceGeneration { 0 };
};

} // namespace

class OHCodecOpenGLInterop::Impl final {
public:
    explicit Impl(OHCodecOpenGLInteropConfig config)
        : config_(std::move(config))
    {
        config_.maximumPendingFrames = std::clamp(
            config_.maximumPendingFrames,
            MinimumPendingFrames,
            MaximumPendingFrames);
    }

    ~Impl()
    {
        shutdown();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    bool initializeCurrentContext(std::string& detail)
    {
        std::lock_guard<std::mutex> contextLock(contextMutex_);
        const EGLDisplay display = eglGetCurrentDisplay();
        const EGLContext context = eglGetCurrentContext();
        if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT) {
            detail =
                "OH_NativeImage external-OES setup requires a current EGL context";
            setLastError(detail);
            return false;
        }
        GLint majorVersion = 0;
        clearGLErrors();
        glGetIntegerv(GL_MAJOR_VERSION, &majorVersion);
        const char* extensions = reinterpret_cast<const char*>(
            glGetString(GL_EXTENSIONS));
        const GLenum queryError = glGetError();
        if (queryError != GL_NO_ERROR || majorVersion < 3) {
            detail =
                "OH_NativeImage external-OES interop requires OpenGL ES 3.x";
            setLastError(detail);
            return false;
        }
        if (!hasExtension(
                extensions,
                "GL_OES_EGL_image_external_essl3")) {
            detail =
                "OH_NativeImage external-OES interop requires GL_OES_EGL_image_external_essl3";
            setLastError(detail);
            return false;
        }
        if (!hasExtension(extensions, "GL_EXT_YUV_target")) {
            detail =
                "OHCodec raw external-YCbCr interop requires GL_EXT_YUV_target; implicit external-OES color conversion is not accepted";
            setLastError(detail);
            return false;
        }
        if (!hasExtension(extensions, "GL_EXT_color_buffer_float")
            && !hasExtension(
                extensions,
                "GL_EXT_color_buffer_half_float")) {
            detail =
                "OHCodec raw external-YCbCr interop requires a renderable RGBA16F normalization target";
            setLastError(detail);
            return false;
        }
        if (attached_) {
            if (display_ == display && context_ == context && texture_ != 0) {
                detail.clear();
                return true;
            }
            detail =
                "OH_NativeImage is attached to another EGL context; release current-context resources before reattaching";
            setLastError(detail);
            return false;
        }

        GLuint texture = 0;
        clearGLErrors();
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
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
        const GLenum textureError = glGetError();
        if (texture == 0 || textureError != GL_NO_ERROR) {
            if (texture != 0) {
                glDeleteTextures(1, &texture);
            }
            detail = "Could not create the OH_NativeImage external-OES texture: "
                + std::to_string(textureError);
            setLastError(detail);
            return false;
        }

        if (!nativeImage_) {
            OH_NativeImage* image = OH_NativeImage_Create(
                texture,
                GL_TEXTURE_EXTERNAL_OES);
            if (!image) {
                glDeleteTextures(1, &texture);
                detail =
                    "OH_NativeImage_Create(external-OES texture) failed";
                setLastError(detail);
                return false;
            }
            OHNativeWindow* window =
                OH_NativeImage_AcquireNativeWindow(image);
            OHCodecSurface surface(window);
            if (!window || !surface) {
                OH_NativeImage_Destroy(&image);
                glDeleteTextures(1, &texture);
                detail =
                    "Could not acquire or retain the OH_NativeImage producer window";
                setLastError(detail);
                return false;
            }
            OH_OnFrameAvailableListener listener {};
            listener.context = this;
            listener.onFrameAvailable = &Impl::onFrameAvailable;
            const std::int32_t listenerResult =
                OH_NativeImage_SetOnFrameAvailableListener(
                    image,
                    listener);
            if (listenerResult != 0) {
                surface = {};
                OH_NativeImage_Destroy(&image);
                glDeleteTextures(1, &texture);
                detail =
                    "OH_NativeImage_SetOnFrameAvailableListener failed: "
                    + std::to_string(listenerResult);
                setLastError(detail);
                return false;
            }
            nativeImage_ = image;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                surface_ = std::move(surface);
            }
        } else {
            const std::int32_t attachResult =
                OH_NativeImage_AttachContext(nativeImage_, texture);
            if (attachResult != 0) {
                glDeleteTextures(1, &texture);
                detail = "OH_NativeImage_AttachContext failed: "
                    + std::to_string(attachResult);
                setLastError(detail);
                return false;
            }
        }

        display_ = display;
        context_ = context;
        texture_ = texture;
        attached_ = true;
        contextAttached_.store(true, std::memory_order_release);
        statistics_.textureName.store(
            texture_,
            std::memory_order_relaxed);
        statistics_.contextAttachments.fetch_add(
            1,
            std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            statistics_.surfaceGeneration.store(
                surface_.generation(),
                std::memory_order_relaxed);
            lastRuntimeError_.clear();
        }
        detail.clear();
        return true;
    }

    bool isValid() const noexcept
    {
        if (!contextAttached_.load(std::memory_order_acquire)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return !shuttingDown_ && nativeImage_ && surface_;
    }

    std::string lastError() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastRuntimeError_;
    }

    OHCodecSurface surface() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return surface_;
    }

    bool supports(const HardwareFrame& frame) const noexcept
    {
        if (!contextAttached_.load(std::memory_order_acquire)
            || !frame
            || frame.deviceType() != HardwareDeviceType::OHCodec) {
            return false;
        }
        const NativeHandle output = frame.nativeHandle(
            HardwareHandleType::Frame);
        const NativeHandle sourceSurface = frame.nativeHandle(
            HardwareHandleType::Surface);
        std::lock_guard<std::mutex> lock(mutex_);
        return !shuttingDown_ && surface_ && output && sourceSurface
            && output.subresource == surface_.generation()
            && sourceSurface.subresource == surface_.generation()
            && sourceSurface.value
                == reinterpret_cast<std::uintptr_t>(
                    surface_.nativeWindow());
    }

    OpenGLHardwareImportStatus queueFrame(
        const VideoFrame& frame,
        std::string& detail)
    {
        const FrameKey key = frameKey(frame);
        if (key.buffer == 0 || key.generation == 0) {
            statistics_.unsupportedFrames.fetch_add(
                1,
                std::memory_order_relaxed);
            detail =
                "The frame is not an OHCodec direct-surface output";
            return OpenGLHardwareImportStatus::Unsupported;
        }
        if (!supports(frame.hardwareFrame())) {
            detail =
                "The OHCodec frame belongs to a stale, foreign, or detached OH_NativeImage surface";
            return OpenGLHardwareImportStatus::Stale;
        }

        OHCodecSurface targetSurface;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shuttingDown_) {
                detail = "The OHCodec OpenGL interop is shutting down";
                return OpenGLHardwareImportStatus::Error;
            }
            const auto queued = std::find_if(
                queuedFrames_.begin(),
                queuedFrames_.end(),
                [&key](const QueuedFrame& pending) {
                    return pending.key == key;
                });
            if (queued != queuedFrames_.end()) {
                return OpenGLHardwareImportStatus::Pending;
            }
            if (std::find(
                    retiredFrames_.begin(),
                    retiredFrames_.end(),
                    key) != retiredFrames_.end()) {
                return OpenGLHardwareImportStatus::Stale;
            }
            if (currentKey_ == key && currentFrame_) {
                return OpenGLHardwareImportStatus::Ready;
            }
            if (static_cast<int>(queuedFrames_.size())
                >= config_.maximumPendingFrames) {
                retireFrame(queuedFrames_.begin());
                statistics_.staleFramesDropped.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
            queuedFrames_.push_back({ key, frame });
            updateMaximum(
                statistics_.maximumPendingFrames,
                queuedFrames_.size());
            targetSurface = surface_;
        }

        OHCodecFrame output = ohCodecFrame(frame, targetSurface);
        const OHCodecFrameDecisionResult decision =
            output && output.isPending()
            ? output.presentResult()
            : OHCodecFrameDecisionResult::Failed;
        const bool presented =
            decision == OHCodecFrameDecisionResult::Applied;
        if (!presented) {
            const bool alreadyDecided =
                decision == OHCodecFrameDecisionResult::AlreadyDecided;
            std::lock_guard<std::mutex> lock(mutex_);
            queuedFrames_.erase(
                std::remove_if(
                    queuedFrames_.begin(),
                    queuedFrames_.end(),
                    [&key](const QueuedFrame& pending) {
                        return pending.key == key;
                    }),
                queuedFrames_.end());
            if (alreadyDecided) {
                detail =
                    "The OHCodec output was already consumed before this redraw";
                return OpenGLHardwareImportStatus::Stale;
            }
            detail = "Could not present the OHCodec output into OH_NativeImage";
            lastRuntimeError_ = detail;
            return OpenGLHardwareImportStatus::Error;
        }
        statistics_.codecOutputsQueued.fetch_add(
            1,
            std::memory_order_relaxed);
        if (frame.hasDolbyVisionMetadata()) {
            statistics_.dolbyVisionFramesQueued.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        detail.clear();
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

    OpenGLHardwareImportResult prepareFrame(const VideoFrame& frame)
    {
        std::lock_guard<std::mutex> contextLock(contextMutex_);
        if (!attached_ || !nativeImage_ || texture_ == 0) {
            return {
                OpenGLHardwareImportStatus::Error,
                {},
                "OHCodec OpenGL interop has not been initialized on the current EGL context",
            };
        }
        if (display_ != eglGetCurrentDisplay()
            || context_ != eglGetCurrentContext()) {
            return {
                OpenGLHardwareImportStatus::Error,
                {},
                "OHCodec OpenGL frame preparation ran on a different EGL context",
            };
        }

        const FrameKey requested = frameKey(frame);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (currentFrame_) {
                if (currentKey_ == requested) {
                    return {
                        OpenGLHardwareImportStatus::Ready,
                        currentFrame_,
                        {},
                    };
                }
                return {
                    OpenGLHardwareImportStatus::Error,
                    {},
                    "The previous OH_NativeImage external texture has not been released",
                };
            }
        }

        std::string detail;
        const OpenGLHardwareImportStatus queueStatus =
            queueFrame(frame, detail);
        if (queueStatus == OpenGLHardwareImportStatus::Unsupported
            || queueStatus == OpenGLHardwareImportStatus::Stale
            || queueStatus == OpenGLHardwareImportStatus::Error) {
            return { queueStatus, {}, std::move(detail) };
        }

        const int maximumUpdates = config_.maximumPendingFrames + 1;
        for (int attempt = 0; attempt != maximumUpdates; ++attempt) {
            if (!takeFrameAvailableSignal()) {
                return {
                    OpenGLHardwareImportStatus::Pending,
                    {},
                    {},
                };
            }

            clearGLErrors();
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture_);
            const std::int32_t updateResult =
                OH_NativeImage_UpdateSurfaceImage(nativeImage_);
            if (updateResult != 0) {
                detail = "OH_NativeImage_UpdateSurfaceImage failed: "
                    + std::to_string(updateResult);
                setLastError(detail);
                return {
                    OpenGLHardwareImportStatus::Error,
                    {},
                    std::move(detail),
                };
            }
            statistics_.surfaceImagesUpdated.fetch_add(
                1,
                std::memory_order_relaxed);

            const std::int64_t observedTimestamp =
                OH_NativeImage_GetTimestamp(nativeImage_);
            if (observedTimestamp < 0) {
                detail =
                    "OH_NativeImage_GetTimestamp returned an invalid timestamp";
                setLastError(detail);
                return {
                    OpenGLHardwareImportStatus::Error,
                    {},
                    std::move(detail),
                };
            }
            std::array<float, 16> transform {};
            std::int32_t transformResult =
                OH_NativeImage_GetBufferMatrix(
                    nativeImage_,
                    transform.data());
            if (transformResult != 0) {
                transformResult = OH_NativeImage_GetTransformMatrixV2(
                    nativeImage_,
                    transform.data());
            }
            if (transformResult != 0) {
                detail =
                    "OH_NativeImage transform query failed: "
                    + std::to_string(transformResult);
                setLastError(detail);
                return {
                    OpenGLHardwareImportStatus::Error,
                    {},
                    std::move(detail),
                };
            }
            statistics_.transformQueries.fetch_add(
                1,
                std::memory_order_relaxed);
            const GLenum updateGLError = glGetError();
            if (updateGLError != GL_NO_ERROR) {
                detail =
                    "OH_NativeImage external texture update produced a GL error: "
                    + std::to_string(updateGLError);
                setLastError(detail);
                return {
                    OpenGLHardwareImportStatus::Error,
                    {},
                    std::move(detail),
                };
            }

            QueuedFrame matched;
            bool matchedRequested = false;
            bool requestedBecameStale = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto normalized =
                    normalizeTimestamp(observedTimestamp);
                const std::int64_t timestamp = normalized.nanoseconds;
                if (normalized.fromMicroseconds) {
                    statistics_.microsecondTimestampsNormalized.fetch_add(
                        1,
                        std::memory_order_relaxed);
                }
                const auto closest = closestFrame(timestamp);
                if (closest != queuedFrames_.end()) {
                    matched = *closest;
                    matchedRequested = matched.key == requested;
                    retireFrame(closest);
                    statistics_.timestampMatches.fetch_add(
                        1,
                        std::memory_order_relaxed);
                } else {
                    statistics_.staleFramesDropped.fetch_add(
                        1,
                        std::memory_order_relaxed);
                }

                const std::int64_t requestedTimestamp =
                    requested.timestampMilliseconds * 1'000'000LL;
                if (!matchedRequested
                    && timestamp
                        > requestedTimestamp
                            + MaximumPresentationLagNanoseconds) {
                    const auto iterator = std::find_if(
                        queuedFrames_.begin(),
                        queuedFrames_.end(),
                        [&requested](const QueuedFrame& pending) {
                            return pending.key == requested;
                        });
                    if (iterator != queuedFrames_.end()) {
                        retireFrame(iterator);
                        statistics_.staleFramesDropped.fetch_add(
                            1,
                            std::memory_order_relaxed);
                    }
                    requestedBecameStale = true;
                }

                if (matchedRequested) {
                    if (matched.frame.hasDolbyVisionMetadata()) {
                        statistics_.dolbyVisionTimestampMatches.fetch_add(
                            1,
                            std::memory_order_relaxed);
                    }
                    currentFrame_.texture = texture_;
                    currentFrame_.transform = transform;
                    currentFrame_.rawYcbcr = true;
                    currentFrame_.bitDepth = sourceBitDepth(matched.frame);
                    currentFrame_.timestampNanoseconds = timestamp;
                    currentFrame_.generation = requested.generation;
                    currentKey_ = requested;
                    currentSource_ = std::move(matched.frame);
                    statistics_.rawYcbcrImages.fetch_add(
                        1,
                        std::memory_order_relaxed);
                    statistics_.lastTimestampNanoseconds.store(
                        timestamp,
                        std::memory_order_relaxed);
                }
            }
            if (matchedRequested) {
                return {
                    OpenGLHardwareImportStatus::Ready,
                    currentFrame_,
                    {},
                };
            }
            if (requestedBecameStale) {
                return {
                    OpenGLHardwareImportStatus::Stale,
                    {},
                    "OH_NativeImage advanced beyond the requested OHCodec timestamp",
                };
            }
            // A coalesced listener callback may cover more than one queued
            // buffer. If another signal is available, advance again and drop
            // the unmatched image without exposing it to the renderer.
        }
        return { OpenGLHardwareImportStatus::Pending, {}, {} };
    }

    bool releaseFrame(
        const OpenGLExternalTextureFrame& frame,
        std::string& detail) noexcept
    {
        std::lock_guard<std::mutex> contextLock(contextMutex_);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!currentFrame_
            || currentFrame_.texture != frame.texture
            || currentFrame_.timestampNanoseconds
                != frame.timestampNanoseconds
            || currentFrame_.generation != frame.generation) {
            detail =
                "The OH_NativeImage release does not match the current external texture";
            return false;
        }
        glFlush();
        if (currentSource_.hasDolbyVisionMetadata()) {
            statistics_.dolbyVisionFramesReleased.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        currentFrame_ = {};
        currentKey_ = {};
        currentSource_ = {};
        statistics_.framesReleased.fetch_add(
            1,
            std::memory_order_relaxed);
        detail.clear();
        return true;
    }

    void releaseCurrentContextResources() noexcept
    {
        std::lock_guard<std::mutex> contextLock(contextMutex_);
        if (!attached_) {
            return;
        }
        const bool expectedContext = display_ == eglGetCurrentDisplay()
            && context_ == eglGetCurrentContext();
        if (!expectedContext) {
            setLastError(
                "OH_NativeImage current-context resources were released from a different EGL context");
            return;
        }
        if (currentFrame_) {
            glFinish();
        }
        const std::int32_t detachResult =
            OH_NativeImage_DetachContext(nativeImage_);
        if (detachResult == 0) {
            statistics_.contextDetaches.fetch_add(
                1,
                std::memory_order_relaxed);
        } else {
            setLastError("OH_NativeImage_DetachContext failed: "
                + std::to_string(detachResult));
        }
        if (texture_ != 0) {
            glDeleteTextures(1, &texture_);
        }
        texture_ = 0;
        display_ = EGL_NO_DISPLAY;
        context_ = EGL_NO_CONTEXT;
        attached_ = false;
        contextAttached_.store(false, std::memory_order_release);
        statistics_.textureName.store(0, std::memory_order_relaxed);
        flushLocked();
    }

    void setFrameAvailableCallback(FrameAvailableCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frameAvailable_ = std::move(callback);
    }

    void flush() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        flushAssociationsLocked();
    }

    OHCodecOpenGLInteropStatistics statistics() const noexcept
    {
        OHCodecOpenGLInteropStatistics result;
        result.codecOutputsQueued = statistics_.codecOutputsQueued.load(
            std::memory_order_relaxed);
        result.surfaceImagesUpdated =
            statistics_.surfaceImagesUpdated.load(
                std::memory_order_relaxed);
        result.frameAvailableSignals =
            statistics_.frameAvailableSignals.load(
                std::memory_order_relaxed);
        result.redrawSignals = statistics_.redrawSignals.load(
            std::memory_order_relaxed);
        result.transformQueries = statistics_.transformQueries.load(
            std::memory_order_relaxed);
        result.timestampMatches = statistics_.timestampMatches.load(
            std::memory_order_relaxed);
        result.dolbyVisionFramesQueued =
            statistics_.dolbyVisionFramesQueued.load(
                std::memory_order_relaxed);
        result.dolbyVisionTimestampMatches =
            statistics_.dolbyVisionTimestampMatches.load(
                std::memory_order_relaxed);
        result.dolbyVisionFramesReleased =
            statistics_.dolbyVisionFramesReleased.load(
                std::memory_order_relaxed);
        result.microsecondTimestampsNormalized =
            statistics_.microsecondTimestampsNormalized.load(
                std::memory_order_relaxed);
        result.staleFramesDropped =
            statistics_.staleFramesDropped.load(
                std::memory_order_relaxed);
        result.maximumPendingFrames =
            statistics_.maximumPendingFrames.load(
                std::memory_order_relaxed);
        result.contextAttachments =
            statistics_.contextAttachments.load(
                std::memory_order_relaxed);
        result.contextDetaches = statistics_.contextDetaches.load(
            std::memory_order_relaxed);
        result.framesReleased = statistics_.framesReleased.load(
            std::memory_order_relaxed);
        result.unsupportedFrames = statistics_.unsupportedFrames.load(
            std::memory_order_relaxed);
        result.rawYcbcrImages = statistics_.rawYcbcrImages.load(
            std::memory_order_relaxed);
        result.implicitRgbImages = 0;
        result.lastTimestampNanoseconds =
            statistics_.lastTimestampNanoseconds.load(
                std::memory_order_relaxed);
        result.textureName = statistics_.textureName.load(
            std::memory_order_relaxed);
        result.surfaceGeneration = statistics_.surfaceGeneration.load(
            std::memory_order_relaxed);
        // The implementation has no decoded-pixel map, software transfer,
        // CPU staging, or renderer upload operation.
        result.cpuMapCalls = 0;
        result.softwareTransferCalls = 0;
        result.stagingCopies = 0;
        result.rendererUploads = 0;
        return result;
    }

private:
    using QueuedIterator = std::deque<QueuedFrame>::iterator;

    struct NormalizedTimestamp {
        std::int64_t nanoseconds = 0;
        bool fromMicroseconds = false;
    };

    NormalizedTimestamp normalizeTimestamp(
        std::int64_t observed) const noexcept
    {
        NormalizedTimestamp result { observed, false };
        if (observed <= 0 || queuedFrames_.empty()
            || observed
                > std::numeric_limits<std::int64_t>::max() / 1'000LL) {
            return result;
        }

        const std::int64_t microsecondsAsNanoseconds =
            observed * 1'000LL;
        const auto closestDistance = [this](std::int64_t candidate) {
            std::int64_t distance =
                std::numeric_limits<std::int64_t>::max();
            for (const QueuedFrame& queued : queuedFrames_) {
                const std::int64_t expected =
                    queued.key.timestampMilliseconds * 1'000'000LL;
                const std::int64_t current = candidate >= expected
                    ? candidate - expected
                    : expected - candidate;
                distance = std::min(distance, current);
            }
            return distance;
        };
        if (closestDistance(microsecondsAsNanoseconds)
            < closestDistance(observed)) {
            result.nanoseconds = microsecondsAsNanoseconds;
            result.fromMicroseconds = true;
        }
        return result;
    }

    static void onFrameAvailable(void* context) noexcept
    {
        auto* self = static_cast<Impl*>(context);
        if (!self
            || self->shuttingDownAtomic_.load(
                std::memory_order_acquire)) {
            return;
        }
        self->pendingSignals_.fetch_add(1, std::memory_order_release);
        self->statistics_.frameAvailableSignals.fetch_add(
            1,
            std::memory_order_relaxed);
        FrameAvailableCallback callback;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (!self->shuttingDown_) {
                callback = self->frameAvailable_;
            }
        }
        if (callback) {
            self->statistics_.redrawSignals.fetch_add(
                1,
                std::memory_order_relaxed);
            callback();
        }
    }

    bool takeFrameAvailableSignal() noexcept
    {
        std::uint64_t current = pendingSignals_.load(
            std::memory_order_acquire);
        while (current != 0) {
            if (pendingSignals_.compare_exchange_weak(
                    current,
                    current - 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    QueuedIterator closestFrame(std::int64_t timestamp) noexcept
    {
        auto closest = queuedFrames_.end();
        std::int64_t closestDistance = TimestampToleranceNanoseconds + 1;
        for (auto iterator = queuedFrames_.begin();
             iterator != queuedFrames_.end();
             ++iterator) {
            const std::int64_t expected =
                iterator->key.timestampMilliseconds * 1'000'000LL;
            const std::int64_t distance = std::llabs(timestamp - expected);
            if (distance <= TimestampToleranceNanoseconds
                && distance < closestDistance) {
                closest = iterator;
                closestDistance = distance;
            }
        }
        return closest;
    }

    void retireFrame(QueuedIterator iterator)
    {
        retiredFrames_.push_back(iterator->key);
        queuedFrames_.erase(iterator);
        while (retiredFrames_.size() > MaximumRetiredFrameKeys) {
            retiredFrames_.pop_front();
        }
    }

    void setLastError(std::string detail)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastRuntimeError_ = std::move(detail);
    }

    void flushAssociationsLocked() noexcept
    {
        statistics_.staleFramesDropped.fetch_add(
            queuedFrames_.size(),
            std::memory_order_relaxed);
        queuedFrames_.clear();
        retiredFrames_.clear();
        pendingSignals_.store(0, std::memory_order_release);
    }

    void flushLocked() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        flushAssociationsLocked();
        currentFrame_ = {};
        currentKey_ = {};
        currentSource_ = {};
    }

    void shutdown() noexcept
    {
        shuttingDownAtomic_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> contextLock(contextMutex_);
        if (nativeImage_) {
            OH_NativeImage_UnsetOnFrameAvailableListener(nativeImage_);
        }
        if (attached_ && display_ == eglGetCurrentDisplay()
            && context_ == eglGetCurrentContext()) {
            if (currentFrame_) {
                glFinish();
            }
            if (OH_NativeImage_DetachContext(nativeImage_) == 0) {
                statistics_.contextDetaches.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
            if (texture_ != 0) {
                glDeleteTextures(1, &texture_);
            }
        }
        texture_ = 0;
        attached_ = false;
        contextAttached_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shuttingDown_ = true;
            frameAvailable_ = {};
            flushAssociationsLocked();
            currentFrame_ = {};
            currentKey_ = {};
            currentSource_ = {};
            // OHCodecSurface owns an extra producer-window reference. Release
            // it before OH_NativeImage destroys the original window owner.
            surface_ = {};
        }
        if (nativeImage_) {
            OH_NativeImage_Destroy(&nativeImage_);
        }
        statistics_.textureName.store(0, std::memory_order_relaxed);
        statistics_.surfaceGeneration.store(0, std::memory_order_relaxed);
    }

    OHCodecOpenGLInteropConfig config_;
    mutable std::mutex mutex_;
    std::mutex contextMutex_;
    OH_NativeImage* nativeImage_ = nullptr;
    OHCodecSurface surface_;
    std::deque<QueuedFrame> queuedFrames_;
    std::deque<FrameKey> retiredFrames_;
    FrameAvailableCallback frameAvailable_;
    std::string lastRuntimeError_;
    OpenGLExternalTextureFrame currentFrame_;
    FrameKey currentKey_;
    VideoFrame currentSource_;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    GLuint texture_ = 0;
    bool attached_ = false;
    bool shuttingDown_ = false;
    std::atomic<bool> contextAttached_ { false };
    std::atomic<bool> shuttingDownAtomic_ { false };
    std::atomic<std::uint64_t> pendingSignals_ { 0 };
    AtomicStatistics statistics_;
};

OHCodecOpenGLInterop::OHCodecOpenGLInterop(
    OHCodecOpenGLInteropConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

OHCodecOpenGLInterop::~OHCodecOpenGLInterop() = default;
OHCodecOpenGLInterop::OHCodecOpenGLInterop(
    OHCodecOpenGLInterop&&) noexcept = default;
OHCodecOpenGLInterop& OHCodecOpenGLInterop::operator=(
    OHCodecOpenGLInterop&&) noexcept = default;

OHCodecOpenGLInterop::operator bool() const noexcept
{
    return isValid();
}

bool OHCodecOpenGLInterop::isValid() const noexcept
{
    return impl_ && impl_->isValid();
}

std::string OHCodecOpenGLInterop::lastError() const
{
    return impl_ ? impl_->lastError()
                 : "The OHCodec OpenGL interop object is empty";
}

OHCodecSurface OHCodecOpenGLInterop::surface() const noexcept
{
    return impl_ ? impl_->surface() : OHCodecSurface {};
}

bool OHCodecOpenGLInterop::initializeCurrentContext(std::string& detail)
{
    if (!impl_) {
        detail = "The OHCodec OpenGL interop object is empty";
        return false;
    }
    return impl_->initializeCurrentContext(detail);
}

HardwareInteropCapabilities OHCodecOpenGLInterop::capabilities() const
{
    HardwareInteropCapabilities result;
    result.sourceDevices = { HardwareDeviceType::OHCodec };
    result.targetDevice = HardwareDeviceType::OpenGL;
    result.zeroCopy = true;
    result.cpuFallback = false;
    return result;
}

bool OHCodecOpenGLInterop::supports(
    const HardwareFrame& frame) const noexcept
{
    return impl_ && impl_->supports(frame);
}

bool OHCodecOpenGLInterop::queueFrame(
    const VideoFrame& frame,
    std::string& detail)
{
    return impl_ && impl_->queueFrameForPresentation(frame, detail);
}

OpenGLHardwareImportResult OHCodecOpenGLInterop::prepareFrame(
    const VideoFrame& frame)
{
    if (!impl_) {
        return {
            OpenGLHardwareImportStatus::Error,
            {},
            "The OHCodec OpenGL interop object is empty",
        };
    }
    return impl_->prepareFrame(frame);
}

bool OHCodecOpenGLInterop::releaseFrame(
    const OpenGLExternalTextureFrame& frame,
    std::string& detail) noexcept
{
    return impl_ && impl_->releaseFrame(frame, detail);
}

void OHCodecOpenGLInterop::releaseCurrentContextResources() noexcept
{
    if (impl_) {
        impl_->releaseCurrentContextResources();
    }
}

void OHCodecOpenGLInterop::setFrameAvailableCallback(
    FrameAvailableCallback callback)
{
    if (impl_) {
        impl_->setFrameAvailableCallback(std::move(callback));
    }
}

void OHCodecOpenGLInterop::flush() noexcept
{
    if (impl_) {
        impl_->flush();
    }
}

OHCodecOpenGLInteropStatistics
OHCodecOpenGLInterop::statistics() const noexcept
{
    return impl_ ? impl_->statistics()
                 : OHCodecOpenGLInteropStatistics {};
}

} // namespace qtav
