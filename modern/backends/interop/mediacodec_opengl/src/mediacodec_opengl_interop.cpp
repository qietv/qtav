// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/mediacodec_opengl_interop.h>

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/data_space.h>
#include <android/surface_texture.h>
#include <android/surface_texture_jni.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace qtav {
namespace {

constexpr std::int64_t TimestampToleranceNanoseconds = 2'000'000;
constexpr std::int64_t MaximumPresentationLagNanoseconds =
    250'000'000;

class ScopedJNIEnv final {
public:
    explicit ScopedJNIEnv(JavaVM* vm) noexcept
        : vm_(vm)
    {
        if (!vm_) {
            return;
        }
        const jint status = vm_->GetEnv(
            reinterpret_cast<void**>(&env_),
            JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            if (vm_->AttachCurrentThread(&env_, nullptr) == JNI_OK) {
                attached_ = true;
            } else {
                env_ = nullptr;
            }
        } else if (status != JNI_OK) {
            env_ = nullptr;
        }
    }

    ~ScopedJNIEnv()
    {
        if (attached_ && vm_) {
            vm_->DetachCurrentThread();
        }
    }

    JNIEnv* get() const noexcept
    {
        return env_;
    }

private:
    JavaVM* vm_ = nullptr;
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
};

bool clearJavaException(JNIEnv* env) noexcept
{
    if (!env || !env->ExceptionCheck()) {
        return false;
    }
    env->ExceptionClear();
    return true;
}

bool hasExtension(const char* requested) noexcept
{
    if (!requested || !*requested) {
        return false;
    }
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint index = 0; index < count; ++index) {
        const auto* value = reinterpret_cast<const char*>(
            glGetStringi(
                GL_EXTENSIONS,
                static_cast<GLuint>(index)));
        if (value && std::string(value) == requested) {
            return true;
        }
    }
    return false;
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
    const NativeHandle output =
        frame.hardwareFrame().nativeHandle(
            HardwareHandleType::Frame);
    result.buffer = output.value;
    result.generation = output.subresource;
    result.timestampMilliseconds = frame.timestamp();
    return result;
}

struct AtomicStatistics {
    std::atomic<std::uint64_t> codecOutputsQueued { 0 };
    std::atomic<std::uint64_t> imagesLatched { 0 };
    std::atomic<std::uint64_t> textureAttachments { 0 };
    std::atomic<std::uint64_t> textureDetaches { 0 };
    std::atomic<std::uint64_t> textureUpdates { 0 };
    std::atomic<std::uint64_t> redrawSignals { 0 };
    std::atomic<std::uint64_t> staleFramesDropped { 0 };
    std::atomic<std::uint64_t> maximumPendingFrames { 0 };
    std::atomic<std::int64_t> lastTimestampNanoseconds { 0 };
    std::atomic<std::uint32_t> textureName { 0 };
    std::atomic<int> hdrSamplingStatus {
        static_cast<int>(
            MediaCodecOpenGLHdrSamplingStatus::Disabled)
    };
    std::atomic<std::int32_t> lastDataSpace { ADATASPACE_UNKNOWN };
};

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

void convertSurfaceTextureTransformToTopLeft(
    std::array<float, 16>& transform) noexcept
{
    // SurfaceTexture's matrix expects the conventional OpenGL texture
    // coordinate origin. QtAV renderers use the same top-left source
    // coordinate convention as Vulkan and software video frames. Compose a
    // vertical origin conversion on the input side (M * flipY), preserving
    // any crop, scale, or producer rotation already present in M.
    const std::array<float, 16> original = transform;
    for (std::size_t row = 0; row < 4; ++row) {
        transform[4 + row] = -original[4 + row];
        transform[12 + row] =
            original[4 + row] + original[12 + row];
    }
}

} // namespace

class MediaCodecOpenGLInterop::Impl {
public:
    explicit Impl(MediaCodecOpenGLInteropConfig config)
        : config_(config)
    {
        config_.maximumPendingFrames = std::clamp(
            config_.maximumPendingFrames,
            2,
            16);
        config_.redrawRetryMilliseconds = std::clamp(
            config_.redrawRetryMilliseconds,
            1,
            16);
        config_.width = std::max(1, config_.width);
        config_.height = std::max(1, config_.height);
        setHdrSamplingStatus(
            config_.hdrExternalOesSamplingEnabled
                ? MediaCodecOpenGLHdrSamplingStatus::Supported
                : config_.autoDetectHdrExternalOesSampling
                    ? MediaCodecOpenGLHdrSamplingStatus::Unchecked
                    : MediaCodecOpenGLHdrSamplingStatus::Disabled);
        initialize();
    }

    ~Impl()
    {
        stopRetryWorker();
        surface_ = {};
        ScopedJNIEnv scoped(config_.javaVM);
        JNIEnv* env = scoped.get();
        if (env && surfaceTextureObject_) {
            jclass type = env->GetObjectClass(surfaceTextureObject_);
            if (type) {
                const jmethodID release =
                    env->GetMethodID(type, "release", "()V");
                if (release) {
                    env->CallVoidMethod(
                        surfaceTextureObject_,
                        release);
                    clearJavaException(env);
                } else {
                    clearJavaException(env);
                }
                env->DeleteLocalRef(type);
            } else {
                clearJavaException(env);
            }
        }
        if (surfaceTexture_) {
            ASurfaceTexture_release(surfaceTexture_);
            surfaceTexture_ = nullptr;
        }
        if (env && surfaceTextureObject_) {
            env->DeleteGlobalRef(surfaceTextureObject_);
        }
        surfaceTextureObject_ = nullptr;
    }

    void initialize()
    {
        if (!config_.javaVM) {
            error_ =
                "MediaCodec OpenGL ES interop requires the application's JavaVM";
            return;
        }
        ScopedJNIEnv scoped(config_.javaVM);
        JNIEnv* env = scoped.get();
        if (!env) {
            error_ =
                "Could not attach the MediaCodec OpenGL ES interop to the Java VM";
            return;
        }
        jclass type =
            env->FindClass("android/graphics/SurfaceTexture");
        if (!type || clearJavaException(env)) {
            if (type) {
                env->DeleteLocalRef(type);
            }
            error_ =
                "Could not resolve android.graphics.SurfaceTexture";
            return;
        }
        const jmethodID constructor =
            env->GetMethodID(type, "<init>", "(Z)V");
        const jmethodID setDefaultBufferSize =
            env->GetMethodID(type, "setDefaultBufferSize", "(II)V");
        if (!constructor || !setDefaultBufferSize
            || clearJavaException(env)) {
            env->DeleteLocalRef(type);
            error_ =
                "The Android SurfaceTexture detached constructor or buffer-size API is unavailable";
            return;
        }
        if (!config_.hdrExternalOesSamplingEnabled
            && config_.autoDetectHdrExternalOesSampling) {
            getDataSpace_ =
                env->GetMethodID(type, "getDataSpace", "()I");
            if (!getDataSpace_ || clearJavaException(env)) {
                getDataSpace_ = nullptr;
                setHdrSamplingStatus(
                    MediaCodecOpenGLHdrSamplingStatus::Unsupported);
            }
        }
        jobject local =
            env->NewObject(type, constructor, JNI_FALSE);
        if (!local || clearJavaException(env)) {
            if (local) {
                env->DeleteLocalRef(local);
            }
            env->DeleteLocalRef(type);
            error_ =
                "Could not create a detached Android SurfaceTexture";
            return;
        }
        env->CallVoidMethod(
            local,
            setDefaultBufferSize,
            config_.width,
            config_.height);
        if (clearJavaException(env)) {
            env->DeleteLocalRef(local);
            env->DeleteLocalRef(type);
            error_ =
                "SurfaceTexture rejected the decoded buffer dimensions";
            return;
        }
        surfaceTextureObject_ = env->NewGlobalRef(local);
        env->DeleteLocalRef(local);
        env->DeleteLocalRef(type);
        if (!surfaceTextureObject_
            || clearJavaException(env)) {
            error_ =
                "Could not retain the Android SurfaceTexture";
            return;
        }

        surfaceTexture_ =
            ASurfaceTexture_fromSurfaceTexture(
                env,
                surfaceTextureObject_);
        if (!surfaceTexture_) {
            error_ =
                "ASurfaceTexture_fromSurfaceTexture failed";
            return;
        }
        ANativeWindow* producer =
            ASurfaceTexture_acquireANativeWindow(
                surfaceTexture_);
        if (!producer) {
            error_ =
                "ASurfaceTexture_acquireANativeWindow failed";
            return;
        }
        surface_ = MediaCodecSurface(producer);
        ANativeWindow_release(producer);
        if (!surface_) {
            error_ =
                "Could not retain the SurfaceTexture producer window";
            return;
        }
        valid_ = true;
        retryWorker_ =
            std::thread([this] { retryLoop(); });
    }

    MediaCodecOpenGLHdrSamplingStatus hdrSamplingStatus() const noexcept
    {
        return static_cast<MediaCodecOpenGLHdrSamplingStatus>(
            statistics_.hdrSamplingStatus.load(
                std::memory_order_relaxed));
    }

    void setHdrSamplingStatus(
        MediaCodecOpenGLHdrSamplingStatus status) noexcept
    {
        statistics_.hdrSamplingStatus.store(
            static_cast<int>(status),
            std::memory_order_relaxed);
    }

    bool mayAttemptHdrSampling() const noexcept
    {
        const MediaCodecOpenGLHdrSamplingStatus status =
            hdrSamplingStatus();
        return status == MediaCodecOpenGLHdrSamplingStatus::Unchecked
            || status == MediaCodecOpenGLHdrSamplingStatus::Supported;
    }

    bool validateHdrSamplingContext(std::string& detail) noexcept
    {
        const MediaCodecOpenGLHdrSamplingStatus status =
            hdrSamplingStatus();
        if (status == MediaCodecOpenGLHdrSamplingStatus::Supported) {
            return true;
        }
        if (status != MediaCodecOpenGLHdrSamplingStatus::Unchecked) {
            detail =
                "P010/HDR SurfaceTexture sampling is unavailable because "
                "native automatic capability detection is disabled or unsupported";
            return false;
        }
        if (!getDataSpace_) {
            setHdrSamplingStatus(
                MediaCodecOpenGLHdrSamplingStatus::Unsupported);
            detail =
                "P010/HDR SurfaceTexture sampling requires Android 13 "
                "SurfaceTexture dataspace reporting";
            return false;
        }
        if (!hasExtension("GL_OES_EGL_image_external_essl3")
            || !hasExtension("GL_EXT_YUV_target")) {
            setHdrSamplingStatus(
                MediaCodecOpenGLHdrSamplingStatus::Unsupported);
            detail =
                "P010/HDR SurfaceTexture sampling requires "
                "GL_OES_EGL_image_external_essl3 and GL_EXT_YUV_target";
            return false;
        }
        return true;
    }

    bool currentDataSpace(std::int32_t& dataSpace) noexcept
    {
        if (!getDataSpace_ || !surfaceTextureObject_) {
            return false;
        }
        ScopedJNIEnv scoped(config_.javaVM);
        JNIEnv* env = scoped.get();
        if (!env) {
            return false;
        }
        const jint value = env->CallIntMethod(
            surfaceTextureObject_,
            getDataSpace_);
        if (clearJavaException(env)) {
            return false;
        }
        dataSpace = static_cast<std::int32_t>(value);
        statistics_.lastDataSpace.store(
            dataSpace,
            std::memory_order_relaxed);
        return true;
    }

    bool dataSpaceMatchesFrame(
        const VideoFrame& frame,
        std::int32_t dataSpace) const noexcept
    {
        if (dataSpace == ADATASPACE_UNKNOWN) {
            return false;
        }
        const std::int32_t standard =
            dataSpace & ADATASPACE_STANDARD_MASK;
        const std::int32_t transfer =
            dataSpace & ADATASPACE_TRANSFER_MASK;
        const VideoColorSpace color = frame.colorSpaceInfo();
        if (color.primaries == ColorPrimaries::BT2020
            && standard != ADATASPACE_STANDARD_BT2020
            && standard
                != ADATASPACE_STANDARD_BT2020_CONSTANT_LUMINANCE) {
            return false;
        }
        if (color.transfer == ColorTransfer::PQ) {
            return transfer == ADATASPACE_TRANSFER_ST2084;
        }
        if (color.transfer == ColorTransfer::HLG) {
            return transfer == ADATASPACE_TRANSFER_HLG;
        }
        // P010 can carry SDR or incomplete container metadata. In that case a
        // concrete SurfaceTexture dataspace still proves that Android and the
        // codec preserved the information needed by the external sampler.
        return frame.hardwareFrame().softwareFormat()
                == PixelFormat::P010
            && standard != ADATASPACE_STANDARD_UNSPECIFIED
            && transfer != ADATASPACE_TRANSFER_UNSPECIFIED;
    }

    void stopRetryWorker() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopRetry_ = true;
            retryRequested_ = false;
            frameAvailable_ = {};
        }
        retryChanged_.notify_all();
        if (retryWorker_.joinable()) {
            retryWorker_.join();
        }
    }

    void retryLoop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopRetry_) {
            retryChanged_.wait(lock, [this] {
                return stopRetry_ || retryRequested_;
            });
            if (stopRetry_) {
                break;
            }
            retryRequested_ = false;
            const auto delay = std::chrono::milliseconds(
                config_.redrawRetryMilliseconds);
            if (retryChanged_.wait_for(
                    lock,
                    delay,
                    [this] { return stopRetry_; })) {
                break;
            }
            FrameAvailableCallback callback = frameAvailable_;
            lock.unlock();
            if (callback) {
                statistics_.redrawSignals.fetch_add(
                    1,
                    std::memory_order_relaxed);
                callback();
            }
            lock.lock();
        }
    }

    void scheduleRetry() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopRetry_) {
                return;
            }
            retryRequested_ = true;
        }
        retryChanged_.notify_one();
    }

    bool attachTexture(std::string& error)
    {
        if (texture_ != 0) {
            return true;
        }
        if (!hasExtension(
                "GL_OES_EGL_image_external_essl3")) {
            error =
                "The current OpenGL ES context lacks GL_OES_EGL_image_external_essl3";
            return false;
        }
        while (glGetError() != GL_NO_ERROR) {
        }
        GLuint texture = 0;
        glGenTextures(1, &texture);
        if (!texture) {
            error =
                "glGenTextures failed for the SurfaceTexture external image";
            return false;
        }
        const int status =
            ASurfaceTexture_attachToGLContext(
                surfaceTexture_,
                texture);
        if (status != 0) {
            glDeleteTextures(1, &texture);
            error =
                "ASurfaceTexture_attachToGLContext failed: "
                + std::to_string(status);
            return false;
        }
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
        const GLenum glError = glGetError();
        if (glError != GL_NO_ERROR) {
            ASurfaceTexture_detachFromGLContext(surfaceTexture_);
            error =
                "Could not configure the SurfaceTexture external image: "
                + std::to_string(glError);
            return false;
        }
        texture_ = texture;
        statistics_.textureName.store(
            texture_,
            std::memory_order_relaxed);
        statistics_.textureAttachments.fetch_add(
            1,
            std::memory_order_relaxed);
        return true;
    }

    void releaseCurrentContextResources() noexcept
    {
        if (!surfaceTexture_ || texture_ == 0) {
            return;
        }
        const int status =
            ASurfaceTexture_detachFromGLContext(
                surfaceTexture_);
        if (status == 0) {
            statistics_.textureDetaches.fetch_add(
                1,
                std::memory_order_relaxed);
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            lastRuntimeError_ =
                "ASurfaceTexture_detachFromGLContext failed: "
                + std::to_string(status);
        }
        texture_ = 0;
        statistics_.textureName.store(
            0,
            std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            currentFrameKey_ = {};
            currentTexture_ = {};
        }
    }

    bool supports(const HardwareFrame& frame) const noexcept
    {
        if (!valid_ || !frame
            || frame.deviceType()
                != HardwareDeviceType::MediaCodec) {
            return false;
        }
        if (frame.softwareFormat() == PixelFormat::P010
            && !mayAttemptHdrSampling()) {
            return false;
        }
        const NativeHandle output =
            frame.nativeHandle(HardwareHandleType::Frame);
        const NativeHandle sourceSurface =
            frame.nativeHandle(HardwareHandleType::Surface);
        return output && sourceSurface
            && output.subresource == surface_.generation()
            && sourceSurface.subresource
                == surface_.generation()
            && sourceSurface.value
                == reinterpret_cast<std::uintptr_t>(
                    surface_.nativeWindow());
    }

    OpenGLHardwareImportResult prepareFrame(
        const VideoFrame& frame)
    {
        if (!valid_) {
            return {
                OpenGLHardwareImportStatus::Error,
                {},
                error_,
            };
        }
        const FrameKey key = frameKey(frame);
        if (key.buffer == 0 || key.generation == 0) {
            return {
                OpenGLHardwareImportStatus::Unsupported,
                {},
                "The frame is not a MediaCodec direct-surface output",
            };
        }
        const bool p010OrHdr =
            frame.colorSpaceInfo().isHdr()
            || frame.hardwareFrame().softwareFormat()
                == PixelFormat::P010;
        if (p010OrHdr) {
            std::string capabilityDetail;
            if (!validateHdrSamplingContext(capabilityDetail)) {
                return {
                    OpenGLHardwareImportStatus::Unsupported,
                    {},
                    std::move(capabilityDetail),
                };
            }
        }
        if (!supports(frame.hardwareFrame())) {
            return {
                OpenGLHardwareImportStatus::Stale,
                {},
                "The MediaCodec frame belongs to a stale or foreign SurfaceTexture generation",
            };
        }

        std::string attachError;
        if (!attachTexture(attachError)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                lastRuntimeError_ = attachError;
            }
            return {
                OpenGLHardwareImportStatus::Error,
                {},
                std::move(attachError),
            };
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (key == currentFrameKey_ && currentTexture_) {
                return {
                    OpenGLHardwareImportStatus::Ready,
                    currentTexture_,
                    {},
                };
            }
        }

        bool alreadyQueued = false;
        const std::int64_t expected =
            frame.timestamp() * 1'000'000LL;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingFrames_.erase(
                std::remove_if(
                    pendingFrames_.begin(),
                    pendingFrames_.end(),
                    [this, expected](const FrameKey& candidate) {
                        const bool stale =
                            candidate.timestampMilliseconds
                                    * 1'000'000LL
                                < expected
                                    - MaximumPresentationLagNanoseconds;
                        if (stale) {
                            statistics_.staleFramesDropped.fetch_add(
                                1,
                                std::memory_order_relaxed);
                        }
                        return stale;
                    }),
                pendingFrames_.end());
            alreadyQueued =
                std::find(
                    pendingFrames_.begin(),
                    pendingFrames_.end(),
                    key)
                != pendingFrames_.end();
            if (!alreadyQueued) {
                if (static_cast<int>(pendingFrames_.size())
                    >= config_.maximumPendingFrames) {
                    pendingFrames_.erase(pendingFrames_.begin());
                    statistics_.staleFramesDropped.fetch_add(
                        1,
                        std::memory_order_relaxed);
                }
                pendingFrames_.push_back(key);
                updateMaximum(
                    statistics_.maximumPendingFrames,
                    pendingFrames_.size());
            }
        }

        if (!alreadyQueued) {
            MediaCodecFrame output =
                mediaCodecFrame(frame, surface_);
            if (!output || !output.isPending()
                || !output.present()) {
                std::lock_guard<std::mutex> lock(mutex_);
                pendingFrames_.erase(
                    std::remove(
                        pendingFrames_.begin(),
                        pendingFrames_.end(),
                        key),
                    pendingFrames_.end());
                lastRuntimeError_ =
                    "Could not release the MediaCodec output into SurfaceTexture";
                return {
                    OpenGLHardwareImportStatus::Error,
                    {},
                    lastRuntimeError_,
                };
            }
            statistics_.codecOutputsQueued.fetch_add(
                1,
                std::memory_order_relaxed);
        }

        const int updateStatus =
            ASurfaceTexture_updateTexImage(surfaceTexture_);
        if (updateStatus != 0) {
            const std::string updateError =
                "ASurfaceTexture_updateTexImage failed: "
                + std::to_string(updateStatus);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                lastRuntimeError_ = updateError;
            }
            return {
                OpenGLHardwareImportStatus::Error,
                {},
                updateError,
            };
        }
        statistics_.textureUpdates.fetch_add(
            1,
            std::memory_order_relaxed);
        const std::int64_t timestamp =
            ASurfaceTexture_getTimestamp(surfaceTexture_);
        statistics_.lastTimestampNanoseconds.store(
            timestamp,
            std::memory_order_relaxed);

        bool matched = false;
        bool skipped = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool isNewImage =
                timestamp > 0
                && timestamp != ignoredTimestampNanoseconds_;
            if (timestamp > 0
                && std::llabs(timestamp - expected)
                    <= TimestampToleranceNanoseconds
                && (isNewImage
                    || ignoredTimestampNanoseconds_ == 0)) {
                matched = true;
                ignoredTimestampNanoseconds_ = timestamp;
                pendingFrames_.erase(
                    std::remove(
                        pendingFrames_.begin(),
                        pendingFrames_.end(),
                        key),
                    pendingFrames_.end());
            } else if (isNewImage
                       && timestamp
                           > expected
                               + TimestampToleranceNanoseconds) {
                skipped = true;
                ignoredTimestampNanoseconds_ = timestamp;
                pendingFrames_.erase(
                    std::remove(
                        pendingFrames_.begin(),
                        pendingFrames_.end(),
                        key),
                    pendingFrames_.end());
            }
        }
        if (skipped) {
            statistics_.staleFramesDropped.fetch_add(
                1,
                std::memory_order_relaxed);
            return {
                OpenGLHardwareImportStatus::Stale,
                {},
                "SurfaceTexture advanced past this MediaCodec output",
            };
        }
        if (!matched) {
            scheduleRetry();
            return {
                OpenGLHardwareImportStatus::Pending,
                {},
                {},
            };
        }

        if (p010OrHdr
            && hdrSamplingStatus()
                == MediaCodecOpenGLHdrSamplingStatus::Unchecked) {
            std::int32_t dataSpace = ADATASPACE_UNKNOWN;
            if (!currentDataSpace(dataSpace)
                || !dataSpaceMatchesFrame(frame, dataSpace)) {
                setHdrSamplingStatus(
                    MediaCodecOpenGLHdrSamplingStatus::Unsupported);
                return {
                    OpenGLHardwareImportStatus::Unsupported,
                    {},
                    "P010/HDR SurfaceTexture sampling reported an "
                    "unknown or incompatible dataspace "
                        + std::to_string(dataSpace),
                };
            }
            setHdrSamplingStatus(
                MediaCodecOpenGLHdrSamplingStatus::Supported);
        }

        OpenGLExternalTextureFrame texture;
        texture.texture = texture_;
        texture.timestampNanoseconds = timestamp;
        texture.generation = surface_.generation();
        ASurfaceTexture_getTransformMatrix(
            surfaceTexture_,
            texture.transform.data());
        convertSurfaceTextureTransformToTopLeft(texture.transform);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            currentFrameKey_ = key;
            currentTexture_ = texture;
        }
        statistics_.imagesLatched.fetch_add(
            1,
            std::memory_order_relaxed);
        return {
            OpenGLHardwareImportStatus::Ready,
            std::move(texture),
            {},
        };
    }

    void flush() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingFrames_.clear();
        currentFrameKey_ = {};
        currentTexture_ = {};
        retryRequested_ = false;
        lastRuntimeError_.clear();
    }

    MediaCodecOpenGLInteropConfig config_;
    ASurfaceTexture* surfaceTexture_ = nullptr;
    jobject surfaceTextureObject_ = nullptr;
    jmethodID getDataSpace_ = nullptr;
    MediaCodecSurface surface_;
    mutable std::mutex mutex_;
    std::condition_variable retryChanged_;
    std::vector<FrameKey> pendingFrames_;
    FrameKey currentFrameKey_;
    OpenGLExternalTextureFrame currentTexture_;
    FrameAvailableCallback frameAvailable_;
    std::thread retryWorker_;
    std::string error_;
    std::string lastRuntimeError_;
    std::int64_t ignoredTimestampNanoseconds_ = 0;
    GLuint texture_ = 0;
    bool retryRequested_ = false;
    bool stopRetry_ = false;
    bool valid_ = false;
    AtomicStatistics statistics_;
};

MediaCodecOpenGLInterop::MediaCodecOpenGLInterop(
    MediaCodecOpenGLInteropConfig config)
    : impl_(std::make_unique<Impl>(config))
{
}

MediaCodecOpenGLInterop::~MediaCodecOpenGLInterop() = default;
MediaCodecOpenGLInterop::MediaCodecOpenGLInterop(
    MediaCodecOpenGLInterop&&) noexcept = default;
MediaCodecOpenGLInterop&
MediaCodecOpenGLInterop::operator=(
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
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return !impl_->lastRuntimeError_.empty()
        ? impl_->lastRuntimeError_
        : impl_->error_;
}

MediaCodecSurface
MediaCodecOpenGLInterop::surface() const noexcept
{
    return impl_ ? impl_->surface_ : MediaCodecSurface {};
}

HardwareInteropCapabilities
MediaCodecOpenGLInterop::capabilities() const
{
    HardwareInteropCapabilities result;
    if (isValid()) {
        result.sourceDevices = {
            HardwareDeviceType::MediaCodec,
        };
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

OpenGLHardwareImportResult
MediaCodecOpenGLInterop::prepareFrame(
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
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->frameAvailable_ = std::move(callback);
}

void MediaCodecOpenGLInterop::flush() noexcept
{
    if (impl_) {
        impl_->flush();
    }
}

MediaCodecOpenGLInteropStatistics
MediaCodecOpenGLInterop::statistics() const noexcept
{
    MediaCodecOpenGLInteropStatistics result;
    if (!impl_) {
        return result;
    }
    const AtomicStatistics& source = impl_->statistics_;
    result.codecOutputsQueued =
        source.codecOutputsQueued.load(std::memory_order_relaxed);
    result.imagesLatched =
        source.imagesLatched.load(std::memory_order_relaxed);
    result.textureAttachments =
        source.textureAttachments.load(std::memory_order_relaxed);
    result.textureDetaches =
        source.textureDetaches.load(std::memory_order_relaxed);
    result.textureUpdates =
        source.textureUpdates.load(std::memory_order_relaxed);
    result.redrawSignals =
        source.redrawSignals.load(std::memory_order_relaxed);
    result.staleFramesDropped =
        source.staleFramesDropped.load(std::memory_order_relaxed);
    result.maximumPendingFrames =
        source.maximumPendingFrames.load(std::memory_order_relaxed);
    result.lastTimestampNanoseconds =
        source.lastTimestampNanoseconds.load(
            std::memory_order_relaxed);
    result.textureName =
        source.textureName.load(std::memory_order_relaxed);
    result.hdrSamplingStatus =
        static_cast<MediaCodecOpenGLHdrSamplingStatus>(
            source.hdrSamplingStatus.load(
                std::memory_order_relaxed));
    result.lastDataSpace =
        source.lastDataSpace.load(std::memory_order_relaxed);
    return result;
}

} // namespace qtav
