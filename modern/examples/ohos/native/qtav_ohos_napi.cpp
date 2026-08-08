// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ohos_vulkan_context.h"
#include "vvc_validation_session.h"

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>
#include <native_buffer/native_buffer.h>
#include <node_api.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include <qtav/mobile_video_renderer.h>
#include <qtav/ohcodec_hardware_decoder.h>
#include <qtav/ohcodec_opengl_interop.h>
#include <qtav/ohcodec_vulkan_interop.h>
#include <qtav/ohos_opengl_video_renderer.h>
#include <qtav/ohaudio_audio_sink.h>
#include <qtav/player.h>
#include <qtav/swresample_audio_converter.h>

namespace qtav::ohos_example {
namespace {

constexpr std::uint32_t LogDomain = 0xD003900;
constexpr const char* LogTag = "QtAVCoreOHOS";
constexpr const char* H264MediaPath =
    "/data/storage/el2/base/files/qtav-ohos-test-h264.mp4";
constexpr const char* HEVCMediaPath =
    "/data/storage/el2/base/files/qtav-ohos-test-hevc.mp4";
constexpr std::uint64_t RequiredInitialFallbackFrames = 20;
constexpr std::uint64_t InjectFatalAfterVulkanFrames = 12;
constexpr std::uint64_t RequiredFatalFallbackFrames = 30;
constexpr std::uint64_t RequiredH264LifecycleFrames = 12;
constexpr std::uint64_t RequiredH264RecreatedFrames = 12;
constexpr std::uint64_t RequiredHEVCFrames = 45;
constexpr std::int64_t OHCodecSeekTargetMilliseconds = 2'000;
constexpr std::uint64_t RetainedOHCodecOutputLimit = 1;
constexpr std::uint64_t RequiredOHCodecOpenGLH264Frames = 24;
constexpr std::uint64_t RequiredOHCodecOpenGLH264AfterSeekFrames = 24;
constexpr std::uint64_t RequiredOHCodecOpenGLHEVCFrames = 45;
constexpr std::uint64_t OHCodecOpenGLMaximumPendingFrames = 4;
constexpr std::size_t OHCodecOpenGLMaximumScheduledFrames = 1;
constexpr std::uint64_t RequiredOHCodecVulkanH264Frames = 30;
constexpr std::uint64_t RequiredOHCodecVulkanHEVCFrames = 30;
constexpr std::size_t OHCodecVulkanMaximumScheduledFrames = 1;
constexpr std::uint64_t InjectOHCodecFallbackAfterVulkanFrames = 8;
constexpr std::uint64_t RequiredOHCodecNativeFallbackFrames = 30;
constexpr std::uint64_t RequiredOHCodecSoftwareFallbackFrames = 30;
constexpr std::size_t OHCodecFallbackMaximumScheduledFrames = 1;

constexpr OHCodecVulkanExternalFormatProbeMode
externalFormatProbeMode() noexcept
{
#if defined(QTAV_OHOS_EXTERNAL_FORMAT_PROBE_MODE) \
    && QTAV_OHOS_EXTERNAL_FORMAT_PROBE_MODE == 1
    return OHCodecVulkanExternalFormatProbeMode::ForcedVkFormatNativeSampling;
#elif defined(QTAV_OHOS_EXTERNAL_FORMAT_PROBE_MODE) \
    && QTAV_OHOS_EXTERNAL_FORMAT_PROBE_MODE == 2
    return OHCodecVulkanExternalFormatProbeMode::ForcedVkFormatLibplacebo;
#else
    return OHCodecVulkanExternalFormatProbeMode::Disabled;
#endif
}

enum class ValidationPhase {
    InitialFallback,
    SwitchingSession,
    FatalFallback,
    SwitchingToOHCodec,
    OHCodec,
    OHCodecOpenGL,
    OHCodecVulkan,
    OHCodecFallback,
    Complete,
};

enum class OHCodecLifecyclePhase {
    Inactive,
    H264Warmup,
    H264PausePending,
    H264AfterResume,
    H264SeekPending,
    H264AfterSeek,
    H264BackgroundPending,
    H264WaitingForSurface,
    H264AfterSurfaceRecreation,
    H264ReplacementPending,
    HEVCWarmup,
    HEVCStopPending,
    Finalizing,
    Complete,
};

enum class OHCodecControlAction {
    PauseResumeH264,
    SeekH264,
    RequestBackground,
    ReplaceWithHEVC,
    StopHEVC,
    SeekOpenGLH264,
    ReplaceOpenGLWithHEVC,
    StopOpenGLHEVC,
    RenderOpenGLFrame,
    AbortOpenGL,
    ReplaceVulkanWithHEVC,
    StopVulkanHEVC,
    RenderVulkanFrame,
    AbortVulkan,
    RenderFallbackFrame,
    FinishNativeFallback,
    FinishSoftwareFallback,
    AbortFallback,
};

enum class OHCodecOpenGLPhase {
    Inactive,
    H264Warmup,
    H264SeekPending,
    H264AfterSeek,
    H264ReplacementPending,
    HEVCWarmup,
    HEVCStopPending,
    Finalizing,
    Complete,
};

enum class OHCodecVulkanPhase {
    Inactive,
    H264Warmup,
    H264ReplacementPending,
    HEVCWarmup,
    HEVCStopPending,
    Finalizing,
    Complete,
};

enum class OHCodecFallbackPhase {
    Inactive,
    NativeInterop,
    NativeInteropFinalizing,
    SoftwareDecode,
    SoftwareDecodeFinalizing,
    Complete,
};

struct ScheduledOHCodecOpenGLFrame {
    std::uint64_t serial = 0;
    VideoFrame frame;
};

struct ScheduledOHCodecVulkanFrame {
    std::uint64_t serial = 0;
    VideoFrame frame;
};

struct ScheduledOHCodecFallbackFrame {
    std::uint64_t serial = 0;
    VideoFrame frame;
};

const char* validationPhaseName(ValidationPhase phase) noexcept
{
    switch (phase) {
    case ValidationPhase::InitialFallback:
        return "initial-fallback";
    case ValidationPhase::SwitchingSession:
        return "switching-session";
    case ValidationPhase::FatalFallback:
        return "fatal-fallback";
    case ValidationPhase::SwitchingToOHCodec:
        return "switching-to-ohcodec";
    case ValidationPhase::OHCodec:
        return "ohcodec";
    case ValidationPhase::OHCodecOpenGL:
        return "ohcodec-opengl";
    case ValidationPhase::OHCodecVulkan:
        return "ohcodec-vulkan";
    case ValidationPhase::OHCodecFallback:
        return "ohcodec-fallback";
    case ValidationPhase::Complete:
        return "complete";
    }
    return "unknown";
}

const char* ohCodecFallbackPhaseName(
    OHCodecFallbackPhase phase) noexcept
{
    switch (phase) {
    case OHCodecFallbackPhase::Inactive:
        return "inactive";
    case OHCodecFallbackPhase::NativeInterop:
        return "native-interop";
    case OHCodecFallbackPhase::NativeInteropFinalizing:
        return "native-interop-finalizing";
    case OHCodecFallbackPhase::SoftwareDecode:
        return "software-decode";
    case OHCodecFallbackPhase::SoftwareDecodeFinalizing:
        return "software-decode-finalizing";
    case OHCodecFallbackPhase::Complete:
        return "complete";
    }
    return "unknown";
}

const char* ohCodecVulkanPhaseName(
    OHCodecVulkanPhase phase) noexcept
{
    switch (phase) {
    case OHCodecVulkanPhase::Inactive:
        return "inactive";
    case OHCodecVulkanPhase::H264Warmup:
        return "h264-warmup";
    case OHCodecVulkanPhase::H264ReplacementPending:
        return "h264-replacement-pending";
    case OHCodecVulkanPhase::HEVCWarmup:
        return "hevc-warmup";
    case OHCodecVulkanPhase::HEVCStopPending:
        return "hevc-stop-pending";
    case OHCodecVulkanPhase::Finalizing:
        return "finalizing";
    case OHCodecVulkanPhase::Complete:
        return "complete";
    }
    return "unknown";
}

const char* ohCodecOpenGLPhaseName(
    OHCodecOpenGLPhase phase) noexcept
{
    switch (phase) {
    case OHCodecOpenGLPhase::Inactive:
        return "inactive";
    case OHCodecOpenGLPhase::H264Warmup:
        return "h264-warmup";
    case OHCodecOpenGLPhase::H264SeekPending:
        return "h264-seek-pending";
    case OHCodecOpenGLPhase::H264AfterSeek:
        return "h264-after-seek";
    case OHCodecOpenGLPhase::H264ReplacementPending:
        return "h264-replacement-pending";
    case OHCodecOpenGLPhase::HEVCWarmup:
        return "hevc-warmup";
    case OHCodecOpenGLPhase::HEVCStopPending:
        return "hevc-stop-pending";
    case OHCodecOpenGLPhase::Finalizing:
        return "finalizing";
    case OHCodecOpenGLPhase::Complete:
        return "complete";
    }
    return "unknown";
}

const char* ohCodecLifecyclePhaseName(
    OHCodecLifecyclePhase phase) noexcept
{
    switch (phase) {
    case OHCodecLifecyclePhase::Inactive:
        return "inactive";
    case OHCodecLifecyclePhase::H264Warmup:
        return "h264-warmup";
    case OHCodecLifecyclePhase::H264PausePending:
        return "h264-pause-pending";
    case OHCodecLifecyclePhase::H264AfterResume:
        return "h264-after-resume";
    case OHCodecLifecyclePhase::H264SeekPending:
        return "h264-seek-pending";
    case OHCodecLifecyclePhase::H264AfterSeek:
        return "h264-after-seek";
    case OHCodecLifecyclePhase::H264BackgroundPending:
        return "h264-background-pending";
    case OHCodecLifecyclePhase::H264WaitingForSurface:
        return "h264-waiting-for-surface";
    case OHCodecLifecyclePhase::H264AfterSurfaceRecreation:
        return "h264-after-surface-recreation";
    case OHCodecLifecyclePhase::H264ReplacementPending:
        return "h264-replacement-pending";
    case OHCodecLifecyclePhase::HEVCWarmup:
        return "hevc-warmup";
    case OHCodecLifecyclePhase::HEVCStopPending:
        return "hevc-stop-pending";
    case OHCodecLifecyclePhase::Finalizing:
        return "finalizing";
    case OHCodecLifecyclePhase::Complete:
        return "complete";
    }
    return "unknown";
}

const char* mediaStatusName(MediaStatus status) noexcept
{
    switch (status) {
    case MediaStatus::NoMedia:
        return "NoMedia";
    case MediaStatus::Loading:
        return "Loading";
    case MediaStatus::Loaded:
        return "Loaded";
    case MediaStatus::Buffering:
        return "Buffering";
    case MediaStatus::EndOfMedia:
        return "EndOfMedia";
    case MediaStatus::Invalid:
        return "Invalid";
    }
    return "Unknown";
}

void logMessage(LogLevel level, const std::string& message)
{
    OH_LOG_Print(
        LOG_APP,
        level,
        LogDomain,
        LogTag,
        "%{public}s",
        message.c_str());
}

class FatalAfterVideoRenderer final : public VideoRenderAPI {
public:
    FatalAfterVideoRenderer(
        std::shared_ptr<VideoRenderAPI> renderer,
        std::uint64_t successfulFrameLimit)
        : renderer_(std::move(renderer))
        , successfulFrameLimit_(successfulFrameLimit)
    {
    }

    VideoRenderCapabilities capabilities() const override
    {
        return renderer_ ? renderer_->capabilities()
                         : VideoRenderCapabilities {};
    }

    void setEventCallback(EventCallback callback) override
    {
        if (renderer_) {
            renderer_->setEventCallback(std::move(callback));
        }
    }

    bool open(const VideoRenderConfig& config) override
    {
        successfulFrames_ = 0;
        return renderer_ && renderer_->open(config);
    }

    bool configure(const VideoRenderConfig& config) override
    {
        return renderer_ && renderer_->configure(config);
    }

    VideoRenderAttemptResult renderDetailed(
        const VideoFrame& frame) override
    {
        if (!renderer_) {
            return {
                VideoRenderAttemptStatus::FatalError,
                0,
                "The wrapped OHOS Vulkan renderer is unavailable",
            };
        }
        if (successfulFrames_ >= successfulFrameLimit_) {
            return {
                VideoRenderAttemptStatus::FatalError,
                0,
                "Injected OHOS Vulkan fatal failure after verified presentation",
            };
        }
        VideoRenderAttemptResult result =
            renderer_->renderDetailed(frame);
        if (result.presented()) {
            ++successfulFrames_;
        }
        return result;
    }

    bool render(const VideoFrame& frame) override
    {
        return renderDetailed(frame).frameConsumed();
    }

    void close() noexcept override
    {
        if (renderer_) {
            renderer_->close();
        }
    }

private:
    std::shared_ptr<VideoRenderAPI> renderer_;
    std::uint64_t successfulFrameLimit_ = 0;
    std::uint64_t successfulFrames_ = 0;
};

class AppSession final {
public:
    AppSession()
    {
        audioSink_ = std::make_shared<OHAudioAudioSink>();
        player_
            .setAudioFrameConverter(
                std::make_shared<SwresampleAudioConverter>())
            .setAudioSink(audioSink_)
            .onMediaStatus(
                [this](MediaStatus, MediaStatus status) {
                    {
                        std::lock_guard<std::mutex> lock(statusMutex_);
                        mediaStatus_ = status;
                        detail_ = std::string("Media status: ")
                            + mediaStatusName(status);
                    }
                    if (status == MediaStatus::Invalid) {
                        fail("QtAVCore reported invalid media");
                    }
                    return false;
                })
            .onEvent([this](const MediaEvent& event) {
                const std::string detail = event.category + ": "
                    + event.detail;
                logMessage(
                    event.error == 0 ? LOG_INFO : LOG_ERROR,
                    detail);
                if (event.error != 0) {
                    setDetail(detail);
                }
                return false;
            })
            .onVideoFrame([this](const VideoFrame&, int) {
                decodedFrames_.fetch_add(1, std::memory_order_relaxed);
            })
            .onAudioFrame([this](const AudioFrame&, int) {
                decodedAudioFrames_.fetch_add(
                    1,
                    std::memory_order_relaxed);
            });
        player_.setLoop(-1);
        controlWorker_ = std::thread([this] { controlLoop(); });
    }

    ~AppSession()
    {
        {
            std::lock_guard<std::mutex> lock(controlMutex_);
            controlQuit_ = true;
        }
        controlCondition_.notify_all();
        if (controlWorker_.joinable()) {
            controlWorker_.join();
        }
        stop();
        releaseSurface();
        player_
            .onMediaStatus({})
            .onEvent({})
            .onVideoFrame({})
            .onAudioFrame({})
            .setAudioSink({})
            .setAudioFrameConverter({});
    }

    bool startMedia(
        const std::uint8_t* h264Data,
        std::size_t h264Size,
        const std::uint8_t* hevcData,
        std::size_t hevcSize,
        const std::uint8_t* vvcData,
        std::size_t vvcSize)
    {
        if (vvcData && vvcSize > 1) {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            player_.setState(State::Stopped);
            player_
                .setRenderCallback({})
                .setVideoRenderAPI({})
                .setVideoFrameScheduler({})
                .setHardwareDecodeConfig({});
            if (selector_) {
                selector_->close();
                selector_.reset();
            }
            vulkanContext_.reset();
            vvcValidation_ =
                std::make_unique<VVCValidationSession>();
            mediaReady_ = true;
            mediaStarted_ = true;
            return vvcValidation_->start(
                vvcData,
                vvcSize,
                window_);
        }
        if (!h264Data || h264Size == 0 || !hevcData || hevcSize == 0) {
            fail("The packaged H.264 or HEVC test media is empty");
            return false;
        }
        if (!writeMediaFile(H264MediaPath, h264Data, h264Size)
            || !writeMediaFile(HEVCMediaPath, hevcData, hevcSize)) {
            fail(
                "Could not write the packaged H.264/HEVC media to app storage");
            return false;
        }

        bool shouldStart = false;
        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            mediaReady_ = true;
            shouldStart = surfaceReady_;
        }
        setDetail(
            "Packaged H.264/HEVC media ready ("
            + std::to_string(h264Size) + "/"
            + std::to_string(hevcSize) + " bytes)");
        if (shouldStart) {
            startPlayer();
        }
        return true;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            if (vvcValidation_) {
                vvcValidation_->stop();
                vvcValidation_.reset();
                mediaStarted_ = false;
                mediaReady_ = false;
                return;
            }
        }
        player_.setState(State::Stopped);
        releaseRetainedOHCodecOutput(false);
        player_
            .setVideoFrameScheduler({})
            .setHardwareDecodeConfig({});
        clearOHCodecFallbackFrames();
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        teardownOHCodecOpenGLLocked();
        teardownOHCodecVulkanLocked();
        teardownOHCodecFallbackLocked();
        mediaStarted_ = false;
        mediaReady_ = false;
    }

    void setForeground(bool foreground)
    {
        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            if (vvcValidation_) {
                vvcValidation_->setForeground(foreground);
                return;
            }
        }
        const auto lifecycle = ohCodecLifecycle_.load(
            std::memory_order_acquire);
        if (lifecycle != OHCodecLifecyclePhase::H264BackgroundPending
            && lifecycle
                != OHCodecLifecyclePhase::H264WaitingForSurface) {
            return;
        }
        if (foreground) {
            foregroundTransitions_.fetch_add(
                1,
                std::memory_order_relaxed);
            logMessage(
                LOG_INFO,
                "QTAV_OHOS_LIFECYCLE FOREGROUND_OBSERVED");
        } else {
            backgroundTransitions_.fetch_add(
                1,
                std::memory_order_relaxed);
            logMessage(
                LOG_INFO,
                "QTAV_OHOS_LIFECYCLE BACKGROUND_OBSERVED");
        }
    }

    void createSurface(
        OH_NativeXComponent* component,
        OHNativeWindow* window)
    {
        if (!component || !window) {
            fail("XComponent created without an OHNativeWindow");
            return;
        }

        std::uint64_t width = 0;
        std::uint64_t height = 0;
        const int32_t sizeResult =
            OH_NativeXComponent_GetXComponentSize(
                component,
                window,
                &width,
                &height);
        if (sizeResult != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
            fail("Could not query the XComponent surface size");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            if (vvcValidation_) {
                window_ = window;
                renderConfig_.surfaceSize = {
                    static_cast<int>(width),
                    static_cast<int>(height),
                };
                surfaceReady_ = true;
                vvcValidation_->setSurface(window);
                return;
            }
        }

        std::lock_guard<std::mutex> lock(pipelineMutex_);
        const auto ohCodecLifecycle = ohCodecLifecycle_.load(
            std::memory_order_acquire);
        const bool resumeOHCodec =
            phase_.load(std::memory_order_acquire)
                == ValidationPhase::OHCodec
            && (ohCodecLifecycle
                    == OHCodecLifecyclePhase::H264BackgroundPending
                || ohCodecLifecycle
                    == OHCodecLifecyclePhase::H264WaitingForSurface);
        const bool interruptedOHCodecOpenGL =
            phase_.load(std::memory_order_acquire)
                == ValidationPhase::OHCodecOpenGL
            && ohCodecOpenGLPhase_.load(std::memory_order_acquire)
                != OHCodecOpenGLPhase::Complete;
        const bool interruptedOHCodecFallback =
            phase_.load(std::memory_order_acquire)
                == ValidationPhase::OHCodecFallback
            && ohCodecFallbackPhase_.load(std::memory_order_acquire)
                != OHCodecFallbackPhase::Complete;
        releaseSurfaceLocked();
        if (interruptedOHCodecOpenGL) {
            failOHCodecOpenGL(
                "The XComponent surface was replaced during OHCodec/OpenGL interop validation");
            return;
        }
        if (interruptedOHCodecFallback) {
            failOHCodecFallback(
                "The XComponent surface was replaced during OHCodec selector fallback validation");
            return;
        }
        window_ = window;
        renderConfig_ = {};
        renderConfig_.surfaceSize = {
            static_cast<int>(width),
            static_cast<int>(height),
        };
        renderConfig_.aspectRatio = VideoAspectRatioMode::Fit;
        if (resumeOHCodec) {
            surfaceReady_ = true;
            if (!resumeOHCodecAfterSurfaceRecreationLocked()) {
                surfaceReady_ = false;
                window_ = nullptr;
            }
            return;
        }
        phase_.store(
            ValidationPhase::InitialFallback,
            std::memory_order_release);
        ohCodecLifecycle_.store(
            OHCodecLifecyclePhase::Inactive,
            std::memory_order_release);
        ohCodecOpenGLPhase_.store(
            OHCodecOpenGLPhase::Inactive,
            std::memory_order_release);
        ohCodecVulkanPhase_.store(
            OHCodecVulkanPhase::Inactive,
            std::memory_order_release);
        ohCodecFallbackPhase_.store(
            OHCodecFallbackPhase::Inactive,
            std::memory_order_release);
        initialFallbackFrames_.store(0, std::memory_order_relaxed);
        fatalFallbackFrames_.store(0, std::memory_order_relaxed);
        initialFallbackObserved_.store(false, std::memory_order_release);
        fatalVulkanSelected_.store(false, std::memory_order_release);
        fatalFallbackObserved_.store(false, std::memory_order_release);
        lastSelectedAPI_.store(
            MobileRenderAPI::None,
            std::memory_order_release);
        transitionQueued_.store(false, std::memory_order_release);
        ohCodecTransitionQueued_.store(false, std::memory_order_release);
        if (!createSelectorLocked(true, false)) {
            window_ = nullptr;
            return;
        }
        surfaceReady_ = true;
        player_.setVideoRenderAPI(selector_).setRenderCallback(
            [this](void*) { renderCurrentFrame(); });

        std::ostringstream detail;
        detail << "XComponent " << width << 'x' << height
               << ", selector initial API="
               << mobileRenderAPIName(
                      lastSelectedAPI_.load(std::memory_order_acquire));
        setDetail(detail.str());
        logMessage(LOG_INFO, detail.str());

        if (mediaReady_) {
            startPlayerLocked();
        }
    }

    void changeSurface(
        OH_NativeXComponent* component,
        OHNativeWindow* window)
    {
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        if (vvcValidation_) {
            window_ = window;
            return;
        }
        if (!window || window != window_) {
            return;
        }
        std::uint64_t width = 0;
        std::uint64_t height = 0;
        if (component
            && OH_NativeXComponent_GetXComponentSize(
                   component,
                   window,
                   &width,
                   &height)
                == OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
            const VideoSize previousSize = renderConfig_.surfaceSize;
            renderConfig_.surfaceSize = {
                static_cast<int>(width),
                static_cast<int>(height),
            };
            renderConfig_.aspectRatio = VideoAspectRatioMode::Fit;
            if (selector_) {
                selector_->suspendSurface();
                selector_->configure(renderConfig_);
                if (!selector_->recreateSurface()) {
                    fail(
                        "The OHOS mobile selector could not follow the XComponent resize");
                }
            } else if (ohCodecFallbackSelector_) {
                if (!ohCodecFallbackSelector_->configure(
                        renderConfig_)) {
                    failOHCodecFallback(
                        "The OHCodec fallback selector rejected the XComponent resize");
                }
            } else if (ohCodecVulkanRenderer_
                       && (previousSize.width
                               != renderConfig_.surfaceSize.width
                           || previousSize.height
                               != renderConfig_.surfaceSize.height)) {
                if (!ohCodecVulkanRenderer_->setWindow(window_)) {
                    failOHCodecVulkan(
                        "The OHCodec/Vulkan renderer could not recreate its resized XComponent surface");
                    return;
                }
                renderConfig_.surfaceSize =
                    ohCodecVulkanRenderer_->surfaceSize();
            } else if (ohCodecOpenGLRenderer_
                       && (previousSize.width
                               != renderConfig_.surfaceSize.width
                           || previousSize.height
                               != renderConfig_.surfaceSize.height)) {
                if (!ohCodecOpenGLRenderer_->setWindow(window_)) {
                    failOHCodecOpenGL(
                        "The OHCodec/OpenGL renderer could not recreate its resized XComponent surface");
                    return;
                }
                renderConfig_.surfaceSize =
                    ohCodecOpenGLRenderer_->surfaceSize();
                ohCodecOpenGLRendererGeneration_.store(
                    ohCodecOpenGLRenderer_->surfaceGeneration(),
                    std::memory_order_release);
                logMessage(
                    LOG_INFO,
                    "QTAV_OHOS_CHECKPOINT OHCODEC_OPENGL_SURFACE_RESIZED width="
                        + std::to_string(
                            renderConfig_.surfaceSize.width)
                        + " height="
                        + std::to_string(
                            renderConfig_.surfaceSize.height)
                        + " rendererGeneration="
                        + std::to_string(
                            ohCodecOpenGLRenderer_->surfaceGeneration()));
            }
        }
    }

    void releaseSurface()
    {
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        if (vvcValidation_) {
            vvcValidation_->clearSurface();
            surfaceReady_ = false;
            window_ = nullptr;
            return;
        }
        releaseSurfaceLocked();
    }

    std::string status() const
    {
        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            if (vvcValidation_) {
                return vvcValidation_->status();
            }
        }
        std::string detail;
        MediaStatus mediaStatus = MediaStatus::NoMedia;
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            detail = detail_;
            mediaStatus = mediaStatus_;
        }
        const auto playback = player_.playbackStatistics();
        std::ostringstream result;
        if (failed_.load(std::memory_order_acquire)) {
            result << "QtAVCore OHOS mobile fallback: FAIL";
        } else if (passed_.load(std::memory_order_acquire)) {
            result << "QtAVCore OHOS mobile fallback: PASS";
        } else {
            result << "QtAVCore OHOS mobile fallback: RUNNING";
        }
        result << "\nstatus=" << mediaStatusName(mediaStatus)
               << " phase="
               << validationPhaseName(
                      phase_.load(std::memory_order_acquire))
               << " ohcodecPhase="
               << ohCodecLifecyclePhaseName(
                      ohCodecLifecycle_.load(
                          std::memory_order_acquire))
               << " ohcodecOpenGLPhase="
               << ohCodecOpenGLPhaseName(
                      ohCodecOpenGLPhase_.load(
                          std::memory_order_acquire))
               << " ohcodecVulkanPhase="
               << ohCodecVulkanPhaseName(
                      ohCodecVulkanPhase_.load(
                          std::memory_order_acquire))
               << " ohcodecFallbackPhase="
               << ohCodecFallbackPhaseName(
                      ohCodecFallbackPhase_.load(
                          std::memory_order_acquire))
               << " api="
               << mobileRenderAPIName(
                      lastSelectedAPI_.load(std::memory_order_acquire))
               << " decoded="
               << decodedFrames_.load(std::memory_order_relaxed)
               << " initialGLES="
               << initialFallbackFrames_.load(std::memory_order_relaxed)
               << " fatalGLES="
               << fatalFallbackFrames_.load(std::memory_order_relaxed)
               << " ohcodec="
               << ohCodecFrames_.load(std::memory_order_relaxed)
               << " ohcodecDropped="
               << ohCodecDroppedFrames_.load(std::memory_order_relaxed)
               << " ohcodecH264="
               << h264PresentedFrames_.load(std::memory_order_relaxed)
               << " ohcodecHEVC="
               << hevcPresentedFrames_.load(std::memory_order_relaxed)
               << " ohcodecPending="
               << pendingOHCodecOutputs_.load(std::memory_order_relaxed)
               << " ohcodecPendingMax="
               << maximumPendingOHCodecOutputs_.load(
                      std::memory_order_relaxed)
               << " ohcodecOpenGLH264="
               << ohCodecOpenGLH264Rendered_.load(
                      std::memory_order_relaxed)
               << " ohcodecOpenGLHEVC="
               << ohCodecOpenGLHEVCRendered_.load(
                      std::memory_order_relaxed)
               << " ohcodecOpenGLDeferred="
               << ohCodecOpenGLDeferred_.load(
                      std::memory_order_relaxed)
               << " ohcodecOpenGLRedraw="
               << ohCodecOpenGLRedrawCallbacks_.load(
                      std::memory_order_relaxed)
               << " ohcodecOpenGLExactMax="
               << ohCodecOpenGLMaximumScheduledFrames_.load(
                      std::memory_order_relaxed)
               << " ohcodecOpenGLSchedulerDrops="
               << ohCodecOpenGLSchedulerDrops_.load(
                      std::memory_order_relaxed)
               << " ohcodecVulkanH264="
               << ohCodecVulkanH264Rendered_.load(
                      std::memory_order_relaxed)
               << " ohcodecVulkanHEVC="
               << ohCodecVulkanHEVCRendered_.load(
                      std::memory_order_relaxed)
               << " ohcodecFallbackVulkan="
               << ohCodecFallbackVulkanPresented_.load(
                      std::memory_order_relaxed)
               << " ohcodecFallbackGLES="
               << ohCodecFallbackOpenGLPresented_.load(
                      std::memory_order_relaxed)
               << " ohcodecFallbackSoftware="
               << ohCodecFallbackSoftwarePresented_.load(
                      std::memory_order_relaxed)
               << " delivered=" << playback.deliveredVideoFrames
               << " overflowDrops=" << playback.videoQueueOverflowDrops
               << " lateDrops=" << playback.lateVideoDrops;
        if (audioSink_) {
            const OHAudioStreamInfo audio = audioSink_->streamInfo();
            result << " audioDecoded="
                   << decodedAudioFrames_.load(std::memory_order_relaxed)
                   << " audioRendered=" << audio.renderedPcmFrames
                   << " audioClock="
                   << audioClockSamples_.load(std::memory_order_relaxed)
                   << " audioLatencyMs="
                   << maximumAudioLatencyMilliseconds_.load(
                          std::memory_order_relaxed)
                   << " audioStarts=" << audio.starts
                   << " audioFlushes=" << audio.flushes
                   << " audioDrains=" << audio.drains
                   << " audioRestarts=" << audio.streamRestarts;
        }
        if (!detail.empty()) {
            result << '\n' << detail;
        }
        return result.str();
    }

private:
    static bool writeMediaFile(
        const char* path,
        const std::uint8_t* data,
        std::size_t size)
    {
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        if (!output
            || !output.write(
                reinterpret_cast<const char*>(data),
                static_cast<std::streamsize>(size))) {
            return false;
        }
        output.close();
        return output.good();
    }

    bool createSelectorLocked(
        bool forceVulkanUnavailable,
        bool injectFatalVulkan)
    {
        MobileRendererSelectorConfig selectorConfig;
        selectorConfig.maximumRecoveryAttempts = 1;
        selectorConfig.vulkan =
            [this, forceVulkanUnavailable, injectFatalVulkan] {
                if (forceVulkanUnavailable) {
                    return MobileRendererCandidate {
                        {},
                        "Injected Vulkan-unavailable startup for OHOS selector validation",
                    };
                }

                vulkanContext_.reset();
                auto context = std::make_unique<OHOSVulkanContext>();
                std::string error;
                if (!context->create(window_, error)) {
                    return MobileRendererCandidate {
                        {},
                        error.empty()
                            ? "The OHOS Vulkan context is unavailable"
                            : std::move(error),
                    };
                }
                auto adapter =
                    std::make_shared<OHOSVulkanVideoRenderer>(
                        context->borrowed(),
                        VulkanOutputPreference::SdrOnly);
                if (!adapter->setWindow(window_)) {
                    return MobileRendererCandidate {
                        {},
                        "The OHOS Vulkan adapter rejected the XComponent window",
                    };
                }
                const VkSurfaceFormatKHR format =
                    adapter->surfaceFormat();
                std::ostringstream detail;
                detail << context->description() << ", VkFormat="
                       << static_cast<int>(format.format)
                       << ", colorSpace="
                       << static_cast<int>(format.colorSpace);
                vulkanContext_ = std::move(context);
                std::shared_ptr<VideoRenderAPI> renderer = adapter;
                if (injectFatalVulkan) {
                    renderer = std::make_shared<FatalAfterVideoRenderer>(
                        renderer,
                        InjectFatalAfterVulkanFrames);
                    detail << ", injectedFatalAfter="
                           << InjectFatalAfterVulkanFrames;
                }
                return MobileRendererCandidate {
                    std::move(renderer),
                    detail.str(),
                };
            };
        selectorConfig.openGLES = [this] {
            // The selector has already closed and released the previous
            // Vulkan candidate before invoking this one-way fallback factory.
            vulkanContext_.reset();
            auto adapter =
                std::make_shared<OHOSOpenGLVideoRenderer>(
                    OpenGLOutputPreference::SdrOnly);
            if (!adapter->setWindow(window_)) {
                return MobileRendererCandidate {
                    {},
                    "The OHOS EGL adapter rejected the XComponent window",
                };
            }
            const VideoSize size = adapter->surfaceSize();
            std::ostringstream detail;
            detail << "OHOS EGL/OpenGL ES 3 RGBA8/sRGB "
                   << size.width << 'x' << size.height;
            return MobileRendererCandidate {
                std::move(adapter),
                detail.str(),
            };
        };

        auto selector =
            std::make_shared<MobileVideoRendererSelector>(
                std::move(selectorConfig));
        selector->setSelectionCallback(
            [this](const MobileRendererSelectionEvent& event) {
                lastSelectedAPI_.store(
                    event.selectedAPI,
                    std::memory_order_release);
                const ValidationPhase phase =
                    phase_.load(std::memory_order_acquire);
                if (phase == ValidationPhase::InitialFallback
                    && event.type
                        == MobileRendererSelectionEventType::FellBack
                    && event.previousAPI == MobileRenderAPI::Vulkan
                    && event.selectedAPI == MobileRenderAPI::OpenGLES) {
                    initialFallbackObserved_.store(
                        true,
                        std::memory_order_release);
                }
                if (phase == ValidationPhase::FatalFallback
                    && event.type
                        == MobileRendererSelectionEventType::Selected
                    && event.selectedAPI == MobileRenderAPI::Vulkan) {
                    fatalVulkanSelected_.store(
                        true,
                        std::memory_order_release);
                }
                if (phase == ValidationPhase::FatalFallback
                    && event.type
                        == MobileRendererSelectionEventType::FellBack
                    && event.previousAPI == MobileRenderAPI::Vulkan
                    && event.selectedAPI == MobileRenderAPI::OpenGLES) {
                    fatalFallbackObserved_.store(
                        true,
                        std::memory_order_release);
                }
                const std::string detail =
                    std::string("selector ")
                    + mobileRenderAPIName(event.previousAPI) + " -> "
                    + mobileRenderAPIName(event.selectedAPI) + ": "
                    + event.detail;
                setDetail(detail);
                logMessage(
                    event.type
                            == MobileRendererSelectionEventType::Unavailable
                        ? LOG_ERROR
                        : LOG_INFO,
                    detail);
            });
        if (!selector->open(renderConfig_)) {
            const std::string error = selector->lastError();
            selector->close();
            fail(
                error.empty()
                    ? "The OHOS mobile renderer selector could not open"
                    : error);
            return false;
        }
        selector_ = std::move(selector);
        return true;
    }

    void requestFatalFallbackSession()
    {
        bool expected = false;
        if (!transitionQueued_.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
            return;
        }
        phase_.store(
            ValidationPhase::SwitchingSession,
            std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(controlMutex_);
            transitionRequested_ = true;
        }
        controlCondition_.notify_one();
    }

    void controlLoop()
    {
        for (;;) {
            bool startOHCodec = false;
            bool startFatalFallback = false;
            OHCodecControlAction ohCodecAction {};
            bool hasOHCodecAction = false;
            {
                std::unique_lock<std::mutex> lock(controlMutex_);
                controlCondition_.wait(lock, [this] {
                    return controlQuit_ || transitionRequested_
                        || ohCodecTransitionRequested_
                        || !ohCodecControlActions_.empty();
                });
                if (controlQuit_) {
                    return;
                }
                if (!ohCodecControlActions_.empty()) {
                    ohCodecAction = ohCodecControlActions_.front();
                    ohCodecControlActions_.pop_front();
                    hasOHCodecAction = true;
                } else if (ohCodecTransitionRequested_) {
                    ohCodecTransitionRequested_ = false;
                    startOHCodec = true;
                } else {
                    transitionRequested_ = false;
                    startFatalFallback = true;
                }
            }

            if (hasOHCodecAction) {
                executeOHCodecControlAction(ohCodecAction);
                continue;
            }

            std::lock_guard<std::mutex> lock(pipelineMutex_);
            if (!surfaceReady_ || !mediaStarted_
                || (startFatalFallback && !selector_)) {
                continue;
            }
            if (startOHCodec) {
                startOHCodecLocked();
                continue;
            }
            if (!startFatalFallback) {
                continue;
            }
            player_.setState(State::Paused);
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            player_.seek(250);
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            player_.setState(State::Playing);
            audioLifecycleRequested_.store(
                true,
                std::memory_order_release);
            player_.setVideoRenderAPI({});
            selector_->close();
            selector_.reset();
            vulkanContext_.reset();
            fatalFallbackFrames_.store(0, std::memory_order_relaxed);
            fatalVulkanSelected_.store(false, std::memory_order_release);
            fatalFallbackObserved_.store(false, std::memory_order_release);
            lastSelectedAPI_.store(
                MobileRenderAPI::None,
                std::memory_order_release);
            phase_.store(
                ValidationPhase::FatalFallback,
                std::memory_order_release);
            if (!createSelectorLocked(false, true)) {
                return;
            }
            player_.setVideoRenderAPI(selector_);
            setDetail(
                "Started fatal Vulkan-to-OpenGL ES fallback session without reopening media");
        }
    }

    void queueOHCodecControlAction(OHCodecControlAction action)
    {
        {
            std::lock_guard<std::mutex> lock(controlMutex_);
            ohCodecControlActions_.push_back(action);
        }
        controlCondition_.notify_one();
    }

    void requestOHCodecSession()
    {
        bool expected = false;
        if (!ohCodecTransitionQueued_.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
            return;
        }
        phase_.store(
            ValidationPhase::SwitchingToOHCodec,
            std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(controlMutex_);
            ohCodecTransitionRequested_ = true;
        }
        controlCondition_.notify_one();
    }

    void startOHCodecLocked()
    {
        player_.setState(State::Paused);
        player_
            .setRenderCallback({})
            .setVideoRenderAPI({});
        if (selector_) {
            selector_->close();
            selector_.reset();
        }
        vulkanContext_.reset();

        auto surface = std::make_unique<OHCodecSurface>(window_);
        if (!*surface) {
            fail("OHCodec could not retain the XComponent OHNativeWindow");
            return;
        }
        OHCodecHardwareDecodeOptions options;
        options.allowSoftwareFallback = false;
        options.extraHardwareFrames = 4;
        const HardwareDecodeConfig decodeConfig =
            ohCodecHardwareDecodeConfig(*surface, options);
        if (!decodeConfig.device
            || decodeConfig.deviceType != HardwareDeviceType::OHCodec
            || decodeConfig.decoderWrapper != "ohcodec") {
            fail("OHCodec hardware decode configuration is invalid");
            return;
        }

        ohCodecSurfaceGeneration_.store(
            surface->generation(),
            std::memory_order_release);
        ohCodecFrames_.store(0, std::memory_order_relaxed);
        ohCodecDroppedFrames_.store(0, std::memory_order_relaxed);
        ohCodecOutputs_.store(0, std::memory_order_relaxed);
        h264PresentedFrames_.store(0, std::memory_order_relaxed);
        h264DroppedFrames_.store(0, std::memory_order_relaxed);
        hevcPresentedFrames_.store(0, std::memory_order_relaxed);
        hevcDroppedFrames_.store(0, std::memory_order_relaxed);
        pendingOHCodecOutputs_.store(0, std::memory_order_relaxed);
        maximumPendingOHCodecOutputs_.store(
            0,
            std::memory_order_relaxed);
        retainedOHCodecOutputsReleased_.store(
            0,
            std::memory_order_relaxed);
        pauseResumeCount_.store(0, std::memory_order_relaxed);
        seekCount_.store(0, std::memory_order_relaxed);
        mediaReplacementCount_.store(0, std::memory_order_relaxed);
        explicitStopCount_.store(0, std::memory_order_relaxed);
        surfaceDestroyedCount_.store(0, std::memory_order_relaxed);
        surfaceRecreatedCount_.store(0, std::memory_order_relaxed);
        staleSurfaceRejections_.store(0, std::memory_order_relaxed);
        backgroundTransitions_.store(0, std::memory_order_relaxed);
        foregroundTransitions_.store(0, std::memory_order_relaxed);
        transitionDroppedFrames_.store(0, std::memory_order_relaxed);
        pendingOHCodecOutputsAtStop_.store(
            0,
            std::memory_order_relaxed);
        oldOHCodecSurfaceGeneration_.store(
            surface->generation(),
            std::memory_order_release);
        newOHCodecSurfaceGeneration_.store(0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(retainedOHCodecMutex_);
            retainedOHCodecOutput_.reset();
            staleOHCodecFrame_ = {};
        }
        resetOHCodecStageCounters();
        const OHCodecSurface schedulerSurface = *surface;
        ohCodecSurface_ = std::move(surface);
        phase_.store(
            ValidationPhase::OHCodec,
            std::memory_order_release);
        ohCodecLifecycle_.store(
            OHCodecLifecyclePhase::H264Warmup,
            std::memory_order_release);
        player_
            .setVideoFrameScheduler(
                [this, schedulerSurface](
                    const VideoFrame& frame,
                    int,
                    std::int64_t monotonicNanoseconds) {
                    return consumeOHCodecFrame(
                        frame,
                        monotonicNanoseconds,
                        schedulerSurface);
                })
            .setHardwareDecodeConfig(decodeConfig)
            .setState(State::Playing);
        setDetail(
            "Started required FFmpeg OHCodec H.264 session on the retained XComponent surface");
        logMessage(
            LOG_INFO,
            "QTAV_OHOS_CHECKPOINT OHCODEC_PHASE_READY codec=h264 generation="
                + std::to_string(schedulerSurface.generation()));
    }

    bool consumeOHCodecFrame(
        const VideoFrame& frame,
        std::int64_t monotonicNanoseconds,
        const OHCodecSurface& surfaceToken)
    {
        const ValidationPhase validation =
            phase_.load(std::memory_order_acquire);
        const OHCodecLifecyclePhase lifecycle =
            ohCodecLifecycle_.load(std::memory_order_acquire);
        if (!frame.hasHardwareFrame()) {
            if (validation == ValidationPhase::OHCodec) {
                fail(
                    "OHCodec lifecycle unexpectedly received a software video frame");
            }
            return false;
        }

        auto output = ohCodecFrame(frame, surfaceToken);
        if (!output) {
            if (validation == ValidationPhase::OHCodec) {
                fail(
                    "OHCodec produced an output that did not match its scheduler surface");
            }
            return false;
        }

        const std::uint64_t pending =
            pendingOHCodecOutputs_.fetch_add(
                1,
                std::memory_order_relaxed)
            + 1;
        updateMaximum(
            maximumPendingOHCodecOutputs_,
            pending);

        const auto generation =
            ohCodecSurfaceGeneration_.load(std::memory_order_acquire);
        if (validation != ValidationPhase::OHCodec
            || output.surfaceGeneration() != generation
            || !isActiveOHCodecLifecycle(lifecycle)) {
            const bool dropped = output.drop();
            finishPendingOHCodecOutput();
            if (!dropped) {
                fail("OHCodec transition output drop failed");
                return false;
            }
            transitionDroppedFrames_.fetch_add(
                1,
                std::memory_order_relaxed);
            return true;
        }

        if (isH264Lifecycle(lifecycle)) {
            std::lock_guard<std::mutex> lock(retainedOHCodecMutex_);
            staleOHCodecFrame_ = frame;
        }

        const std::uint64_t ordinal =
            ohCodecOutputs_.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::uint64_t stageOrdinal =
            ohCodecStageOutputs_.fetch_add(
                1,
                std::memory_order_relaxed)
            + 1;

        if (lifecycle == OHCodecLifecyclePhase::HEVCWarmup
            && stageOrdinal == 15) {
            std::lock_guard<std::mutex> lock(retainedOHCodecMutex_);
            if (retainedOHCodecOutput_) {
                finishPendingOHCodecOutput();
                fail("OHCodec retained-output slot was already occupied");
                return false;
            }
            retainedOHCodecOutput_ =
                std::make_unique<OHCodecFrame>(std::move(output));
            logMessage(
                LOG_INFO,
                "QTAV_OHOS_CHECKPOINT OHCODEC_OUTPUT_RETAINED codec=hevc pending=1");
            return true;
        }

        const bool shouldDrop = ordinal % 10 == 0;
        if (!(shouldDrop ? output.drop()
                         : output.presentAt(monotonicNanoseconds))) {
            finishPendingOHCodecOutput();
            fail(
                shouldDrop
                    ? "OHCodec explicit output drop failed"
                    : "OHCodec timed surface presentation failed");
            return false;
        }
        finishPendingOHCodecOutput();

        if (shouldDrop) {
            ohCodecDroppedFrames_.fetch_add(
                1,
                std::memory_order_relaxed);
            ohCodecStageDropped_.fetch_add(
                1,
                std::memory_order_relaxed);
            (isH264Lifecycle(lifecycle) ? h264DroppedFrames_
                                        : hevcDroppedFrames_)
                .fetch_add(1, std::memory_order_relaxed);
        } else {
            ohCodecFrames_.fetch_add(1, std::memory_order_relaxed);
            const std::uint64_t stagePresented =
                ohCodecStagePresented_.fetch_add(
                    1,
                    std::memory_order_relaxed)
                + 1;
            (isH264Lifecycle(lifecycle) ? h264PresentedFrames_
                                        : hevcPresentedFrames_)
                .fetch_add(1, std::memory_order_relaxed);
            maybeAdvanceOHCodecLifecycle(
                lifecycle,
                stagePresented,
                stageOrdinal);
        }
        return true;
    }

    static bool isH264Lifecycle(
        OHCodecLifecyclePhase phase) noexcept
    {
        return phase == OHCodecLifecyclePhase::H264Warmup
            || phase == OHCodecLifecyclePhase::H264AfterResume
            || phase == OHCodecLifecyclePhase::H264AfterSeek
            || phase
                == OHCodecLifecyclePhase::H264AfterSurfaceRecreation;
    }

    static bool isActiveOHCodecLifecycle(
        OHCodecLifecyclePhase phase) noexcept
    {
        return isH264Lifecycle(phase)
            || phase == OHCodecLifecyclePhase::HEVCWarmup;
    }

    static void updateMaximum(
        std::atomic<std::uint64_t>& maximum,
        std::uint64_t value) noexcept
    {
        auto previous = maximum.load(std::memory_order_relaxed);
        while (previous < value
               && !maximum.compare_exchange_weak(
                   previous,
                   value,
                   std::memory_order_relaxed)) {
        }
    }

    void finishPendingOHCodecOutput() noexcept
    {
        const auto previous = pendingOHCodecOutputs_.fetch_sub(
            1,
            std::memory_order_relaxed);
        if (previous == 0) {
            pendingOHCodecOutputs_.store(0, std::memory_order_relaxed);
        }
    }

    void resetOHCodecStageCounters() noexcept
    {
        ohCodecStageOutputs_.store(0, std::memory_order_relaxed);
        ohCodecStagePresented_.store(0, std::memory_order_relaxed);
        ohCodecStageDropped_.store(0, std::memory_order_relaxed);
    }

    void maybeAdvanceOHCodecLifecycle(
        OHCodecLifecyclePhase lifecycle,
        std::uint64_t stagePresented,
        std::uint64_t stageOutputs)
    {
        OHCodecLifecyclePhase next = lifecycle;
        OHCodecControlAction action {};
        bool advance = false;
        switch (lifecycle) {
        case OHCodecLifecyclePhase::H264Warmup:
            if (stagePresented >= RequiredH264LifecycleFrames) {
                next = OHCodecLifecyclePhase::H264PausePending;
                action = OHCodecControlAction::PauseResumeH264;
                advance = true;
            }
            break;
        case OHCodecLifecyclePhase::H264AfterResume:
            if (stagePresented >= RequiredH264LifecycleFrames) {
                next = OHCodecLifecyclePhase::H264SeekPending;
                action = OHCodecControlAction::SeekH264;
                advance = true;
            }
            break;
        case OHCodecLifecyclePhase::H264AfterSeek:
            if (stagePresented >= RequiredH264LifecycleFrames) {
                next = OHCodecLifecyclePhase::H264BackgroundPending;
                action = OHCodecControlAction::RequestBackground;
                advance = true;
            }
            break;
        case OHCodecLifecyclePhase::H264AfterSurfaceRecreation:
            if (stagePresented >= RequiredH264RecreatedFrames) {
                next = OHCodecLifecyclePhase::H264ReplacementPending;
                action = OHCodecControlAction::ReplaceWithHEVC;
                advance = true;
            }
            break;
        case OHCodecLifecyclePhase::HEVCWarmup:
            if (stageOutputs >= RequiredHEVCFrames) {
                next = OHCodecLifecyclePhase::HEVCStopPending;
                action = OHCodecControlAction::StopHEVC;
                advance = true;
            }
            break;
        default:
            break;
        }

        if (!advance) {
            return;
        }
        auto expected = lifecycle;
        if (ohCodecLifecycle_.compare_exchange_strong(
                expected,
                next,
                std::memory_order_acq_rel)) {
            queueOHCodecControlAction(action);
        }
    }

    void executeOHCodecControlAction(OHCodecControlAction action)
    {
        if (failed_.load(std::memory_order_acquire)
            && action != OHCodecControlAction::AbortOpenGL
            && action != OHCodecControlAction::AbortVulkan
            && action != OHCodecControlAction::AbortFallback) {
            return;
        }

        switch (action) {
        case OHCodecControlAction::PauseResumeH264: {
            player_.setState(State::Paused);
            if (!player_.waitFor(State::Paused, 3'000)) {
                fail("OHCodec H.264 pause did not complete");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            const auto pausedOutputs =
                ohCodecOutputs_.load(std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(160));
            if (ohCodecOutputs_.load(std::memory_order_relaxed)
                != pausedOutputs) {
                fail("OHCodec produced output while H.264 was paused");
                return;
            }
            resetOHCodecStageCounters();
            ohCodecLifecycle_.store(
                OHCodecLifecyclePhase::H264AfterResume,
                std::memory_order_release);
            pauseResumeCount_.fetch_add(1, std::memory_order_relaxed);
            player_.setState(State::Playing);
            logMessage(
                LOG_INFO,
                "QTAV_OHOS_CHECKPOINT OHCODEC_PAUSE_RESUME codec=h264 paused=1 resumed=1");
            break;
        }
        case OHCodecControlAction::SeekH264: {
            auto completed = std::make_shared<std::atomic<bool>>(false);
            auto callbackPosition =
                std::make_shared<std::atomic<std::int64_t>>(-1);
            if (!player_.seek(
                    OHCodecSeekTargetMilliseconds,
                    SeekFlag::FromStart,
                    [completed, callbackPosition](std::int64_t position) {
                        callbackPosition->store(
                            position,
                            std::memory_order_release);
                        completed->store(true, std::memory_order_release);
                    })) {
                fail("OHCodec H.264 seek request was rejected");
                return;
            }
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::seconds(4);
            while (!completed->load(std::memory_order_acquire)
                   && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10));
            }
            if (!completed->load(std::memory_order_acquire)
                || callbackPosition->load(std::memory_order_acquire) < 0) {
                fail("OHCodec H.264 seek did not complete");
                return;
            }
            resetOHCodecStageCounters();
            ohCodecLifecycle_.store(
                OHCodecLifecyclePhase::H264AfterSeek,
                std::memory_order_release);
            seekCount_.fetch_add(1, std::memory_order_relaxed);
            logMessage(
                LOG_INFO,
                "QTAV_OHOS_CHECKPOINT OHCODEC_SEEK codec=h264 targetMs="
                    + std::to_string(OHCodecSeekTargetMilliseconds)
                    + " callbackMs="
                    + std::to_string(
                        callbackPosition->load(
                            std::memory_order_acquire)));
            break;
        }
        case OHCodecControlAction::RequestBackground:
            player_.setState(State::Paused);
            if (!player_.waitFor(State::Paused, 3'000)) {
                fail(
                    "OHCodec H.264 did not pause before background transition");
                return;
            }
            logMessage(
                LOG_INFO,
                "QTAV_OHOS_LIFECYCLE BACKGROUND_REQUEST generation="
                    + std::to_string(
                        ohCodecSurfaceGeneration_.load(
                            std::memory_order_acquire)));
            setDetail(
                "OHCodec H.264 lifecycle is waiting for a real background/foreground surface replacement");
            break;
        case OHCodecControlAction::ReplaceWithHEVC: {
            {
                std::lock_guard<std::mutex> lock(pipelineMutex_);
                if (!surfaceReady_ || !ohCodecSurface_) {
                    fail(
                        "OHCodec HEVC replacement has no active recreated surface");
                    return;
                }
            }
            mediaReplacementCount_.fetch_add(
                1,
                std::memory_order_relaxed);
            mediaOpenCount_.fetch_add(1, std::memory_order_relaxed);
            player_.setState(State::Paused);
            if (!player_.waitFor(State::Paused, 3'000)) {
                fail("OHCodec H.264 did not pause before HEVC replacement");
                return;
            }
            player_.setMedia(HEVCMediaPath);
            resetOHCodecStageCounters();
            ohCodecLifecycle_.store(
                OHCodecLifecyclePhase::HEVCWarmup,
                std::memory_order_release);
            player_.setState(State::Playing);
            logMessage(
                LOG_INFO,
                "QTAV_OHOS_CHECKPOINT OHCODEC_MEDIA_REPLACED from=h264 to=hevc count=1");
            break;
        }
        case OHCodecControlAction::StopHEVC:
            finalizeOHCodecLifecycle();
            break;
        case OHCodecControlAction::SeekOpenGLH264:
            seekOHCodecOpenGLH264();
            break;
        case OHCodecControlAction::ReplaceOpenGLWithHEVC:
            replaceOHCodecOpenGLWithHEVC();
            break;
        case OHCodecControlAction::StopOpenGLHEVC:
            finalizeOHCodecOpenGLLifecycle();
            break;
        case OHCodecControlAction::RenderOpenGLFrame:
            ohCodecOpenGLRenderQueued_.store(
                false,
                std::memory_order_release);
            renderScheduledOHCodecOpenGLFrame();
            break;
        case OHCodecControlAction::AbortOpenGL:
            abortOHCodecOpenGLValidation();
            break;
        case OHCodecControlAction::ReplaceVulkanWithHEVC:
            replaceOHCodecVulkanWithHEVC();
            break;
        case OHCodecControlAction::StopVulkanHEVC:
            finalizeOHCodecVulkanLifecycle();
            break;
        case OHCodecControlAction::RenderVulkanFrame:
            ohCodecVulkanRenderQueued_.store(
                false,
                std::memory_order_release);
            renderScheduledOHCodecVulkanFrame();
            break;
        case OHCodecControlAction::AbortVulkan:
            abortOHCodecVulkanValidation();
            break;
        case OHCodecControlAction::RenderFallbackFrame:
            ohCodecFallbackRenderQueued_.store(
                false,
                std::memory_order_release);
            renderScheduledOHCodecFallbackFrame();
            break;
        case OHCodecControlAction::FinishNativeFallback:
            finalizeOHCodecNativeFallback();
            break;
        case OHCodecControlAction::FinishSoftwareFallback:
            finalizeOHCodecSoftwareFallback();
            break;
        case OHCodecControlAction::AbortFallback:
            abortOHCodecFallbackValidation();
            break;
        }
    }

    bool resumeOHCodecAfterSurfaceRecreationLocked()
    {
        if (!window_) {
            fail("OHCodec surface recreation has no OHNativeWindow");
            return false;
        }
        auto surface = std::make_unique<OHCodecSurface>(window_);
        if (!*surface) {
            fail("OHCodec could not retain the recreated OHNativeWindow");
            return false;
        }
        const auto oldGeneration =
            oldOHCodecSurfaceGeneration_.load(std::memory_order_acquire);
        if (oldGeneration == 0 || surface->generation() == oldGeneration) {
            fail("OHCodec surface generation did not change after recreation");
            return false;
        }

        VideoFrame staleFrame;
        {
            std::lock_guard<std::mutex> lock(retainedOHCodecMutex_);
            staleFrame = staleOHCodecFrame_;
        }
        if (!staleFrame || ohCodecFrame(staleFrame, *surface)) {
            fail("OHCodec accepted a stale frame against the recreated surface");
            return false;
        }
        staleSurfaceRejections_.fetch_add(1, std::memory_order_relaxed);

        OHCodecHardwareDecodeOptions options;
        options.allowSoftwareFallback = false;
        options.extraHardwareFrames = 4;
        const HardwareDecodeConfig decodeConfig =
            ohCodecHardwareDecodeConfig(*surface, options);
        if (!decodeConfig.device
            || decodeConfig.deviceType != HardwareDeviceType::OHCodec
            || decodeConfig.decoderWrapper != "ohcodec") {
            fail("OHCodec recreated-surface configuration is invalid");
            return false;
        }

        const auto newGeneration = surface->generation();
        const OHCodecSurface schedulerSurface = *surface;
        ohCodecSurface_ = std::move(surface);
        ohCodecSurfaceGeneration_.store(
            newGeneration,
            std::memory_order_release);
        newOHCodecSurfaceGeneration_.store(
            newGeneration,
            std::memory_order_release);
        surfaceRecreatedCount_.fetch_add(1, std::memory_order_relaxed);
        resetOHCodecStageCounters();
        ohCodecLifecycle_.store(
            OHCodecLifecyclePhase::H264AfterSurfaceRecreation,
            std::memory_order_release);
        player_
            .setVideoFrameScheduler(
                [this, schedulerSurface](
                    const VideoFrame& frame,
                    int,
                    std::int64_t monotonicNanoseconds) {
                    return consumeOHCodecFrame(
                        frame,
                        monotonicNanoseconds,
                        schedulerSurface);
                })
            .setHardwareDecodeConfig(decodeConfig)
            .setState(State::Playing);

        logMessage(
            LOG_INFO,
            "QTAV_OHOS_CHECKPOINT OHCODEC_SURFACE_RECREATED oldGeneration="
                + std::to_string(oldGeneration)
                + " newGeneration=" + std::to_string(newGeneration));
        logMessage(
            LOG_INFO,
            "QTAV_OHOS_CHECKPOINT OHCODEC_STALE_SURFACE_REJECTED oldGeneration="
                + std::to_string(oldGeneration)
                + " activeGeneration=" + std::to_string(newGeneration));
        setDetail(
            "OHCodec H.264 resumed on recreated surface generation "
            + std::to_string(newGeneration));
        return true;
    }

    bool releaseRetainedOHCodecOutput(bool afterStop)
    {
        std::unique_ptr<OHCodecFrame> retained;
        {
            std::lock_guard<std::mutex> lock(retainedOHCodecMutex_);
            retained = std::move(retainedOHCodecOutput_);
        }
        if (!retained) {
            return true;
        }
        const bool dropped = retained->drop();
        finishPendingOHCodecOutput();
        if (dropped) {
            retainedOHCodecOutputsReleased_.fetch_add(
                1,
                std::memory_order_relaxed);
            hevcDroppedFrames_.fetch_add(1, std::memory_order_relaxed);
            ohCodecDroppedFrames_.fetch_add(1, std::memory_order_relaxed);
            if (afterStop) {
                logMessage(
                    LOG_INFO,
                    "QTAV_OHOS_CHECKPOINT OHCODEC_RETAINED_RELEASED_AFTER_STOP pending=0");
            }
        }
        return dropped;
    }

    void finalizeOHCodecLifecycle()
    {
        ohCodecLifecycle_.store(
            OHCodecLifecyclePhase::Finalizing,
            std::memory_order_release);
        player_.setState(State::Stopped);
        if (!player_.waitFor(State::Stopped, 4'000)) {
            fail("OHCodec HEVC explicit stop did not complete");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        const auto stoppedOutputs =
            ohCodecOutputs_.load(std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(160));
        if (ohCodecOutputs_.load(std::memory_order_relaxed)
            != stoppedOutputs) {
            fail("OHCodec produced output after HEVC reached Stopped");
            return;
        }

        const auto pendingAtStop =
            pendingOHCodecOutputs_.load(std::memory_order_relaxed);
        pendingOHCodecOutputsAtStop_.store(
            pendingAtStop,
            std::memory_order_relaxed);
        if (pendingAtStop != RetainedOHCodecOutputLimit
            || !releaseRetainedOHCodecOutput(true)) {
            fail(
                "OHCodec retained output did not survive until explicit stop");
            return;
        }
        explicitStopCount_.fetch_add(1, std::memory_order_relaxed);
        player_
            .setVideoFrameScheduler({})
            .setHardwareDecodeConfig({});
        {
            std::lock_guard<std::mutex> lock(retainedOHCodecMutex_);
            // The retained H.264 frame has completed its stale-generation
            // assertion. Keeping it alive here would also keep that decoder's
            // direct XComponent producer connection alive, preventing EGL
            // from becoming the producer for the following interop phase.
            staleOHCodecFrame_ = {};
        }
        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            ohCodecSurface_.reset();
            ohCodecSurfaceGeneration_.store(
                0,
                std::memory_order_release);
        }

        const PlaybackStatistics playback = player_.playbackStatistics();
        const bool lifecyclePassed =
            h264PresentedFrames_.load(std::memory_order_relaxed)
                >= RequiredH264LifecycleFrames * 3
                    + RequiredH264RecreatedFrames
            && hevcPresentedFrames_.load(std::memory_order_relaxed) > 0
            && pauseResumeCount_.load(std::memory_order_relaxed) == 1
            && seekCount_.load(std::memory_order_relaxed) == 1
            && mediaReplacementCount_.load(std::memory_order_relaxed) == 1
            && explicitStopCount_.load(std::memory_order_relaxed) == 1
            && backgroundTransitions_.load(std::memory_order_relaxed) >= 1
            && foregroundTransitions_.load(std::memory_order_relaxed) >= 1
            && surfaceDestroyedCount_.load(std::memory_order_relaxed) == 1
            && surfaceRecreatedCount_.load(std::memory_order_relaxed) == 1
            && staleSurfaceRejections_.load(std::memory_order_relaxed) == 1
            && maximumPendingOHCodecOutputs_.load(
                   std::memory_order_relaxed)
                == RetainedOHCodecOutputLimit + 1
            && pendingAtStop == RetainedOHCodecOutputLimit
            && pendingOHCodecOutputs_.load(std::memory_order_relaxed) == 0
            && retainedOHCodecOutputsReleased_.load(
                   std::memory_order_relaxed)
                == RetainedOHCodecOutputLimit
            && playback.maximumQueuedVideoFrames == 0
            && playback.videoQueueOverflowDrops == 0
            && playback.lateVideoDrops == 0
            && mediaOpenCount_.load(std::memory_order_relaxed) == 2
            && player_.state() == State::Stopped;
        if (!lifecyclePassed) {
            fail("OHCodec lifecycle counters did not satisfy the required matrix");
            return;
        }

        ohCodecLifecycle_.store(
            OHCodecLifecyclePhase::Complete,
            std::memory_order_release);
        const std::string ohCodecResult =
            "QTAV_OHOS_OHCODEC_RESULT PASS codecs=h264,hevc"
            " h264Presented="
            + std::to_string(
                h264PresentedFrames_.load(std::memory_order_relaxed))
            + " h264Dropped="
            + std::to_string(
                h264DroppedFrames_.load(std::memory_order_relaxed))
            + " hevcPresented="
            + std::to_string(
                hevcPresentedFrames_.load(std::memory_order_relaxed))
            + " hevcDropped="
            + std::to_string(
                hevcDroppedFrames_.load(std::memory_order_relaxed))
            + " pauseResume=1 seek=1 mediaReplacement=1 stop=1"
              " background="
            + std::to_string(
                backgroundTransitions_.load(std::memory_order_relaxed))
            + " foreground="
            + std::to_string(
                foregroundTransitions_.load(std::memory_order_relaxed))
            + " surfaceRecreation=1 staleRejected=1 maxPending="
            + std::to_string(
                maximumPendingOHCodecOutputs_.load(
                    std::memory_order_relaxed))
            + " pendingAtStop=" + std::to_string(pendingAtStop)
            + " pendingEnd=0 maxQueued="
            + std::to_string(playback.maximumQueuedVideoFrames)
            + " oldGeneration="
            + std::to_string(
                oldOHCodecSurfaceGeneration_.load(
                    std::memory_order_relaxed))
            + " newGeneration="
            + std::to_string(
                newOHCodecSurfaceGeneration_.load(
                    std::memory_order_relaxed));
        logMessage(LOG_INFO, ohCodecResult);
        setDetail(
            "OHCodec direct-surface lifecycle passed; starting OHCodec/OpenGL external-OES validation");
        startOHCodecOpenGLPhase();
    }

    static bool isActiveOHCodecOpenGLPhase(
        OHCodecOpenGLPhase phase) noexcept
    {
        return phase == OHCodecOpenGLPhase::H264Warmup
            || phase == OHCodecOpenGLPhase::H264AfterSeek
            || phase == OHCodecOpenGLPhase::HEVCWarmup;
    }

    void startOHCodecOpenGLPhase()
    {
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        if (!surfaceReady_ || !window_) {
            fail("OHCodec/OpenGL interop has no active XComponent surface");
            return;
        }

        OHCodecOpenGLInteropConfig interopConfig;
        interopConfig.maximumPendingFrames =
            static_cast<int>(OHCodecOpenGLMaximumPendingFrames);
        auto interop = std::make_shared<OHCodecOpenGLInterop>(
            interopConfig);

        auto renderer = std::make_shared<OHOSOpenGLVideoRenderer>(
            OpenGLOutputPreference::SdrOnly);
        renderer->setEventCallback(
            [this](const VideoRenderEvent& event) {
                handleOHCodecOpenGLRendererEvent(event);
            });
        renderer->setHardwareFrameInterop(interop);
        if (!renderer->setWindow(window_)) {
            fail(
                "Could not bind the OHCodec/OpenGL renderer to the XComponent window");
            return;
        }

        VideoRenderConfig interopRenderConfig = renderConfig_;
        interopRenderConfig.surfaceSize = renderer->surfaceSize();
        interopRenderConfig.aspectRatio = VideoAspectRatioMode::Fit;
        if (!interopRenderConfig.surfaceSize.isValid()
            || !renderer->open(interopRenderConfig)) {
            fail(
                "Could not open the OHCodec/OpenGL external-OES renderer");
            return;
        }

        bool advertisesOHCodec = false;
        for (const HardwareDeviceType type :
             renderer->capabilities().hardwareDevices) {
            if (type == HardwareDeviceType::OHCodec) {
                advertisesOHCodec = true;
                break;
            }
        }
        const OHCodecSurface surface = interop->surface();
        OHCodecHardwareDecodeOptions options;
        options.allowSoftwareFallback = false;
        options.extraHardwareFrames = 6;
        const HardwareDecodeConfig decodeConfig =
            ohCodecHardwareDecodeConfig(surface, options);
        if (!*interop || !advertisesOHCodec || !surface
            || !decodeConfig.device
            || decodeConfig.deviceType != HardwareDeviceType::OHCodec
            || decodeConfig.decoderWrapper != "ohcodec"
            || decodeConfig.surfaceGeneration != surface.generation()) {
            renderer->setEventCallback({});
            renderer->close();
            fail(
                "OHCodec/OpenGL did not publish a valid OH_NativeImage decode surface");
            return;
        }

        ohCodecOpenGLH264Rendered_.store(0, std::memory_order_relaxed);
        ohCodecOpenGLHEVCRendered_.store(0, std::memory_order_relaxed);
        ohCodecOpenGLStageRendered_.store(0, std::memory_order_relaxed);
        ohCodecOpenGLDeferred_.store(0, std::memory_order_relaxed);
        ohCodecOpenGLRedrawCallbacks_.store(
            0,
            std::memory_order_relaxed);
        ohCodecOpenGLSeekCount_.store(0, std::memory_order_relaxed);
        ohCodecOpenGLFlushCount_.store(0, std::memory_order_relaxed);
        ohCodecOpenGLMediaReplacementCount_.store(
            0,
            std::memory_order_relaxed);
        ohCodecOpenGLStopCount_.store(0, std::memory_order_relaxed);
        ohCodecOpenGLH264Statistics_ = {};
        ohCodecOpenGLAbortQueued_.store(false, std::memory_order_release);
        ohCodecOpenGLRenderQueued_.store(false, std::memory_order_release);
        ohCodecOpenGLNextFrameSerial_.store(1, std::memory_order_relaxed);
        ohCodecOpenGLMaximumScheduledFrames_.store(
            0,
            std::memory_order_relaxed);
        ohCodecOpenGLSchedulerDrops_.store(0, std::memory_order_relaxed);
        ohCodecOpenGLRendererDiscards_.store(
            0,
            std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> queueLock(
                ohCodecOpenGLQueueMutex_);
            ohCodecOpenGLScheduledFrames_.clear();
        }
        ohCodecOpenGLSurfaceGeneration_.store(
            surface.generation(),
            std::memory_order_release);
        ohCodecOpenGLRendererGeneration_.store(
            renderer->surfaceGeneration(),
            std::memory_order_release);
        ohCodecOpenGLInterop_ = interop;
        ohCodecOpenGLRenderer_ = renderer;
        renderConfig_ = interopRenderConfig;
        phase_.store(
            ValidationPhase::OHCodecOpenGL,
            std::memory_order_release);
        ohCodecOpenGLPhase_.store(
            OHCodecOpenGLPhase::H264Warmup,
            std::memory_order_release);

        player_
            .setRenderCallback({})
            .setVideoRenderAPI({})
            .setVideoFrameScheduler(
                [this, interop, surface](
                    const VideoFrame& frame,
                    int,
                    std::int64_t) {
                    return scheduleOHCodecOpenGLFrame(
                        frame,
                        interop,
                        surface);
                })
            .setHardwareDecodeConfig(decodeConfig);
        mediaOpenCount_.fetch_add(1, std::memory_order_relaxed);
        player_.setMedia(H264MediaPath);
        player_.setState(State::Playing);

        const std::string detail =
            "QTAV_OHOS_CHECKPOINT OHCODEC_OPENGL_PHASE_READY codec=h264 generation="
            + std::to_string(surface.generation())
            + " rendererGeneration="
            + std::to_string(renderer->surfaceGeneration())
            + " producer=oh_nativeimage texture=external-oes maxPending="
            + std::to_string(OHCodecOpenGLMaximumPendingFrames)
            + " exactInFlight="
            + std::to_string(OHCodecOpenGLMaximumScheduledFrames);
        setDetail(detail);
        logMessage(LOG_INFO, detail);
    }

    void handleOHCodecOpenGLRendererEvent(
        const VideoRenderEvent& event)
    {
        if (event.type == VideoRenderEventType::RedrawRequested) {
            ohCodecOpenGLRedrawCallbacks_.fetch_add(
                1,
                std::memory_order_relaxed);
            // OH_NativeImage invokes this listener on a platform callback
            // thread. Only publish a control action here; UpdateSurfaceImage,
            // timestamp queries, and rendering stay on the control worker.
            requestOHCodecOpenGLRender();
            return;
        }
        if (phase_.load(std::memory_order_acquire)
            != ValidationPhase::OHCodecOpenGL) {
            return;
        }
        failOHCodecOpenGL(
            std::string(
                event.type == VideoRenderEventType::SurfaceLost
                    ? "OHCodec/OpenGL surface lost: "
                    : "OHCodec/OpenGL renderer error: ")
            + (event.detail.empty() ? "no detail" : event.detail));
    }

    void requestOHCodecOpenGLRender()
    {
        if (failed_.load(std::memory_order_acquire)
            || phase_.load(std::memory_order_acquire)
                != ValidationPhase::OHCodecOpenGL
            || !isActiveOHCodecOpenGLPhase(
                ohCodecOpenGLPhase_.load(
                    std::memory_order_acquire))) {
            return;
        }

        bool expected = false;
        if (ohCodecOpenGLRenderQueued_.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
            queueOHCodecControlAction(
                OHCodecControlAction::RenderOpenGLFrame);
        }
    }

    bool dropUnscheduledOHCodecOpenGLFrame(
        const VideoFrame& frame,
        const OHCodecSurface& surface,
        const char* reason)
    {
        OHCodecFrame output = ohCodecFrame(frame, surface);
        const bool dropped = output && output.drop();
        if (!dropped) {
            failOHCodecOpenGL(
                std::string("Could not drop an OHCodec/OpenGL ")
                + reason + " output with its single decision token");
            return true;
        }
        ohCodecOpenGLSchedulerDrops_.fetch_add(
            1,
            std::memory_order_relaxed);
        return true;
    }

    bool scheduleOHCodecOpenGLFrame(
        const VideoFrame& frame,
        const std::shared_ptr<OHCodecOpenGLInterop>& interop,
        const OHCodecSurface& surface)
    {
        if (!frame.hasHardwareFrame()) {
            failOHCodecOpenGL(
                "OHCodec/OpenGL scheduler received a software video frame");
            return true;
        }
        if (!interop || !surface) {
            failOHCodecOpenGL(
                "OHCodec/OpenGL scheduler lost its retained interop surface");
            return true;
        }

        const auto shouldDrop = [this, &frame, &surface](
                                    const char* reason) {
            return dropUnscheduledOHCodecOpenGLFrame(
                frame,
                surface,
                reason);
        };
        if (failed_.load(std::memory_order_acquire)
            || phase_.load(std::memory_order_acquire)
                != ValidationPhase::OHCodecOpenGL
            || !isActiveOHCodecOpenGLPhase(
                ohCodecOpenGLPhase_.load(
                    std::memory_order_acquire))) {
            return shouldDrop("inactive-phase");
        }

        {
            std::unique_lock<std::mutex> queueLock(
                ohCodecOpenGLQueueMutex_);
            if (failed_.load(std::memory_order_acquire)
                || phase_.load(std::memory_order_acquire)
                    != ValidationPhase::OHCodecOpenGL
                || !isActiveOHCodecOpenGLPhase(
                    ohCodecOpenGLPhase_.load(
                        std::memory_order_acquire))) {
                queueLock.unlock();
                return shouldDrop("phase-transition");
            }
            if (ohCodecOpenGLScheduledFrames_.size()
                >= OHCodecOpenGLMaximumScheduledFrames) {
                queueLock.unlock();
                return shouldDrop("queue-full");
            }

            const std::uint64_t serial =
                ohCodecOpenGLNextFrameSerial_.fetch_add(
                    1,
                    std::memory_order_relaxed);
            ohCodecOpenGLScheduledFrames_.push_back(
                { serial, frame });
            updateMaximum(
                ohCodecOpenGLMaximumScheduledFrames_,
                ohCodecOpenGLScheduledFrames_.size());

            std::string detail;
            if (!interop->queueFrame(frame, detail)) {
                if (!ohCodecOpenGLScheduledFrames_.empty()
                    && ohCodecOpenGLScheduledFrames_.back().serial
                        == serial) {
                    ohCodecOpenGLScheduledFrames_.pop_back();
                }
                queueLock.unlock();
                failOHCodecOpenGL(
                    detail.empty()
                        ? "OHCodec/OpenGL could not queue the exact scheduler frame"
                        : detail);
                // queueFrame owns the output decision attempt. Always consume
                // this callback so the same hardware token cannot enter the
                // Player current-frame path as a second owner.
                return true;
            }
        }

        // The NativeImage listener normally publishes this action. Also
        // publish once after queueFrame() so a coalesced callback cannot leave
        // the retained exact frame idle; a premature attempt simply defers.
        requestOHCodecOpenGLRender();
        return true;
    }

    void renderScheduledOHCodecOpenGLFrame()
    {
        if (phase_.load(std::memory_order_acquire)
                != ValidationPhase::OHCodecOpenGL
            || failed_.load(std::memory_order_acquire)) {
            return;
        }

        VideoRenderAttemptResult result;
        ScheduledOHCodecOpenGLFrame scheduledFrame;
        bool hasAnotherFrame = false;
        bool countedPresentation = false;
        {
            std::lock_guard<std::mutex> queueLock(
                ohCodecOpenGLQueueMutex_);
            if (ohCodecOpenGLScheduledFrames_.empty()) {
                return;
            }
            scheduledFrame = ohCodecOpenGLScheduledFrames_.front();
        }

        {
            // Serialize native rendering against surface teardown. The copied
            // entry retains the same exact frame while the decode worker can
            // observe the occupied queue and immediately drop newer outputs.
            std::lock_guard<std::mutex> pipelineLock(pipelineMutex_);
            if (!ohCodecOpenGLRenderer_) {
                return;
            }
            result = ohCodecOpenGLRenderer_->renderDetailed(
                scheduledFrame.frame);
        }

        if (result.status == VideoRenderAttemptStatus::Presented
            || result.status == VideoRenderAttemptStatus::Discarded) {
            std::lock_guard<std::mutex> queueLock(
                ohCodecOpenGLQueueMutex_);
            if (!ohCodecOpenGLScheduledFrames_.empty()
                && ohCodecOpenGLScheduledFrames_.front().serial
                    == scheduledFrame.serial) {
                if (result.status
                    == VideoRenderAttemptStatus::Presented) {
                    const OHCodecOpenGLPhase phase =
                        ohCodecOpenGLPhase_.load(
                            std::memory_order_acquire);
                    if (isActiveOHCodecOpenGLPhase(phase)) {
                        const std::uint64_t stage =
                            ohCodecOpenGLStageRendered_.fetch_add(
                                1,
                                std::memory_order_relaxed)
                            + 1;
                        if (phase == OHCodecOpenGLPhase::HEVCWarmup) {
                            ohCodecOpenGLHEVCRendered_.fetch_add(
                                1,
                                std::memory_order_relaxed);
                        } else {
                            ohCodecOpenGLH264Rendered_.fetch_add(
                                1,
                                std::memory_order_relaxed);
                        }
                        maybeAdvanceOHCodecOpenGLLifecycle(
                            phase,
                            stage);
                        countedPresentation = true;
                    }
                }
                if (result.status == VideoRenderAttemptStatus::Presented
                    || result.status
                        == VideoRenderAttemptStatus::Discarded) {
                    ohCodecOpenGLScheduledFrames_.pop_front();
                }
                hasAnotherFrame =
                    !ohCodecOpenGLScheduledFrames_.empty();
            }
        }

        switch (result.status) {
        case VideoRenderAttemptStatus::Presented:
            if (countedPresentation && hasAnotherFrame) {
                requestOHCodecOpenGLRender();
            }
            break;
        case VideoRenderAttemptStatus::DeferredUntilRedraw:
            ohCodecOpenGLDeferred_.fetch_add(
                1,
                std::memory_order_relaxed);
            break;
        case VideoRenderAttemptStatus::Discarded:
            ohCodecOpenGLRendererDiscards_.fetch_add(
                1,
                std::memory_order_relaxed);
            if (hasAnotherFrame) {
                requestOHCodecOpenGLRender();
            }
            break;
        case VideoRenderAttemptStatus::RetryAfterBackoff:
            failOHCodecOpenGL(
                "OHCodec/OpenGL unexpectedly requested timer backoff");
            break;
        case VideoRenderAttemptStatus::SurfaceLost:
        case VideoRenderAttemptStatus::FatalError:
            failOHCodecOpenGL(
                result.detail.empty()
                    ? "OHCodec/OpenGL render attempt failed"
                    : result.detail);
            break;
        }
    }

    void maybeAdvanceOHCodecOpenGLLifecycle(
        OHCodecOpenGLPhase phase,
        std::uint64_t stageRendered)
    {
        OHCodecOpenGLPhase next = phase;
        OHCodecControlAction action {};
        bool advance = false;
        switch (phase) {
        case OHCodecOpenGLPhase::H264Warmup:
            if (stageRendered >= RequiredOHCodecOpenGLH264Frames) {
                next = OHCodecOpenGLPhase::H264SeekPending;
                action = OHCodecControlAction::SeekOpenGLH264;
                advance = true;
            }
            break;
        case OHCodecOpenGLPhase::H264AfterSeek:
            if (stageRendered
                >= RequiredOHCodecOpenGLH264AfterSeekFrames) {
                next = OHCodecOpenGLPhase::H264ReplacementPending;
                action = OHCodecControlAction::ReplaceOpenGLWithHEVC;
                advance = true;
            }
            break;
        case OHCodecOpenGLPhase::HEVCWarmup:
            if (stageRendered >= RequiredOHCodecOpenGLHEVCFrames) {
                next = OHCodecOpenGLPhase::HEVCStopPending;
                action = OHCodecControlAction::StopOpenGLHEVC;
                advance = true;
            }
            break;
        default:
            break;
        }
        if (!advance) {
            return;
        }
        auto expected = phase;
        if (ohCodecOpenGLPhase_.compare_exchange_strong(
                expected,
                next,
                std::memory_order_acq_rel)) {
            queueOHCodecControlAction(action);
        }
    }

    std::shared_ptr<OHCodecOpenGLInterop> activeOHCodecOpenGLInterop()
    {
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        return surfaceReady_ ? ohCodecOpenGLInterop_ : nullptr;
    }

    void clearAndFlushOHCodecOpenGLFrames(
        const std::shared_ptr<OHCodecOpenGLInterop>& interop)
    {
        std::lock_guard<std::mutex> queueLock(
            ohCodecOpenGLQueueMutex_);
        // Every queued frame has already made its one present decision in
        // interop->queueFrame(). Releasing our retained references and then
        // flushing the interop retires those associations without issuing a
        // second OHCodec decision. Queue-full outputs are dropped separately
        // before they ever enter either queue.
        ohCodecOpenGLScheduledFrames_.clear();
        if (interop) {
            interop->flush();
        }
        ohCodecOpenGLRenderQueued_.store(
            false,
            std::memory_order_release);
    }

    void seekOHCodecOpenGLH264()
    {
        auto interop = activeOHCodecOpenGLInterop();
        if (!interop) {
            failOHCodecOpenGL(
                "OHCodec/OpenGL H.264 seek has no active interop");
            return;
        }
        player_.setState(State::Paused);
        if (!player_.waitFor(State::Paused, 3'000)) {
            failOHCodecOpenGL(
                "OHCodec/OpenGL H.264 did not pause before seek");
            return;
        }
        clearAndFlushOHCodecOpenGLFrames(interop);
        ohCodecOpenGLFlushCount_.fetch_add(
            1,
            std::memory_order_relaxed);

        auto completed = std::make_shared<std::atomic<bool>>(false);
        auto callbackPosition =
            std::make_shared<std::atomic<std::int64_t>>(-1);
        if (!player_.seek(
                OHCodecSeekTargetMilliseconds,
                SeekFlag::FromStart,
                [completed, callbackPosition](std::int64_t position) {
                    callbackPosition->store(
                        position,
                        std::memory_order_release);
                    completed->store(true, std::memory_order_release);
                })) {
            failOHCodecOpenGL(
                "OHCodec/OpenGL H.264 seek request was rejected");
            return;
        }
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(4);
        while (!completed->load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!completed->load(std::memory_order_acquire)
            || callbackPosition->load(std::memory_order_acquire) < 0) {
            failOHCodecOpenGL(
                "OHCodec/OpenGL H.264 seek did not complete");
            return;
        }
        ohCodecOpenGLStageRendered_.store(0, std::memory_order_relaxed);
        ohCodecOpenGLSeekCount_.fetch_add(1, std::memory_order_relaxed);
        ohCodecOpenGLPhase_.store(
            OHCodecOpenGLPhase::H264AfterSeek,
            std::memory_order_release);
        player_.setState(State::Playing);
        logMessage(
            LOG_INFO,
            "QTAV_OHOS_CHECKPOINT OHCODEC_OPENGL_SEEK codec=h264 targetMs="
                + std::to_string(OHCodecSeekTargetMilliseconds)
                + " callbackMs="
                + std::to_string(
                    callbackPosition->load(std::memory_order_acquire))
                + " flush=1");
    }

    void replaceOHCodecOpenGLWithHEVC()
    {
        auto interop = activeOHCodecOpenGLInterop();
        if (!interop) {
            failOHCodecOpenGL(
                "OHCodec/OpenGL HEVC replacement has no active interop");
            return;
        }
        player_.setState(State::Paused);
        if (!player_.waitFor(State::Paused, 3'000)) {
            failOHCodecOpenGL(
                "OHCodec/OpenGL H.264 did not pause before HEVC replacement");
            return;
        }
        const auto statistics = interop->statistics();
        const std::uint64_t h264Rendered =
            ohCodecOpenGLH264Rendered_.load(std::memory_order_relaxed);
        if (statistics.surfaceGeneration == 0
            || statistics.surfaceGeneration
                != ohCodecOpenGLSurfaceGeneration_.load(
                    std::memory_order_acquire)
            || statistics.codecOutputsQueued < h264Rendered
            || statistics.surfaceImagesUpdated < h264Rendered
            || statistics.frameAvailableSignals < h264Rendered
            || statistics.redrawSignals < h264Rendered
            || statistics.transformQueries < h264Rendered
            || statistics.timestampMatches < h264Rendered
            || statistics.dolbyVisionFramesQueued != 0
            || statistics.dolbyVisionTimestampMatches != 0
            || statistics.dolbyVisionFramesReleased != 0
            || statistics.framesReleased < h264Rendered
            || statistics.maximumPendingFrames == 0
            || statistics.maximumPendingFrames
                > OHCodecOpenGLMaximumPendingFrames
            || statistics.unsupportedFrames != 0
            || statistics.cpuMapCalls != 0
            || statistics.softwareTransferCalls != 0
            || statistics.stagingCopies != 0
            || statistics.rendererUploads != 0) {
            failOHCodecOpenGL(
                "OHCodec/OpenGL H.264 statistics failed before HEVC replacement");
            return;
        }
        ohCodecOpenGLH264Statistics_ = statistics;
        clearAndFlushOHCodecOpenGLFrames(interop);
        ohCodecOpenGLFlushCount_.fetch_add(
            1,
            std::memory_order_relaxed);
        ohCodecOpenGLMediaReplacementCount_.fetch_add(
            1,
            std::memory_order_relaxed);
        mediaOpenCount_.fetch_add(1, std::memory_order_relaxed);
        player_.setMedia(HEVCMediaPath);
        ohCodecOpenGLStageRendered_.store(0, std::memory_order_relaxed);
        ohCodecOpenGLPhase_.store(
            OHCodecOpenGLPhase::HEVCWarmup,
            std::memory_order_release);
        player_.setState(State::Playing);
        logMessage(
            LOG_INFO,
            "QTAV_OHOS_CHECKPOINT OHCODEC_OPENGL_MEDIA_REPLACED from=h264 to=hevc generation="
                + std::to_string(statistics.surfaceGeneration)
                + " flush=1");
    }

    void finalizeOHCodecOpenGLLifecycle()
    {
        auto interop = activeOHCodecOpenGLInterop();
        std::shared_ptr<OHOSOpenGLVideoRenderer> renderer;
        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            renderer = ohCodecOpenGLRenderer_;
        }
        if (!interop || !renderer) {
            failOHCodecOpenGL(
                "OHCodec/OpenGL finalization lost its renderer or interop");
            return;
        }

        ohCodecOpenGLPhase_.store(
            OHCodecOpenGLPhase::Finalizing,
            std::memory_order_release);
        player_.setState(State::Stopped);
        if (!player_.waitFor(State::Stopped, 4'000)) {
            failOHCodecOpenGL(
                "OHCodec/OpenGL HEVC explicit stop did not complete");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        const auto stoppedStatistics = interop->statistics();
        std::this_thread::sleep_for(std::chrono::milliseconds(160));
        const auto stableStatistics = interop->statistics();
        if (stableStatistics.codecOutputsQueued
                != stoppedStatistics.codecOutputsQueued
            || stableStatistics.surfaceImagesUpdated
                != stoppedStatistics.surfaceImagesUpdated) {
            failOHCodecOpenGL(
                "OHCodec/OpenGL produced output after HEVC reached Stopped");
            return;
        }

        clearAndFlushOHCodecOpenGLFrames(interop);
        ohCodecOpenGLFlushCount_.fetch_add(
            1,
            std::memory_order_relaxed);
        ohCodecOpenGLStopCount_.fetch_add(1, std::memory_order_relaxed);
        player_
            .setRenderCallback({})
            .setVideoRenderAPI({})
            .setVideoFrameScheduler({})
            .setHardwareDecodeConfig({});

        const OHCodecOpenGLInteropStatistics activeStatistics =
            interop->statistics();
        const std::uint64_t totalRendered =
            ohCodecOpenGLH264Rendered_.load(std::memory_order_relaxed)
            + ohCodecOpenGLHEVCRendered_.load(
                std::memory_order_relaxed);
        const std::uint64_t hevcRendered =
            ohCodecOpenGLHEVCRendered_.load(
                std::memory_order_relaxed);
        const std::uint32_t surfaceGeneration =
            ohCodecOpenGLSurfaceGeneration_.load(
                std::memory_order_acquire);
        const bool activeStatisticsPassed =
            activeStatistics.codecOutputsQueued >= totalRendered
            && activeStatistics.surfaceImagesUpdated >= totalRendered
            && activeStatistics.codecOutputsQueued
                    - ohCodecOpenGLH264Statistics_.codecOutputsQueued
                >= hevcRendered
            && activeStatistics.surfaceImagesUpdated
                    - ohCodecOpenGLH264Statistics_.surfaceImagesUpdated
                >= hevcRendered
            && activeStatistics.frameAvailableSignals > 0
            && activeStatistics.frameAvailableSignals
                    - ohCodecOpenGLH264Statistics_.frameAvailableSignals
                >= hevcRendered
            && activeStatistics.redrawSignals > 0
            && activeStatistics.redrawSignals
                    - ohCodecOpenGLH264Statistics_.redrawSignals
                >= hevcRendered
            && activeStatistics.transformQueries >= totalRendered
            && activeStatistics.transformQueries
                    - ohCodecOpenGLH264Statistics_.transformQueries
                >= hevcRendered
            && activeStatistics.timestampMatches >= totalRendered
            && activeStatistics.timestampMatches
                    - ohCodecOpenGLH264Statistics_.timestampMatches
                >= hevcRendered
            && activeStatistics.dolbyVisionFramesQueued
                >= activeStatistics.dolbyVisionTimestampMatches
            && activeStatistics.dolbyVisionTimestampMatches
                >= activeStatistics.dolbyVisionFramesReleased
            && activeStatistics.rawYcbcrImages >= totalRendered
            && activeStatistics.implicitRgbImages == 0
            && activeStatistics.maximumPendingFrames > 0
            && activeStatistics.maximumPendingFrames
                <= OHCodecOpenGLMaximumPendingFrames
            && activeStatistics.contextAttachments > 0
            && activeStatistics.framesReleased >= totalRendered
            && activeStatistics.framesReleased
                    - ohCodecOpenGLH264Statistics_.framesReleased
                >= hevcRendered
            && activeStatistics.unsupportedFrames == 0
            && activeStatistics.lastTimestampNanoseconds > 0
            && activeStatistics.textureName != 0
            && activeStatistics.surfaceGeneration == surfaceGeneration
            && activeStatistics.cpuMapCalls == 0
            && activeStatistics.softwareTransferCalls == 0
            && activeStatistics.stagingCopies == 0
            && activeStatistics.rendererUploads == 0;
        if (!activeStatisticsPassed) {
            failOHCodecOpenGL(
                "OHCodec/OpenGL active external-OES statistics did not satisfy the zero-CPU contract");
            return;
        }

        renderer->setEventCallback({});
        renderer->close();
        const OHCodecOpenGLInteropStatistics finalStatistics =
            interop->statistics();
        const bool lifecyclePassed =
            ohCodecOpenGLH264Rendered_.load(std::memory_order_relaxed)
                >= RequiredOHCodecOpenGLH264Frames
                    + RequiredOHCodecOpenGLH264AfterSeekFrames
            && ohCodecOpenGLHEVCRendered_.load(
                   std::memory_order_relaxed)
                >= RequiredOHCodecOpenGLHEVCFrames
            && ohCodecOpenGLRedrawCallbacks_.load(
                   std::memory_order_relaxed)
                > 0
            && ohCodecOpenGLMaximumScheduledFrames_.load(
                   std::memory_order_relaxed)
                > 0
            && ohCodecOpenGLMaximumScheduledFrames_.load(
                   std::memory_order_relaxed)
                <= OHCodecOpenGLMaximumScheduledFrames
            && ohCodecOpenGLSeekCount_.load(std::memory_order_relaxed)
                == 1
            && ohCodecOpenGLFlushCount_.load(std::memory_order_relaxed)
                == 3
            && ohCodecOpenGLMediaReplacementCount_.load(
                   std::memory_order_relaxed)
                == 1
            && ohCodecOpenGLStopCount_.load(std::memory_order_relaxed)
                == 1
            && ohCodecOpenGLRendererGeneration_.load(
                   std::memory_order_relaxed)
                > 0
            && finalStatistics.contextAttachments > 0
            && finalStatistics.contextDetaches
                == finalStatistics.contextAttachments
            && finalStatistics.textureName == 0
            && finalStatistics.surfaceGeneration == surfaceGeneration
            && finalStatistics.dolbyVisionFramesQueued
                >= finalStatistics.dolbyVisionTimestampMatches
            && finalStatistics.dolbyVisionTimestampMatches
                >= finalStatistics.dolbyVisionFramesReleased
            && finalStatistics.cpuMapCalls == 0
            && finalStatistics.softwareTransferCalls == 0
            && finalStatistics.stagingCopies == 0
            && finalStatistics.rendererUploads == 0
            && mediaOpenCount_.load(std::memory_order_relaxed) == 4
            && player_.state() == State::Stopped;
        if (!lifecyclePassed) {
            failOHCodecOpenGL(
                "OHCodec/OpenGL lifecycle counters did not satisfy the required matrix");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            ohCodecOpenGLRenderer_.reset();
            ohCodecOpenGLInterop_.reset();
        }
        ohCodecOpenGLPhase_.store(
            OHCodecOpenGLPhase::Complete,
            std::memory_order_release);
        ohCodecOpenGLFinalStatistics_ = finalStatistics;

        const std::string interopResult =
            "QTAV_OHOS_OHCODEC_OPENGL_RESULT PASS codecs=h264,hevc"
            " h264Rendered="
            + std::to_string(
                ohCodecOpenGLH264Rendered_.load(
                    std::memory_order_relaxed))
            + " hevcRendered="
            + std::to_string(
                ohCodecOpenGLHEVCRendered_.load(
                    std::memory_order_relaxed))
            + " queued="
            + std::to_string(finalStatistics.codecOutputsQueued)
            + " updated="
            + std::to_string(finalStatistics.surfaceImagesUpdated)
            + " frameAvailable="
            + std::to_string(finalStatistics.frameAvailableSignals)
            + " redrawSignals="
            + std::to_string(finalStatistics.redrawSignals)
            + " redrawCallbacks="
            + std::to_string(
                ohCodecOpenGLRedrawCallbacks_.load(
                    std::memory_order_relaxed))
            + " transformQueries="
            + std::to_string(finalStatistics.transformQueries)
            + " timestampMatches="
            + std::to_string(finalStatistics.timestampMatches)
            + " doviQueued="
            + std::to_string(
                finalStatistics.dolbyVisionFramesQueued)
            + " doviMatched="
            + std::to_string(
                finalStatistics.dolbyVisionTimestampMatches)
            + " doviReleased="
            + std::to_string(
                finalStatistics.dolbyVisionFramesReleased)
            + " ptsUsNormalized="
            + std::to_string(
                finalStatistics.microsecondTimestampsNormalized)
            + " rawYcbcr="
            + std::to_string(finalStatistics.rawYcbcrImages)
            + " implicitRgb="
            + std::to_string(finalStatistics.implicitRgbImages)
            + " attachments="
            + std::to_string(finalStatistics.contextAttachments)
            + " detaches="
            + std::to_string(finalStatistics.contextDetaches)
            + " released="
            + std::to_string(finalStatistics.framesReleased)
            + " stale="
            + std::to_string(finalStatistics.staleFramesDropped)
            + " maxPending="
            + std::to_string(finalStatistics.maximumPendingFrames)
            + " exactQueueMax="
            + std::to_string(
                ohCodecOpenGLMaximumScheduledFrames_.load(
                    std::memory_order_relaxed))
            + " schedulerDrops="
            + std::to_string(
                ohCodecOpenGLSchedulerDrops_.load(
                    std::memory_order_relaxed))
            + " rendererDiscards="
            + std::to_string(
                ohCodecOpenGLRendererDiscards_.load(
                    std::memory_order_relaxed))
            + " generation="
            + std::to_string(finalStatistics.surfaceGeneration)
            + " seek=1 flush=3 mediaReplacement=1 stop=1"
              " cpuMap=0 transfer=0 staging=0 upload=0";
        logMessage(LOG_INFO, interopResult);
        setDetail(
            "OHCodec/OpenGL validation passed; starting strict OH_NativeBuffer Vulkan wrapping");
        startOHCodecVulkanPhase();
    }

    void failOHCodecOpenGL(std::string detail)
    {
        fail(std::move(detail));
        if (phase_.load(std::memory_order_acquire)
            != ValidationPhase::OHCodecOpenGL) {
            return;
        }
        bool expected = false;
        if (ohCodecOpenGLAbortQueued_.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
            queueOHCodecControlAction(OHCodecControlAction::AbortOpenGL);
        }
    }

    void abortOHCodecOpenGLValidation()
    {
        player_.setState(State::Stopped);
        player_.waitFor(State::Stopped, 4'000);
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        teardownOHCodecOpenGLLocked();
    }

    void teardownOHCodecOpenGLLocked()
    {
        player_
            .setRenderCallback({})
            .setVideoRenderAPI({})
            .setVideoFrameScheduler({})
            .setHardwareDecodeConfig({});
        auto renderer = std::move(ohCodecOpenGLRenderer_);
        auto interop = std::move(ohCodecOpenGLInterop_);
        clearAndFlushOHCodecOpenGLFrames(interop);
        if (renderer) {
            renderer->setEventCallback({});
            renderer->close();
        }
    }

    void startOHCodecVulkanPhase()
    {
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        if (!surfaceReady_ || !window_) {
            fail("OHCodec/Vulkan interop has no active XComponent surface");
            return;
        }

        auto context = std::make_unique<OHOSVulkanContext>();
        std::string error;
        if (!context->create(window_, error)) {
            fail(
                error.empty()
                    ? "Could not create the OHCodec/Vulkan context"
                    : error);
            return;
        }
        if (!context->nativeBufferExternalMemoryEnabled()
            || !context->foreignQueueFamilyEnabled()
            || !context->syncFdSemaphoreEnabled()) {
            fail(
                "The connected Vulkan device does not expose the required OH_NativeBuffer external-memory/foreign-queue/sync-fd contract");
            return;
        }

        OHCodecVulkanInteropConfig interopConfig;
        interopConfig.width = 320;
        interopConfig.height = 180;
        interopConfig.ohosExternalMemoryEnabled = true;
        interopConfig.foreignQueueFamilyEnabled = true;
        interopConfig.syncFdSemaphoreEnabled = true;
        interopConfig.samplerYcbcrConversionEnabled =
            context->samplerYcbcrConversionEnabled();
        interopConfig.externalFormatProbeMode =
            externalFormatProbeMode();
        auto interop = std::make_shared<OHCodecVulkanInterop>(
            context->borrowed().device,
            interopConfig);
        if (!*interop) {
            fail(
                "Could not create OHCodec/Vulkan native-buffer interop: "
                + interop->lastError());
            return;
        }

        auto renderer = std::make_shared<OHOSVulkanVideoRenderer>(
            context->borrowed(),
            VulkanOutputPreference::SdrOnly);
        renderer->setHardwareFrameInterop(interop);
        renderer->setEventCallback(
            [this](const VideoRenderEvent& event) {
                if (event.type == VideoRenderEventType::RedrawRequested) {
                    requestOHCodecVulkanRender();
                    return;
                }
                if (phase_.load(std::memory_order_acquire)
                    != ValidationPhase::OHCodecVulkan) {
                    return;
                }
                if (event.detail.find(
                        "opaque Vulkan external format")
                    != std::string::npos) {
                    // The render attempt handles this capability result and
                    // advances the H.264/HEVC strict-rejection probe.
                    return;
                }
                failOHCodecVulkan(
                    std::string(
                        event.type == VideoRenderEventType::SurfaceLost
                            ? "OHCodec/Vulkan surface lost: "
                            : "OHCodec/Vulkan renderer error: ")
                    + (event.detail.empty()
                           ? "no detail"
                           : event.detail));
            });
        if (!renderer->setWindow(window_)) {
            fail(
                "Could not bind the OHCodec/Vulkan renderer to the XComponent window");
            return;
        }
        VideoRenderConfig interopRenderConfig = renderConfig_;
        interopRenderConfig.surfaceSize = renderer->surfaceSize();
        interopRenderConfig.aspectRatio = VideoAspectRatioMode::Fit;
        if (!interopRenderConfig.surfaceSize.isValid()
            || !renderer->open(interopRenderConfig)) {
            fail("Could not open the OHCodec/Vulkan renderer");
            return;
        }

        const OHCodecSurface surface = interop->surface();
        OHCodecHardwareDecodeOptions options;
        options.allowSoftwareFallback = false;
        options.extraHardwareFrames = 8;
        const HardwareDecodeConfig decodeConfig =
            ohCodecHardwareDecodeConfig(surface, options);
        if (!surface || !decodeConfig.device
            || decodeConfig.deviceType != HardwareDeviceType::OHCodec
            || decodeConfig.decoderWrapper != "ohcodec"
            || decodeConfig.surfaceGeneration != surface.generation()) {
            renderer->setEventCallback({});
            renderer->close();
            fail(
                "OHCodec/Vulkan did not publish a valid GPU-texture consumer surface");
            return;
        }

        ohCodecVulkanH264Rendered_.store(0, std::memory_order_relaxed);
        ohCodecVulkanHEVCRendered_.store(0, std::memory_order_relaxed);
        ohCodecVulkanStageRendered_.store(0, std::memory_order_relaxed);
        ohCodecVulkanMaximumScheduledFrames_.store(
            0,
            std::memory_order_relaxed);
        ohCodecVulkanSchedulerDrops_.store(0, std::memory_order_relaxed);
        ohCodecVulkanRendererDiscards_.store(0, std::memory_order_relaxed);
        ohCodecVulkanNextFrameSerial_.store(1, std::memory_order_relaxed);
        ohCodecVulkanRenderQueued_.store(false, std::memory_order_release);
        ohCodecVulkanAbortQueued_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> queueLock(
                ohCodecVulkanQueueMutex_);
            ohCodecVulkanScheduledFrames_.clear();
        }

        ohCodecVulkanSurfaceGeneration_.store(
            surface.generation(),
            std::memory_order_release);
        ohCodecVulkanInterop_ = interop;
        ohCodecVulkanRenderer_ = renderer;
        vulkanContext_ = std::move(context);
        renderConfig_ = interopRenderConfig;
        phase_.store(
            ValidationPhase::OHCodecVulkan,
            std::memory_order_release);
        ohCodecVulkanPhase_.store(
            OHCodecVulkanPhase::H264Warmup,
            std::memory_order_release);

        player_
            .setRenderCallback({})
            .setVideoRenderAPI({})
            .setVideoFrameScheduler(
                [this, surface](
                    const VideoFrame& frame,
                    int,
                    std::int64_t) {
                    return scheduleOHCodecVulkanFrame(frame, surface);
                })
            .setHardwareDecodeConfig(decodeConfig);
        mediaOpenCount_.fetch_add(1, std::memory_order_relaxed);
        player_.setMedia(H264MediaPath);
        player_.setState(State::Playing);

        const std::string detail =
            "QTAV_OHOS_CHECKPOINT OHCODEC_VULKAN_PHASE_READY codec=h264 generation="
            + std::to_string(surface.generation())
            + " producer=oh_consumersurface externalMemory=1 foreignQueue=1 syncFd=1";
        setDetail(detail);
        logMessage(LOG_INFO, detail);
    }

    bool dropUnscheduledOHCodecVulkanFrame(
        const VideoFrame& frame,
        const OHCodecSurface& surface,
        const char* reason)
    {
        OHCodecFrame output = ohCodecFrame(frame, surface);
        const bool dropped = output && output.drop();
        if (!dropped) {
            failOHCodecVulkan(
                std::string("Could not drop an OHCodec/Vulkan ")
                + reason + " output");
            return true;
        }
        ohCodecVulkanSchedulerDrops_.fetch_add(
            1,
            std::memory_order_relaxed);
        return true;
    }

    bool scheduleOHCodecVulkanFrame(
        const VideoFrame& frame,
        const OHCodecSurface& surface)
    {
        if (!frame.hasHardwareFrame() || !surface) {
            failOHCodecVulkan(
                "OHCodec/Vulkan scheduler received an invalid hardware frame or surface");
            return true;
        }
        const OHCodecVulkanPhase phase = ohCodecVulkanPhase_.load(
            std::memory_order_acquire);
        if (failed_.load(std::memory_order_acquire)
            || phase_.load(std::memory_order_acquire)
                != ValidationPhase::OHCodecVulkan
            || (phase != OHCodecVulkanPhase::H264Warmup
                && phase != OHCodecVulkanPhase::HEVCWarmup)) {
            return dropUnscheduledOHCodecVulkanFrame(
                frame,
                surface,
                "inactive-phase");
        }

        {
            std::lock_guard<std::mutex> queueLock(
                ohCodecVulkanQueueMutex_);
            if (ohCodecVulkanScheduledFrames_.size()
                >= OHCodecVulkanMaximumScheduledFrames) {
                return dropUnscheduledOHCodecVulkanFrame(
                    frame,
                    surface,
                    "queue-full");
            }
            const std::uint64_t serial =
                ohCodecVulkanNextFrameSerial_.fetch_add(
                    1,
                    std::memory_order_relaxed);
            ohCodecVulkanScheduledFrames_.push_back({ serial, frame });
            updateMaximum(
                ohCodecVulkanMaximumScheduledFrames_,
                ohCodecVulkanScheduledFrames_.size());
        }
        requestOHCodecVulkanRender();
        return true;
    }

    void requestOHCodecVulkanRender()
    {
        if (phase_.load(std::memory_order_acquire)
                != ValidationPhase::OHCodecVulkan
            || failed_.load(std::memory_order_acquire)) {
            return;
        }
        bool expected = false;
        if (ohCodecVulkanRenderQueued_.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
            queueOHCodecControlAction(
                OHCodecControlAction::RenderVulkanFrame);
        }
    }

    void renderScheduledOHCodecVulkanFrame()
    {
        if (phase_.load(std::memory_order_acquire)
            != ValidationPhase::OHCodecVulkan) {
            return;
        }
        ScheduledOHCodecVulkanFrame scheduled;
        {
            std::lock_guard<std::mutex> queueLock(
                ohCodecVulkanQueueMutex_);
            if (ohCodecVulkanScheduledFrames_.empty()) {
                return;
            }
            scheduled = ohCodecVulkanScheduledFrames_.front();
        }

        VideoRenderAttemptResult result;
        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            if (!ohCodecVulkanRenderer_) {
                return;
            }
            result = ohCodecVulkanRenderer_->renderDetailed(
                scheduled.frame);
        }

        bool hasAnother = false;
        if (result.status == VideoRenderAttemptStatus::Presented
            || result.status == VideoRenderAttemptStatus::Discarded) {
            std::lock_guard<std::mutex> queueLock(
                ohCodecVulkanQueueMutex_);
            if (!ohCodecVulkanScheduledFrames_.empty()
                && ohCodecVulkanScheduledFrames_.front().serial
                    == scheduled.serial) {
                ohCodecVulkanScheduledFrames_.pop_front();
                hasAnother = !ohCodecVulkanScheduledFrames_.empty();
            }
        }

        if (result.status == VideoRenderAttemptStatus::Presented) {
            const OHCodecVulkanPhase phase = ohCodecVulkanPhase_.load(
                std::memory_order_acquire);
            const std::uint64_t stage =
                ohCodecVulkanStageRendered_.fetch_add(
                    1,
                    std::memory_order_relaxed)
                + 1;
            if (phase == OHCodecVulkanPhase::H264Warmup) {
                ohCodecVulkanH264Rendered_.fetch_add(
                    1,
                    std::memory_order_relaxed);
                if (stage >= RequiredOHCodecVulkanH264Frames) {
                    auto expected = phase;
                    if (ohCodecVulkanPhase_.compare_exchange_strong(
                            expected,
                            OHCodecVulkanPhase::H264ReplacementPending,
                            std::memory_order_acq_rel)) {
                        queueOHCodecControlAction(
                            OHCodecControlAction::ReplaceVulkanWithHEVC);
                    }
                }
            } else if (phase == OHCodecVulkanPhase::HEVCWarmup) {
                ohCodecVulkanHEVCRendered_.fetch_add(
                    1,
                    std::memory_order_relaxed);
                if (stage >= RequiredOHCodecVulkanHEVCFrames) {
                    auto expected = phase;
                    if (ohCodecVulkanPhase_.compare_exchange_strong(
                            expected,
                            OHCodecVulkanPhase::HEVCStopPending,
                            std::memory_order_acq_rel)) {
                        queueOHCodecControlAction(
                            OHCodecControlAction::StopVulkanHEVC);
                    }
                }
            }
            if (hasAnother) {
                requestOHCodecVulkanRender();
            }
            return;
        }
        if (result.status == VideoRenderAttemptStatus::Discarded) {
            ohCodecVulkanRendererDiscards_.fetch_add(
                1,
                std::memory_order_relaxed);
            if (hasAnother) {
                requestOHCodecVulkanRender();
            }
            return;
        }
        if (result.status == VideoRenderAttemptStatus::DeferredUntilRedraw
            || result.status
                == VideoRenderAttemptStatus::RetryAfterBackoff) {
            if (result.retryAfterMilliseconds > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(
                        result.retryAfterMilliseconds));
            }
            requestOHCodecVulkanRender();
            return;
        }
        if (result.detail.find("opaque Vulkan external format")
            != std::string::npos) {
            logMessage(
                LOG_INFO,
                "QTAV_OHOS_NATIVEBUFFER_VULKAN_PROBE " + result.detail);
            {
                std::lock_guard<std::mutex> queueLock(
                    ohCodecVulkanQueueMutex_);
                if (!ohCodecVulkanScheduledFrames_.empty()
                    && ohCodecVulkanScheduledFrames_.front().serial
                        == scheduled.serial) {
                    ohCodecVulkanScheduledFrames_.pop_front();
                }
            }
            const OHCodecVulkanPhase phase =
                ohCodecVulkanPhase_.load(std::memory_order_acquire);
            if (phase == OHCodecVulkanPhase::H264Warmup) {
                auto expected = phase;
                if (ohCodecVulkanPhase_.compare_exchange_strong(
                        expected,
                        OHCodecVulkanPhase::H264ReplacementPending,
                        std::memory_order_acq_rel)) {
                    queueOHCodecControlAction(
                        OHCodecControlAction::ReplaceVulkanWithHEVC);
                }
            } else if (phase == OHCodecVulkanPhase::HEVCWarmup) {
                auto expected = phase;
                if (ohCodecVulkanPhase_.compare_exchange_strong(
                        expected,
                        OHCodecVulkanPhase::HEVCStopPending,
                        std::memory_order_acq_rel)) {
                    queueOHCodecControlAction(
                        OHCodecControlAction::StopVulkanHEVC);
                }
            }
            return;
        }
        failOHCodecVulkan(
            result.detail.empty()
                ? "OHCodec/Vulkan render attempt failed"
                : result.detail);
    }

    void clearOHCodecVulkanFrames(const OHCodecSurface& surface)
    {
        std::deque<ScheduledOHCodecVulkanFrame> pending;
        {
            std::lock_guard<std::mutex> queueLock(
                ohCodecVulkanQueueMutex_);
            pending.swap(ohCodecVulkanScheduledFrames_);
        }
        for (const auto& scheduled : pending) {
            OHCodecFrame output = ohCodecFrame(scheduled.frame, surface);
            if (output) {
                output.drop();
            }
        }
        ohCodecVulkanRenderQueued_.store(false, std::memory_order_release);
    }

    void replaceOHCodecVulkanWithHEVC()
    {
        std::shared_ptr<OHCodecVulkanInterop> interop;
        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            interop = ohCodecVulkanInterop_;
        }
        if (!interop) {
            failOHCodecVulkan(
                "OHCodec/Vulkan HEVC replacement lost its interop");
            return;
        }
        player_.setState(State::Paused);
        if (!player_.waitFor(State::Paused, 3'000)) {
            failOHCodecVulkan(
                "OHCodec/Vulkan H.264 did not pause before HEVC replacement");
            return;
        }
        clearOHCodecVulkanFrames(interop->surface());
        const auto statistics = interop->statistics();
        const bool opaqueUnsupported =
            statistics.opaqueFormatsRejected == 1
            && statistics.nativeBuffersAcquired == 1
            && statistics.nativeBuffersImported == 0
            && statistics.directPlaneImports == 0
            && statistics.lastVulkanFormat == VK_FORMAT_UNDEFINED
            && statistics.lastExternalFormat != 0;
        const bool directH264 =
            statistics.directPlaneImports
                >= ohCodecVulkanH264Rendered_.load(
                    std::memory_order_relaxed)
            && statistics.opaqueFormatsRejected == 0
            && statistics.normalizationPasses == 0;
        const bool opaqueSampledH264 =
            statistics.opaqueExternalImports
                >= ohCodecVulkanH264Rendered_.load(
                    std::memory_order_relaxed)
            && statistics.normalizationPasses
                >= ohCodecVulkanH264Rendered_.load(
                    std::memory_order_relaxed)
            && statistics.opaqueFormatsRejected == 0;
        const bool workaroundSampledH264 =
            statistics.externalFormatWorkaroundImports
                >= ohCodecVulkanH264Rendered_.load(
                    std::memory_order_relaxed)
            && statistics.normalizationPasses
                >= ohCodecVulkanH264Rendered_.load(
                    std::memory_order_relaxed)
            && statistics.lastForcedVulkanFormat != VK_FORMAT_UNDEFINED
            && statistics.opaqueFormatsRejected == 0;
        if ((!opaqueUnsupported && !directH264 && !opaqueSampledH264
             && !workaroundSampledH264)
            || statistics.unsupportedFormatsRejected != 0) {
            failOHCodecVulkan(
                "OHCodec/Vulkan H.264 did not satisfy strict direct-plane import before replacement");
            return;
        }
        mediaOpenCount_.fetch_add(1, std::memory_order_relaxed);
        player_.setMedia(HEVCMediaPath);
        ohCodecVulkanStageRendered_.store(0, std::memory_order_relaxed);
        ohCodecVulkanPhase_.store(
            OHCodecVulkanPhase::HEVCWarmup,
            std::memory_order_release);
        player_.setState(State::Playing);
        logMessage(
            LOG_INFO,
            "QTAV_OHOS_CHECKPOINT OHCODEC_VULKAN_MEDIA_REPLACED from=h264 to=hevc directPlanes="
                + std::to_string(statistics.directPlaneImports)
                + " opaqueImports="
                + std::to_string(statistics.opaqueExternalImports)
                + " workaroundImports="
                + std::to_string(
                    statistics.externalFormatWorkaroundImports)
                + " opaqueRejected="
                + std::to_string(statistics.opaqueFormatsRejected));
    }

    void finalizeOHCodecVulkanLifecycle()
    {
        std::shared_ptr<OHCodecVulkanInterop> interop;
        std::shared_ptr<OHOSVulkanVideoRenderer> renderer;
        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            interop = ohCodecVulkanInterop_;
            renderer = ohCodecVulkanRenderer_;
        }
        if (!interop || !renderer) {
            failOHCodecVulkan(
                "OHCodec/Vulkan finalization lost its renderer or interop");
            return;
        }
        ohCodecVulkanPhase_.store(
            OHCodecVulkanPhase::Finalizing,
            std::memory_order_release);
        player_.setState(State::Stopped);
        if (!player_.waitFor(State::Stopped, 4'000)) {
            failOHCodecVulkan(
                "OHCodec/Vulkan HEVC explicit stop did not complete");
            return;
        }
        clearOHCodecVulkanFrames(interop->surface());
        player_
            .setRenderCallback({})
            .setVideoRenderAPI({})
            .setVideoFrameScheduler({})
            .setHardwareDecodeConfig({});
        renderer->setEventCallback({});
        renderer->close();
        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            ohCodecVulkanRenderer_.reset();
        }
        renderer.reset();

        const OHCodecVulkanInteropStatistics statistics =
            interop->statistics();
        const std::uint64_t totalRendered =
            ohCodecVulkanH264Rendered_.load(std::memory_order_relaxed)
            + ohCodecVulkanHEVCRendered_.load(
                std::memory_order_relaxed);
        const bool commonPassed =
            ohCodecVulkanMaximumScheduledFrames_.load(
                std::memory_order_relaxed)
                > 0
            && ohCodecVulkanMaximumScheduledFrames_.load(
                   std::memory_order_relaxed)
                <= VulkanVideoRenderer::FramesInFlight
            && statistics.codecOutputsQueued
                >= statistics.nativeBuffersAcquired
            && statistics.frameAvailableCallbacks
                >= statistics.nativeBuffersAcquired
            && statistics.unsupportedFormatsRejected == 0
            && statistics.cpuMapCalls == 0
            && statistics.softwareTransferCalls == 0
            && statistics.stagingCopies == 0
            && statistics.rendererUploads == 0
            && mediaOpenCount_.load(std::memory_order_relaxed) == 6
            && player_.state() == State::Stopped;
        const bool directPassed =
            ohCodecVulkanH264Rendered_.load(std::memory_order_relaxed)
                >= RequiredOHCodecVulkanH264Frames
            && ohCodecVulkanHEVCRendered_.load(
                   std::memory_order_relaxed)
                >= RequiredOHCodecVulkanHEVCFrames
            && statistics.nativeBuffersAcquired >= totalRendered
            && statistics.nativeBuffersImported >= totalRendered
            && statistics.codecOutputsQueued
                >= statistics.nativeBuffersImported
            && statistics.frameAvailableCallbacks
                >= statistics.nativeBuffersImported
            && statistics.directPlaneImports
                == statistics.nativeBuffersImported
            && statistics.outputsReleasedAfterGpu
                <= statistics.nativeBuffersImported
            && statistics.nativeBuffersImported
                    - statistics.outputsReleasedAfterGpu
                <= VulkanVideoRenderer::FramesInFlight
            && statistics.opaqueFormatsRejected == 0
            && statistics.normalizationPasses == 0
            && (statistics.lastVulkanFormat != VK_FORMAT_UNDEFINED
                || (statistics.forcedVkFormatLibplaceboImports
                        == statistics.nativeBuffersImported
                    && statistics.lastForcedVulkanFormat
                        != VK_FORMAT_UNDEFINED));
        const bool opaqueSampledPassed =
            ohCodecVulkanH264Rendered_.load(std::memory_order_relaxed)
                >= RequiredOHCodecVulkanH264Frames
            && ohCodecVulkanHEVCRendered_.load(
                   std::memory_order_relaxed)
                >= RequiredOHCodecVulkanHEVCFrames
            && statistics.nativeBuffersAcquired >= totalRendered
            && statistics.nativeBuffersImported >= totalRendered
            && statistics.codecOutputsQueued
                >= statistics.nativeBuffersImported
            && statistics.frameAvailableCallbacks
                >= statistics.nativeBuffersImported
            && statistics.opaqueExternalImports
                == statistics.nativeBuffersImported
            && statistics.outputsReleasedAfterGpu
                <= statistics.nativeBuffersImported
            && statistics.nativeBuffersImported
                    - statistics.outputsReleasedAfterGpu
                <= VulkanVideoRenderer::FramesInFlight
            && statistics.normalizationPasses
                == statistics.nativeBuffersImported
            && statistics.opaqueFormatsRejected == 0
            && statistics.lastVulkanFormat == VK_FORMAT_UNDEFINED
            && statistics.lastExternalFormat != 0;
        const bool workaroundSampledPassed =
            ohCodecVulkanH264Rendered_.load(std::memory_order_relaxed)
                >= RequiredOHCodecVulkanH264Frames
            && ohCodecVulkanHEVCRendered_.load(
                   std::memory_order_relaxed)
                >= RequiredOHCodecVulkanHEVCFrames
            && statistics.nativeBuffersAcquired >= totalRendered
            && statistics.nativeBuffersImported >= totalRendered
            && statistics.codecOutputsQueued
                >= statistics.nativeBuffersImported
            && statistics.frameAvailableCallbacks
                >= statistics.nativeBuffersImported
            && statistics.externalFormatWorkaroundImports
                == statistics.nativeBuffersImported
            && statistics.outputsReleasedAfterGpu
                <= statistics.nativeBuffersImported
            && statistics.nativeBuffersImported
                    - statistics.outputsReleasedAfterGpu
                <= VulkanVideoRenderer::FramesInFlight
            && statistics.normalizationPasses
                == statistics.nativeBuffersImported
            && statistics.opaqueFormatsRejected == 0
            && statistics.lastVulkanFormat == VK_FORMAT_UNDEFINED
            && statistics.lastForcedVulkanFormat != VK_FORMAT_UNDEFINED
            && statistics.lastExternalFormat != 0;
        const bool opaqueUnsupportedPassed =
            totalRendered == 0
            && statistics.codecOutputsQueued >= 2
            && statistics.nativeBuffersAcquired >= 2
            && statistics.nativeBuffersImported == 0
            && statistics.directPlaneImports == 0
            && statistics.outputsReleasedAfterGpu == 0
            && statistics.opaqueFormatsRejected >= 2
            && statistics.opaqueExternalObjectProbes
                == statistics.opaqueFormatsRejected
            && (statistics.lastNativeFormat
                    == NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP
                || statistics.lastNativeFormat
                    == NATIVEBUFFER_PIXEL_FMT_YCBCR_P010)
            && statistics.lastVulkanFormat == VK_FORMAT_UNDEFINED
            && statistics.lastExternalFormat != 0;
        const bool vulkanSampled = directPassed || opaqueSampledPassed
            || workaroundSampledPassed;
        const bool passed = commonPassed
            && (vulkanSampled || opaqueUnsupportedPassed);
        if (!passed) {
            failOHCodecVulkan(
                "OHCodec/Vulkan statistics did not satisfy source sampling: rendered="
                + std::to_string(totalRendered)
                + " acquired="
                + std::to_string(statistics.nativeBuffersAcquired)
                + " imported="
                + std::to_string(statistics.nativeBuffersImported)
                + " opaqueImports="
                + std::to_string(statistics.opaqueExternalImports)
                + " normalized="
                + std::to_string(statistics.normalizationPasses)
                + " released="
                + std::to_string(statistics.outputsReleasedAfterGpu)
                + " queued="
                + std::to_string(statistics.codecOutputsQueued)
                + " callbacks="
                + std::to_string(statistics.frameAvailableCallbacks)
                + " mediaOpen="
                + std::to_string(
                    mediaOpenCount_.load(std::memory_order_relaxed))
                + " state="
                + std::to_string(static_cast<int>(player_.state())));
            return;
        }

        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            ohCodecVulkanRenderer_.reset();
            ohCodecVulkanInterop_.reset();
            vulkanContext_.reset();
        }
        ohCodecVulkanPhase_.store(
            OHCodecVulkanPhase::Complete,
            std::memory_order_release);
        ohCodecVulkanFinalStatistics_ = statistics;
        ohCodecVulkanDirectPassed_.store(
            vulkanSampled,
            std::memory_order_release);

        const std::string vulkanResult =
            std::string("QTAV_OHOS_OHCODEC_VULKAN_RESULT ")
            + (vulkanSampled ? "PASS" : "UNSUPPORTED")
            + " mode="
            + (statistics.forcedVkFormatLibplaceboImports != 0
                    ? "forced-vkformat-libplacebo"
                : statistics.forcedVkFormatNativeSamples != 0
                    ? "forced-vkformat-native"
                : statistics.externalFormatWorkaroundImports != 0
                    ? "external-format-workaround"
                : directPassed ? "explicit-plane"
                : opaqueSampledPassed ? "opaque-ycbcr-normalized"
                                      : "unsupported")
            + " codecs=h264,hevc h264Rendered="
            + std::to_string(
                ohCodecVulkanH264Rendered_.load(
                    std::memory_order_relaxed))
            + " hevcRendered="
            + std::to_string(
                ohCodecVulkanHEVCRendered_.load(
                    std::memory_order_relaxed))
            + " acquired="
            + std::to_string(statistics.nativeBuffersAcquired)
            + " imported="
            + std::to_string(statistics.nativeBuffersImported)
            + " directPlanes="
            + std::to_string(statistics.directPlaneImports)
            + " opaqueImports="
            + std::to_string(statistics.opaqueExternalImports)
            + " workaroundImports="
            + std::to_string(
                statistics.externalFormatWorkaroundImports)
            + " forcedVkFormatImports="
            + std::to_string(statistics.forcedVkFormatImports)
            + " forcedNativeSamples="
            + std::to_string(statistics.forcedVkFormatNativeSamples)
            + " forcedLibplacebo="
            + std::to_string(
                statistics.forcedVkFormatLibplaceboImports)
            + " codecQueued="
            + std::to_string(statistics.codecOutputsQueued)
            + " availableCallbacks="
            + std::to_string(statistics.frameAvailableCallbacks)
            + " acquireFences="
            + std::to_string(statistics.acquireFencesImported)
            + " releasedAfterGpu="
            + std::to_string(statistics.outputsReleasedAfterGpu)
            + " nativeFormat="
            + std::to_string(statistics.lastNativeFormat)
            + " vkFormat="
            + std::to_string(
                static_cast<std::int32_t>(
                    statistics.lastVulkanFormat))
            + " forcedVkFormat="
            + std::to_string(
                static_cast<std::int32_t>(
                    statistics.lastForcedVulkanFormat))
            + " externalFormat="
            + std::to_string(statistics.lastExternalFormat)
            + " exactQueueMax="
            + std::to_string(
                ohCodecVulkanMaximumScheduledFrames_.load(
                    std::memory_order_relaxed))
            + " schedulerDrops="
            + std::to_string(
                ohCodecVulkanSchedulerDrops_.load(
                    std::memory_order_relaxed))
            + " rendererDiscards="
            + std::to_string(
                ohCodecVulkanRendererDiscards_.load(
                    std::memory_order_relaxed))
            + " opaqueRejected="
            + std::to_string(statistics.opaqueFormatsRejected)
            + " objectProbes="
            + std::to_string(statistics.opaqueExternalObjectProbes)
            + " objectProbeSuccesses="
            + std::to_string(
                statistics.opaqueExternalObjectProbeSuccesses)
            + " unsupportedRejected=0 normalization="
            + std::to_string(statistics.normalizationPasses)
            + " cpuMap=0 transfer=0 staging=0 upload=0";
        logMessage(LOG_INFO, vulkanResult);
        setDetail(
            "Strict OHCodec/Vulkan validation completed; starting selector hardware-frame fallback validation");
        startOHCodecFallbackPhase(
            MobileHardwareFrameFallbackRoute::OpenGLESInterop);
    }

    void startOHCodecFallbackPhase(MobileHardwareFrameFallbackRoute route)
    {
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        if (!surfaceReady_ || !window_) {
            fail("OHCodec selector fallback has no active XComponent surface");
            return;
        }
        if (route != MobileHardwareFrameFallbackRoute::OpenGLESInterop &&
            route != MobileHardwareFrameFallbackRoute::SoftwareDecode) {
            fail("OHCodec selector fallback received an invalid route");
            return;
        }

        ohCodecFallbackRoute_.store(route, std::memory_order_release);
        ohCodecFallbackVulkanPresented_.store(0, std::memory_order_relaxed);
        ohCodecFallbackOpenGLPresented_.store(0, std::memory_order_relaxed);
        ohCodecFallbackSoftwarePresented_.store(0, std::memory_order_relaxed);
        ohCodecFallbackHardwareInputs_.store(0, std::memory_order_relaxed);
        ohCodecFallbackSoftwareInputs_.store(0, std::memory_order_relaxed);
        ohCodecFallbackSelectorDiscards_.store(0, std::memory_order_relaxed);
        ohCodecFallbackSchedulerDrops_.store(0, std::memory_order_relaxed);
        ohCodecFallbackMaximumScheduledFrames_.store(0,
                                                     std::memory_order_relaxed);
        ohCodecFallbackNextFrameSerial_.store(1, std::memory_order_relaxed);
        ohCodecFallbackVulkanSelected_.store(false, std::memory_order_release);
        ohCodecFallbackObserved_.store(false, std::memory_order_release);
        ohCodecFallbackRouteApplied_.store(false, std::memory_order_release);
        ohCodecFallbackRenderQueued_.store(false, std::memory_order_release);
        ohCodecFallbackAbortQueued_.store(false, std::memory_order_release);
        ohCodecFallbackFinishQueued_.store(false, std::memory_order_release);
        ohCodecFallbackMediaOpenAtRebind_.store(0, std::memory_order_relaxed);
        ohCodecFallbackVulkanSurfaceGeneration_.store(
            0,
            std::memory_order_release);
        ohCodecFallbackOpenGLSurfaceGeneration_.store(
            0,
            std::memory_order_release);
        {
            std::lock_guard<std::mutex> queueLock(ohCodecFallbackQueueMutex_);
            ohCodecFallbackScheduledFrames_.clear();
        }

        phase_.store(ValidationPhase::OHCodecFallback,
                     std::memory_order_release);
        ohCodecFallbackPhase_.store(
            route == MobileHardwareFrameFallbackRoute::OpenGLESInterop
                ? OHCodecFallbackPhase::NativeInterop
                : OHCodecFallbackPhase::SoftwareDecode,
            std::memory_order_release);

        MobileRendererSelectorConfig selectorConfig;
        selectorConfig.maximumRecoveryAttempts = 0;
        selectorConfig.vulkan = [this] {
            auto context = std::make_unique<OHOSVulkanContext>();
            std::string error;
            if (!context->create(window_, error)) {
                return MobileRendererCandidate{
                    {},
                    error.empty()
                        ? "Could not create the OHCodec fallback Vulkan context"
                        : std::move(error),
                };
            }
            if (!context->nativeBufferExternalMemoryEnabled() ||
                !context->foreignQueueFamilyEnabled() ||
                !context->syncFdSemaphoreEnabled()) {
                return MobileRendererCandidate{
                    {},
                    "The Vulkan device lacks the OHCodec native-buffer import "
                    "contract",
                };
            }

            OHCodecVulkanInteropConfig interopConfig;
            interopConfig.width = 320;
            interopConfig.height = 180;
            interopConfig.ohosExternalMemoryEnabled = true;
            interopConfig.foreignQueueFamilyEnabled = true;
            interopConfig.syncFdSemaphoreEnabled = true;
            interopConfig.samplerYcbcrConversionEnabled =
                context->samplerYcbcrConversionEnabled();
            interopConfig.externalFormatProbeMode =
                externalFormatProbeMode();
            auto interop = std::make_shared<OHCodecVulkanInterop>(
                context->borrowed().device,
                interopConfig);
            if (!*interop) {
                return MobileRendererCandidate{
                    {},
                    "Could not create the OHCodec fallback Vulkan interop: " +
                        interop->lastError(),
                };
            }
            auto renderer = std::make_shared<OHOSVulkanVideoRenderer>(
                context->borrowed(),
                VulkanOutputPreference::SdrOnly);
            renderer->setHardwareFrameInterop(interop);
            if (!renderer->setWindow(window_)) {
                return MobileRendererCandidate{
                    {},
                    "Could not bind the OHCodec fallback Vulkan renderer",
                };
            }

            const OHCodecSurface surface = interop->surface();
            if (!surface) {
                return MobileRendererCandidate{
                    {},
                    "OHCodec fallback Vulkan interop published no consumer "
                    "surface",
                };
            }
            ohCodecFallbackVulkanSurfaceGeneration_.store(
                surface.generation(),
                std::memory_order_release);
            ohCodecFallbackVulkanInterop_ = interop;
            ohCodecFallbackVulkanRenderer_ = renderer;
            vulkanContext_ = std::move(context);
            return MobileRendererCandidate{
                std::make_shared<FatalAfterVideoRenderer>(
                    renderer,
                    InjectOHCodecFallbackAfterVulkanFrames),
                "OHCodec Vulkan native interop with bounded fatal injection",
            };
        };
        selectorConfig.openGLES = [this] {
            // The selector closes the Vulkan renderer before this factory is
            // entered. Keep the borrowed Vulkan device alive until the old
            // interop is inspected and destroyed at phase finalization.
            auto renderer = std::make_shared<OHOSOpenGLVideoRenderer>(
                OpenGLOutputPreference::SdrOnly);
            if (ohCodecFallbackRoute_.load(std::memory_order_acquire) ==
                MobileHardwareFrameFallbackRoute::OpenGLESInterop) {
                OHCodecOpenGLInteropConfig interopConfig;
                interopConfig.maximumPendingFrames =
                    static_cast<int>(OHCodecOpenGLMaximumPendingFrames);
                auto interop =
                    std::make_shared<OHCodecOpenGLInterop>(interopConfig);
                renderer->setHardwareFrameInterop(interop);
                ohCodecFallbackOpenGLInterop_ = interop;
            }
            if (!renderer->setWindow(window_)) {
                ohCodecFallbackOpenGLInterop_.reset();
                return MobileRendererCandidate{
                    {},
                    "Could not bind the OHCodec fallback OpenGL ES renderer",
                };
            }
            ohCodecFallbackOpenGLRenderer_ = renderer;
            return MobileRendererCandidate{
                std::move(renderer),
                ohCodecFallbackRoute_.load(std::memory_order_acquire) ==
                        MobileHardwareFrameFallbackRoute::OpenGLESInterop
                    ? "OHCodec OH_NativeImage external-OES fallback"
                    : "OpenGL ES software-frame fallback",
            };
        };

        auto selector = std::make_shared<MobileVideoRendererSelector>(
            std::move(selectorConfig));
        selector->setEventCallback([this](const VideoRenderEvent& event) {
            if (event.type == VideoRenderEventType::RedrawRequested) {
                requestOHCodecFallbackRender();
                return;
            }
            if (phase_.load(std::memory_order_acquire) ==
                ValidationPhase::OHCodecFallback) {
                failOHCodecFallback(
                    event.detail.empty()
                        ? "OHCodec selector fallback renderer failed"
                        : event.detail);
            }
        });
        selector->setSelectionCallback(
            [this](const MobileRendererSelectionEvent& event) {
                lastSelectedAPI_.store(event.selectedAPI,
                                       std::memory_order_release);
                if (event.type == MobileRendererSelectionEventType::Selected &&
                    event.selectedAPI == MobileRenderAPI::Vulkan) {
                    ohCodecFallbackVulkanSelected_.store(
                        true,
                        std::memory_order_release);
                }
                if (event.type == MobileRendererSelectionEventType::FellBack &&
                    event.previousAPI == MobileRenderAPI::Vulkan &&
                    event.selectedAPI == MobileRenderAPI::OpenGLES) {
                    ohCodecFallbackObserved_.store(true,
                                                   std::memory_order_release);
                }
                logMessage(event.type ==
                                   MobileRendererSelectionEventType::Unavailable
                               ? LOG_ERROR
                               : LOG_INFO,
                           std::string("OHCodec fallback selector ") +
                               mobileRenderAPIName(event.previousAPI) + " -> " +
                               mobileRenderAPIName(event.selectedAPI) + ": " +
                               event.detail);
            });
        selector->setHardwareFrameFallbackCallback(
            [this](const MobileHardwareFrameFallbackEvent& event) {
                return applyOHCodecFallbackRoute(event);
            });
        if (!selector->open(renderConfig_)) {
            const std::string error = selector->lastError();
            selector->close();
            failOHCodecFallback(
                error.empty() ? "Could not open the OHCodec fallback selector"
                              : error);
            return;
        }
        if (!ohCodecFallbackVulkanInterop_) {
            selector->close();
            failOHCodecFallback(
                "OHCodec fallback selector opened without Vulkan interop");
            return;
        }

        const OHCodecSurface surface = ohCodecFallbackVulkanInterop_->surface();
        OHCodecHardwareDecodeOptions options;
        options.allowSoftwareFallback = false;
        options.extraHardwareFrames = 8;
        const HardwareDecodeConfig decodeConfig =
            ohCodecHardwareDecodeConfig(surface, options);
        if (!decodeConfig.device ||
            decodeConfig.deviceType != HardwareDeviceType::OHCodec ||
            decodeConfig.decoderWrapper != "ohcodec") {
            selector->close();
            failOHCodecFallback("OHCodec fallback produced an invalid Vulkan "
                                "decode configuration");
            return;
        }

        ohCodecFallbackSelector_ = selector;
        player_.setRenderCallback({})
            .setVideoRenderAPI({})
            .setVideoFrameScheduler(
                [this](const VideoFrame& frame, int, std::int64_t) {
                    return scheduleOHCodecFallbackFrame(frame);
                })
            .setHardwareDecodeConfig(decodeConfig);
        mediaOpenCount_.fetch_add(1, std::memory_order_relaxed);
        player_.setMedia(H264MediaPath);
        player_.setState(State::Playing);

        const std::string detail =
            std::string(
                "QTAV_OHOS_CHECKPOINT OHCODEC_FALLBACK_PHASE_READY route=") +
            mobileHardwareFrameFallbackRouteName(route) +
            " generation=" + std::to_string(surface.generation()) +
            " injectedFatalAfter=" +
            std::to_string(InjectOHCodecFallbackAfterVulkanFrames) +
            " mediaOpen=" +
            std::to_string(mediaOpenCount_.load(std::memory_order_relaxed));
        setDetail(detail);
        logMessage(LOG_INFO, detail);
    }

    MobileHardwareFrameFallbackDecision
    applyOHCodecFallbackRoute(const MobileHardwareFrameFallbackEvent& event)
    {
        const MobileHardwareFrameFallbackRoute route =
            ohCodecFallbackRoute_.load(std::memory_order_acquire);
        if (event.previousAPI != MobileRenderAPI::Vulkan ||
            event.selectedAPI != MobileRenderAPI::OpenGLES ||
            event.sourceDevice != HardwareDeviceType::OHCodec ||
            event.sourceSurfaceGeneration !=
                ohCodecFallbackVulkanSurfaceGeneration_.load(
                    std::memory_order_acquire)) {
            return {
                MobileHardwareFrameFallbackRoute::None,
                "The OHCodec fallback event did not match the retired Vulkan "
                "surface",
            };
        }

        if (route == MobileHardwareFrameFallbackRoute::OpenGLESInterop) {
            const auto interop = ohCodecFallbackOpenGLInterop_;
            if (!interop || !*interop || !interop->surface()) {
                return {
                    MobileHardwareFrameFallbackRoute::None,
                    "OpenGL ES did not publish an OHCodec native interop "
                    "surface",
                };
            }
            const OHCodecSurface surface = interop->surface();
            if (surface.generation() == event.sourceSurfaceGeneration) {
                return {
                    MobileHardwareFrameFallbackRoute::None,
                    "The OpenGL ES fallback reused the retired Vulkan surface "
                    "generation",
                };
            }
            OHCodecHardwareDecodeOptions options;
            options.allowSoftwareFallback = false;
            options.extraHardwareFrames = 6;
            const HardwareDecodeConfig decodeConfig =
                ohCodecHardwareDecodeConfig(surface, options);
            if (!decodeConfig.device ||
                decodeConfig.surfaceGeneration != surface.generation()) {
                return {
                    MobileHardwareFrameFallbackRoute::None,
                    "OpenGL ES published an invalid OHCodec decode "
                    "configuration",
                };
            }
            ohCodecFallbackOpenGLSurfaceGeneration_.store(
                surface.generation(),
                std::memory_order_release);
            ohCodecFallbackMediaOpenAtRebind_.store(
                mediaOpenCount_.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            player_.setHardwareDecodeConfig(decodeConfig);
        } else if (route == MobileHardwareFrameFallbackRoute::SoftwareDecode) {
            ohCodecFallbackMediaOpenAtRebind_.store(
                mediaOpenCount_.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            player_.setHardwareDecodeConfig({});
        } else {
            return {
                MobileHardwareFrameFallbackRoute::None,
                "No supported OHCodec fallback route was selected",
            };
        }

        ohCodecFallbackRouteApplied_.store(true, std::memory_order_release);
        return {
            route,
            route == MobileHardwareFrameFallbackRoute::OpenGLESInterop
                ? "Rebound subsequent OHCodec output to the OpenGL ES native "
                  "interop surface"
                : "Disabled OHCodec so subsequent OpenGL ES input is "
                  "software-decoded",
        };
    }

    bool dropOHCodecFallbackFrame(const VideoFrame& frame, const char* reason)
    {
        if (!frame.hasHardwareFrame()) {
            return true;
        }
        const HardwareFrame hardware = frame.hardwareFrame();
        const NativeHandle handle =
            hardware.nativeHandle(HardwareHandleType::Surface);
        std::shared_ptr<OHCodecVulkanInterop> vulkanInterop;
        std::shared_ptr<OHCodecOpenGLInterop> openGLInterop;
        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            vulkanInterop = ohCodecFallbackVulkanInterop_;
            openGLInterop = ohCodecFallbackOpenGLInterop_;
        }
        OHCodecSurface surface;
        if (vulkanInterop &&
            handle.subresource == vulkanInterop->surface().generation()) {
            surface = vulkanInterop->surface();
        } else if (openGLInterop && handle.subresource ==
                                        openGLInterop->surface().generation()) {
            surface = openGLInterop->surface();
        }
        OHCodecFrame output =
            surface ? ohCodecFrame(frame, surface) : OHCodecFrame{};
        if (!output || !output.drop()) {
            failOHCodecFallback(
                std::string("Could not drop an OHCodec fallback ") + reason +
                " output");
            return false;
        }
        ohCodecFallbackSchedulerDrops_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool scheduleOHCodecFallbackFrame(const VideoFrame& frame)
    {
        const OHCodecFallbackPhase fallbackPhase =
            ohCodecFallbackPhase_.load(std::memory_order_acquire);
        if (failed_.load(std::memory_order_acquire) ||
            phase_.load(std::memory_order_acquire) !=
                ValidationPhase::OHCodecFallback ||
            (fallbackPhase != OHCodecFallbackPhase::NativeInterop &&
             fallbackPhase != OHCodecFallbackPhase::SoftwareDecode)) {
            return frame.hasHardwareFrame()
                       ? dropOHCodecFallbackFrame(frame, "inactive-phase")
                       : true;
        }

        if (frame.hasHardwareFrame()) {
            const HardwareFrame hardware = frame.hardwareFrame();
            if (hardware.deviceType() != HardwareDeviceType::OHCodec) {
                failOHCodecFallback("OHCodec fallback scheduler received a "
                                    "foreign hardware frame");
                return true;
            }
            ohCodecFallbackHardwareInputs_.fetch_add(1,
                                                     std::memory_order_relaxed);
        } else {
            ohCodecFallbackSoftwareInputs_.fetch_add(1,
                                                     std::memory_order_relaxed);
        }

        bool queueFull = false;
        {
            std::lock_guard<std::mutex> queueLock(ohCodecFallbackQueueMutex_);
            if (ohCodecFallbackScheduledFrames_.size() >=
                OHCodecFallbackMaximumScheduledFrames) {
                queueFull = true;
            } else {
                const std::uint64_t serial =
                    ohCodecFallbackNextFrameSerial_.fetch_add(
                        1,
                        std::memory_order_relaxed);
                ohCodecFallbackScheduledFrames_.push_back({serial, frame});
                updateMaximum(ohCodecFallbackMaximumScheduledFrames_,
                              ohCodecFallbackScheduledFrames_.size());
            }
        }
        if (queueFull) {
            return frame.hasHardwareFrame()
                       ? dropOHCodecFallbackFrame(frame, "queue-full")
                       : true;
        }
        requestOHCodecFallbackRender();
        return true;
    }

    void requestOHCodecFallbackRender()
    {
        if (phase_.load(std::memory_order_acquire) !=
                ValidationPhase::OHCodecFallback ||
            failed_.load(std::memory_order_acquire)) {
            return;
        }
        bool expected = false;
        if (ohCodecFallbackRenderQueued_.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
            queueOHCodecControlAction(
                OHCodecControlAction::RenderFallbackFrame);
        }
    }

    void renderScheduledOHCodecFallbackFrame()
    {
        if (phase_.load(std::memory_order_acquire) !=
            ValidationPhase::OHCodecFallback) {
            return;
        }
        ScheduledOHCodecFallbackFrame scheduled;
        {
            std::lock_guard<std::mutex> queueLock(ohCodecFallbackQueueMutex_);
            if (ohCodecFallbackScheduledFrames_.empty()) {
                return;
            }
            scheduled = ohCodecFallbackScheduledFrames_.front();
        }

        std::shared_ptr<MobileVideoRendererSelector> selector;
        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            selector = ohCodecFallbackSelector_;
        }
        if (!selector) {
            failOHCodecFallback("OHCodec fallback render lost its selector");
            return;
        }

        const bool hardware = scheduled.frame.hasHardwareFrame();
        std::uint32_t generation = 0;
        if (hardware) {
            generation = scheduled.frame.hardwareFrame()
                             .nativeHandle(HardwareHandleType::Surface)
                             .subresource;
        }
        const VideoRenderAttemptResult result =
            selector->renderDetailed(scheduled.frame);

        bool hasAnother = false;
        if (result.frameConsumed()) {
            std::lock_guard<std::mutex> queueLock(ohCodecFallbackQueueMutex_);
            if (!ohCodecFallbackScheduledFrames_.empty() &&
                ohCodecFallbackScheduledFrames_.front().serial ==
                    scheduled.serial) {
                ohCodecFallbackScheduledFrames_.pop_front();
                hasAnother = !ohCodecFallbackScheduledFrames_.empty();
            }
        }

        if (result.status == VideoRenderAttemptStatus::Presented) {
            const MobileRenderAPI api = selector->selectedAPI();
            const OHCodecFallbackPhase fallbackPhase =
                ohCodecFallbackPhase_.load(std::memory_order_acquire);
            if (hardware && api == MobileRenderAPI::Vulkan &&
                generation == ohCodecFallbackVulkanSurfaceGeneration_.load(
                                  std::memory_order_acquire)) {
                ohCodecFallbackVulkanPresented_.fetch_add(
                    1,
                    std::memory_order_relaxed);
            } else if (fallbackPhase == OHCodecFallbackPhase::NativeInterop &&
                       hardware && api == MobileRenderAPI::OpenGLES &&
                       generation ==
                           ohCodecFallbackOpenGLSurfaceGeneration_.load(
                               std::memory_order_acquire)) {
                const std::uint64_t presented =
                    ohCodecFallbackOpenGLPresented_.fetch_add(
                        1,
                        std::memory_order_relaxed) +
                    1;
                if (presented >= RequiredOHCodecNativeFallbackFrames) {
                    bool expected = false;
                    if (ohCodecFallbackFinishQueued_.compare_exchange_strong(
                            expected,
                            true,
                            std::memory_order_acq_rel)) {
                        ohCodecFallbackPhase_.store(
                            OHCodecFallbackPhase::NativeInteropFinalizing,
                            std::memory_order_release);
                        queueOHCodecControlAction(
                            OHCodecControlAction::FinishNativeFallback);
                    }
                }
            } else if (fallbackPhase == OHCodecFallbackPhase::SoftwareDecode &&
                       !hardware && api == MobileRenderAPI::OpenGLES) {
                const std::uint64_t presented =
                    ohCodecFallbackSoftwarePresented_.fetch_add(
                        1,
                        std::memory_order_relaxed) +
                    1;
                if (presented >= RequiredOHCodecSoftwareFallbackFrames) {
                    bool expected = false;
                    if (ohCodecFallbackFinishQueued_.compare_exchange_strong(
                            expected,
                            true,
                            std::memory_order_acq_rel)) {
                        ohCodecFallbackPhase_.store(
                            OHCodecFallbackPhase::SoftwareDecodeFinalizing,
                            std::memory_order_release);
                        queueOHCodecControlAction(
                            OHCodecControlAction::FinishSoftwareFallback);
                    }
                }
            }
            if (hasAnother) {
                requestOHCodecFallbackRender();
            }
            return;
        }
        if (result.status == VideoRenderAttemptStatus::Discarded) {
            ohCodecFallbackSelectorDiscards_.fetch_add(
                1,
                std::memory_order_relaxed);
            if (hasAnother) {
                requestOHCodecFallbackRender();
            }
            return;
        }
        if (result.status == VideoRenderAttemptStatus::DeferredUntilRedraw ||
            result.status == VideoRenderAttemptStatus::RetryAfterBackoff) {
            if (result.retryAfterMilliseconds > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(result.retryAfterMilliseconds));
            }
            requestOHCodecFallbackRender();
            return;
        }
        failOHCodecFallback(
            result.detail.empty()
                ? "OHCodec selector fallback render attempt failed"
                : result.detail);
    }

    void clearOHCodecFallbackFrames()
    {
        std::deque<ScheduledOHCodecFallbackFrame> pending;
        {
            std::lock_guard<std::mutex> queueLock(ohCodecFallbackQueueMutex_);
            pending.swap(ohCodecFallbackScheduledFrames_);
        }
        for (const auto& scheduled : pending) {
            if (scheduled.frame.hasHardwareFrame()) {
                dropOHCodecFallbackFrame(scheduled.frame, "queued-teardown");
            }
        }
        ohCodecFallbackRenderQueued_.store(false, std::memory_order_release);
    }

    void finalizeOHCodecNativeFallback()
    {
        std::shared_ptr<MobileVideoRendererSelector> selector;
        std::shared_ptr<OHCodecVulkanInterop> vulkanInterop;
        std::shared_ptr<OHCodecOpenGLInterop> openGLInterop;
        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            selector = ohCodecFallbackSelector_;
            vulkanInterop = ohCodecFallbackVulkanInterop_;
            openGLInterop = ohCodecFallbackOpenGLInterop_;
        }
        if (!selector || !vulkanInterop || !openGLInterop) {
            failOHCodecFallback("OHCodec native fallback finalization lost its "
                                "selector or interop");
            return;
        }

        player_.setState(State::Stopped);
        if (!player_.waitFor(State::Stopped, 4'000)) {
            failOHCodecFallback(
                "OHCodec native fallback stop did not complete");
            return;
        }
        clearOHCodecFallbackFrames();
        player_.setRenderCallback({})
            .setVideoRenderAPI({})
            .setVideoFrameScheduler({})
            .setHardwareDecodeConfig({});
        const MobileRenderAPI selected = selector->selectedAPI();
        const MobileHardwareFrameFallbackRoute appliedRoute =
            selector->hardwareFrameFallbackRoute();
        selector->setEventCallback({});
        selector->setSelectionCallback({});
        selector->setHardwareFrameFallbackCallback({});
        selector->close();

        const OHCodecVulkanInteropStatistics vulkanStatistics =
            vulkanInterop->statistics();
        const OHCodecOpenGLInteropStatistics openGLStatistics =
            openGLInterop->statistics();
        const std::uint64_t rendered =
            ohCodecFallbackOpenGLPresented_.load(std::memory_order_relaxed);
        const bool vulkanSourcePassed =
            vulkanStatistics.nativeBuffersAcquired > 0 &&
            (vulkanStatistics.directPlaneImports > 0 ||
             vulkanStatistics.opaqueExternalImports > 0 ||
             vulkanStatistics.externalFormatWorkaroundImports > 0 ||
             vulkanStatistics.opaqueFormatsRejected > 0) &&
            vulkanStatistics.unsupportedFormatsRejected == 0 &&
            vulkanStatistics.normalizationPasses <=
                vulkanStatistics.opaqueExternalImports +
                    vulkanStatistics.externalFormatWorkaroundImports &&
            vulkanStatistics.cpuMapCalls == 0 &&
            vulkanStatistics.softwareTransferCalls == 0 &&
            vulkanStatistics.stagingCopies == 0 &&
            vulkanStatistics.rendererUploads == 0;
        const bool passed =
            selected == MobileRenderAPI::OpenGLES &&
            appliedRoute == MobileHardwareFrameFallbackRoute::OpenGLESInterop &&
            ohCodecFallbackVulkanSelected_.load(std::memory_order_acquire) &&
            ohCodecFallbackObserved_.load(std::memory_order_acquire) &&
            ohCodecFallbackRouteApplied_.load(std::memory_order_acquire) &&
            ohCodecFallbackMediaOpenAtRebind_.load(std::memory_order_relaxed) ==
                7 &&
            mediaOpenCount_.load(std::memory_order_relaxed) == 7 &&
            ohCodecFallbackVulkanSurfaceGeneration_.load(
                std::memory_order_acquire) != 0 &&
            ohCodecFallbackOpenGLSurfaceGeneration_.load(
                std::memory_order_acquire) != 0 &&
            ohCodecFallbackVulkanSurfaceGeneration_.load(
                std::memory_order_acquire) !=
                ohCodecFallbackOpenGLSurfaceGeneration_.load(
                    std::memory_order_acquire) &&
            ohCodecFallbackHardwareInputs_.load(std::memory_order_relaxed) >
                0 &&
            rendered >= RequiredOHCodecNativeFallbackFrames &&
            ohCodecFallbackMaximumScheduledFrames_.load(
                std::memory_order_relaxed) > 0 &&
            ohCodecFallbackMaximumScheduledFrames_.load(
                std::memory_order_relaxed) <=
                OHCodecFallbackMaximumScheduledFrames &&
            openGLStatistics.rawYcbcrImages >= rendered &&
            openGLStatistics.implicitRgbImages == 0 &&
            openGLStatistics.contextAttachments > 0 &&
            openGLStatistics.contextDetaches ==
                openGLStatistics.contextAttachments &&
            openGLStatistics.framesReleased >= rendered &&
            openGLStatistics.textureName == 0 &&
            openGLStatistics.surfaceGeneration ==
                ohCodecFallbackOpenGLSurfaceGeneration_.load(
                    std::memory_order_acquire) &&
            openGLStatistics.cpuMapCalls == 0 &&
            openGLStatistics.softwareTransferCalls == 0 &&
            openGLStatistics.stagingCopies == 0 &&
            openGLStatistics.rendererUploads == 0 && vulkanSourcePassed &&
            player_.state() == State::Stopped;
        if (!passed) {
            failOHCodecFallback(
                "OHCodec native Vulkan-to-OpenGL ES fallback counters did "
                "not satisfy the required route: direct=" +
                std::to_string(vulkanStatistics.directPlaneImports) +
                " opaque=" +
                std::to_string(vulkanStatistics.opaqueExternalImports) +
                " workaround=" +
                std::to_string(
                    vulkanStatistics.externalFormatWorkaroundImports) +
                " normalized=" +
                std::to_string(vulkanStatistics.normalizationPasses) +
                " glesRendered=" + std::to_string(rendered));
            return;
        }

        ohCodecFallbackOpenGLFinalStatistics_ = openGLStatistics;
        const std::string result =
            "QTAV_OHOS_OHCODEC_FALLBACK_RESULT PASS route=opengl-interop"
            " vulkanRendered=" +
            std::to_string(ohCodecFallbackVulkanPresented_.load(
                std::memory_order_relaxed)) +
            " glesRendered=" + std::to_string(rendered) + " hardwareInputs=" +
            std::to_string(ohCodecFallbackHardwareInputs_.load(
                std::memory_order_relaxed)) +
            " oldGeneration=" +
            std::to_string(ohCodecFallbackVulkanSurfaceGeneration_.load(
                std::memory_order_relaxed)) +
            " newGeneration=" +
            std::to_string(ohCodecFallbackOpenGLSurfaceGeneration_.load(
                std::memory_order_relaxed)) +
            " vulkanImported=" +
            std::to_string(vulkanStatistics.nativeBuffersImported) +
            " opaqueRejected=" +
            std::to_string(vulkanStatistics.opaqueFormatsRejected) +
            " rawYcbcr=" + std::to_string(openGLStatistics.rawYcbcrImages) +
            " released=" + std::to_string(openGLStatistics.framesReleased) +
            " mediaOpen=7 cpuMap=0 transfer=0 staging=0 upload=0";
        logMessage(LOG_INFO, result);

        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            ohCodecFallbackSelector_.reset();
            ohCodecFallbackVulkanRenderer_.reset();
            ohCodecFallbackOpenGLRenderer_.reset();
            ohCodecFallbackVulkanInterop_.reset();
            ohCodecFallbackOpenGLInterop_.reset();
            vulkanContext_.reset();
        }
        setDetail(
            "OHCodec native interop fallback passed; starting independent "
            "software-decode fallback");
        startOHCodecFallbackPhase(
            MobileHardwareFrameFallbackRoute::SoftwareDecode);
    }

    void finalizeOHCodecSoftwareFallback()
    {
        std::shared_ptr<MobileVideoRendererSelector> selector;
        std::shared_ptr<OHCodecVulkanInterop> vulkanInterop;
        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            selector = ohCodecFallbackSelector_;
            vulkanInterop = ohCodecFallbackVulkanInterop_;
        }
        if (!selector || !vulkanInterop) {
            failOHCodecFallback(
                "OHCodec software fallback finalization lost its "
                "selector or Vulkan interop");
            return;
        }

        player_.setState(State::Stopped);
        if (!player_.waitFor(State::Stopped, 4'000)) {
            failOHCodecFallback(
                "OHCodec software fallback stop did not complete");
            return;
        }
        clearOHCodecFallbackFrames();
        player_.setRenderCallback({})
            .setVideoRenderAPI({})
            .setVideoFrameScheduler({})
            .setHardwareDecodeConfig({});
        const MobileRenderAPI selected = selector->selectedAPI();
        const MobileHardwareFrameFallbackRoute appliedRoute =
            selector->hardwareFrameFallbackRoute();
        selector->setEventCallback({});
        selector->setSelectionCallback({});
        selector->setHardwareFrameFallbackCallback({});
        selector->close();
        const OHCodecVulkanInteropStatistics vulkanStatistics =
            vulkanInterop->statistics();
        const std::uint64_t softwareRendered =
            ohCodecFallbackSoftwarePresented_.load(std::memory_order_relaxed);
        const bool passed =
            selected == MobileRenderAPI::OpenGLES &&
            appliedRoute == MobileHardwareFrameFallbackRoute::SoftwareDecode &&
            ohCodecFallbackVulkanSelected_.load(std::memory_order_acquire) &&
            ohCodecFallbackObserved_.load(std::memory_order_acquire) &&
            ohCodecFallbackRouteApplied_.load(std::memory_order_acquire) &&
            ohCodecFallbackMediaOpenAtRebind_.load(std::memory_order_relaxed) ==
                8 &&
            mediaOpenCount_.load(std::memory_order_relaxed) == 8 &&
            ohCodecFallbackHardwareInputs_.load(std::memory_order_relaxed) >
                0 &&
            ohCodecFallbackSoftwareInputs_.load(std::memory_order_relaxed) >=
                softwareRendered &&
            softwareRendered >= RequiredOHCodecSoftwareFallbackFrames &&
            ohCodecFallbackOpenGLInterop_ == nullptr &&
            ohCodecFallbackMaximumScheduledFrames_.load(
                std::memory_order_relaxed) > 0 &&
            ohCodecFallbackMaximumScheduledFrames_.load(
                std::memory_order_relaxed) <=
                OHCodecFallbackMaximumScheduledFrames &&
            vulkanStatistics.nativeBuffersAcquired > 0 &&
            (vulkanStatistics.directPlaneImports > 0 ||
             vulkanStatistics.opaqueExternalImports > 0 ||
             vulkanStatistics.externalFormatWorkaroundImports > 0 ||
             vulkanStatistics.opaqueFormatsRejected > 0) &&
            vulkanStatistics.unsupportedFormatsRejected == 0 &&
            vulkanStatistics.normalizationPasses <=
                vulkanStatistics.opaqueExternalImports +
                    vulkanStatistics.externalFormatWorkaroundImports &&
            vulkanStatistics.cpuMapCalls == 0 &&
            vulkanStatistics.softwareTransferCalls == 0 &&
            vulkanStatistics.stagingCopies == 0 &&
            vulkanStatistics.rendererUploads == 0 &&
            player_.state() == State::Stopped;
        if (!passed) {
            failOHCodecFallback(
                "OHCodec software-decode fallback counters did not "
                "prove an independent route");
            return;
        }

        const std::string fallbackResult =
            "QTAV_OHOS_OHCODEC_SOFTWARE_FALLBACK_RESULT PASS"
            " hardwareInputs=" +
            std::to_string(ohCodecFallbackHardwareInputs_.load(
                std::memory_order_relaxed)) +
            " softwareInputs=" +
            std::to_string(ohCodecFallbackSoftwareInputs_.load(
                std::memory_order_relaxed)) +
            " softwareRendered=" + std::to_string(softwareRendered) +
            " selectorDiscards=" +
            std::to_string(ohCodecFallbackSelectorDiscards_.load(
                std::memory_order_relaxed)) +
            " mediaOpen=8 cpuMap=0 transfer=0 staging=0 upload=0";
        logMessage(LOG_INFO, fallbackResult);

        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            ohCodecFallbackSelector_.reset();
            ohCodecFallbackVulkanRenderer_.reset();
            ohCodecFallbackOpenGLRenderer_.reset();
            ohCodecFallbackVulkanInterop_.reset();
            ohCodecFallbackOpenGLInterop_.reset();
            vulkanContext_.reset();
        }
        ohCodecFallbackPhase_.store(OHCodecFallbackPhase::Complete,
                                    std::memory_order_release);
        phase_.store(ValidationPhase::Complete, std::memory_order_release);
        passed_.store(true, std::memory_order_release);
        bool expected = false;
        if (!passLogged_.compare_exchange_strong(expected,
                                                 true,
                                                 std::memory_order_acq_rel)) {
            return;
        }

        const bool directVulkan =
            ohCodecVulkanDirectPassed_.load(std::memory_order_acquire);
        const std::string message =
            "QTAV_OHOS_RESULT PASS software selector initialGLES=" +
            std::to_string(
                initialFallbackFrames_.load(std::memory_order_relaxed)) +
            " fatalVulkan=" + std::to_string(InjectFatalAfterVulkanFrames) +
            " fatalGLES=" +
            std::to_string(
                fatalFallbackFrames_.load(std::memory_order_relaxed)) +
            " mediaOpen=8 ohcodecWrapper=ohcodec"
            " ohcodecLifecycle=pass ohcodecOpenGLLifecycle=pass"
            " ohcodecVulkanLifecycle=" +
            std::string(directVulkan ? "pass" : "unsupported-opaque") +
            " ohcodecVulkanDirectPlanes=" +
            std::to_string(ohCodecVulkanFinalStatistics_.directPlaneImports) +
            " ohcodecVulkanReleasedAfterGpu=" +
            std::to_string(
                ohCodecVulkanFinalStatistics_.outputsReleasedAfterGpu) +
            " ohcodecOpenGLRawYcbcr=" +
            std::to_string(ohCodecOpenGLFinalStatistics_.rawYcbcrImages) +
            " ohcodecDoviReleased=" +
            std::to_string(
                ohCodecOpenGLFinalStatistics_.dolbyVisionFramesReleased) +
            " ohcodecNativeFallback=pass"
            " ohcodecSoftwareFallback=pass" +
            audioResultText();
        setDetail(message);
        logMessage(LOG_INFO, message);
    }

    void failOHCodecFallback(std::string detail)
    {
        fail(std::move(detail));
        if (phase_.load(std::memory_order_acquire) !=
            ValidationPhase::OHCodecFallback) {
            return;
        }
        bool expected = false;
        if (ohCodecFallbackAbortQueued_.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
            queueOHCodecControlAction(OHCodecControlAction::AbortFallback);
        }
    }

    void abortOHCodecFallbackValidation()
    {
        player_.setState(State::Stopped);
        player_.waitFor(State::Stopped, 4'000);
        clearOHCodecFallbackFrames();
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        teardownOHCodecFallbackLocked();
    }

    void teardownOHCodecFallbackLocked()
    {
        player_.setRenderCallback({})
            .setVideoRenderAPI({})
            .setVideoFrameScheduler({})
            .setHardwareDecodeConfig({});
        if (ohCodecFallbackSelector_) {
            ohCodecFallbackSelector_->setEventCallback({});
            ohCodecFallbackSelector_->setSelectionCallback({});
            ohCodecFallbackSelector_->setHardwareFrameFallbackCallback({});
            ohCodecFallbackSelector_->close();
        }
        std::deque<ScheduledOHCodecFallbackFrame> pending;
        {
            std::lock_guard<std::mutex> queueLock(ohCodecFallbackQueueMutex_);
            pending.swap(ohCodecFallbackScheduledFrames_);
        }
        for (const auto& scheduled : pending) {
            if (!scheduled.frame.hasHardwareFrame()) {
                continue;
            }
            const NativeHandle handle =
                scheduled.frame.hardwareFrame().nativeHandle(
                    HardwareHandleType::Surface);
            OHCodecSurface surface;
            if (ohCodecFallbackVulkanInterop_ &&
                handle.subresource ==
                    ohCodecFallbackVulkanInterop_->surface().generation()) {
                surface = ohCodecFallbackVulkanInterop_->surface();
            } else if (ohCodecFallbackOpenGLInterop_ &&
                       handle.subresource ==
                           ohCodecFallbackOpenGLInterop_->surface()
                               .generation()) {
                surface = ohCodecFallbackOpenGLInterop_->surface();
            }
            OHCodecFrame output = surface
                                      ? ohCodecFrame(scheduled.frame, surface)
                                      : OHCodecFrame{};
            if (output) {
                output.drop();
            }
        }
        ohCodecFallbackRenderQueued_.store(false, std::memory_order_release);
        ohCodecFallbackSelector_.reset();
        ohCodecFallbackVulkanRenderer_.reset();
        ohCodecFallbackOpenGLRenderer_.reset();
        ohCodecFallbackVulkanInterop_.reset();
        ohCodecFallbackOpenGLInterop_.reset();
        vulkanContext_.reset();
    }

    void failOHCodecVulkan(std::string detail)
    {
        fail(std::move(detail));
        if (phase_.load(std::memory_order_acquire)
            != ValidationPhase::OHCodecVulkan) {
            return;
        }
        bool expected = false;
        if (ohCodecVulkanAbortQueued_.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
            queueOHCodecControlAction(OHCodecControlAction::AbortVulkan);
        }
    }

    void abortOHCodecVulkanValidation()
    {
        player_.setState(State::Stopped);
        player_.waitFor(State::Stopped, 4'000);
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        teardownOHCodecVulkanLocked();
    }

    void teardownOHCodecVulkanLocked()
    {
        player_
            .setRenderCallback({})
            .setVideoRenderAPI({})
            .setVideoFrameScheduler({})
            .setHardwareDecodeConfig({});
        const OHCodecSurface surface = ohCodecVulkanInterop_
            ? ohCodecVulkanInterop_->surface()
            : OHCodecSurface {};
        clearOHCodecVulkanFrames(surface);
        if (ohCodecVulkanRenderer_) {
            ohCodecVulkanRenderer_->setEventCallback({});
            ohCodecVulkanRenderer_->close();
        }
        ohCodecVulkanRenderer_.reset();
        ohCodecVulkanInterop_.reset();
        vulkanContext_.reset();
    }

    void startPlayer()
    {
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        startPlayerLocked();
    }

    void startPlayerLocked()
    {
        if (!surfaceReady_ || !mediaReady_) {
            return;
        }
        if (!mediaStarted_) {
            decodedFrames_.store(0, std::memory_order_relaxed);
            decodedAudioFrames_.store(0, std::memory_order_relaxed);
            audioClockSamples_.store(0, std::memory_order_relaxed);
            maximumAudioLatencyMilliseconds_.store(
                0,
                std::memory_order_relaxed);
            audioLifecycleRequested_.store(
                false,
                std::memory_order_release);
            mediaOpenCount_.store(1, std::memory_order_relaxed);
            passed_.store(false, std::memory_order_release);
            failed_.store(false, std::memory_order_release);
            passLogged_.store(false, std::memory_order_release);
            player_.setMedia(H264MediaPath);
            mediaStarted_ = true;
        }
        player_.setState(State::Playing);
    }

    void renderCurrentFrame()
    {
        const VideoRenderResult result = player_.renderVideoDetailed();
        if (result.status == VideoRenderStatus::Rendered) {
            observeAudioClock();
            const ValidationPhase phase =
                phase_.load(std::memory_order_acquire);
            const MobileRenderAPI api =
                lastSelectedAPI_.load(std::memory_order_acquire);
            if (phase == ValidationPhase::InitialFallback
                && api == MobileRenderAPI::OpenGLES) {
                const std::uint64_t presented =
                    initialFallbackFrames_.fetch_add(
                        1,
                        std::memory_order_relaxed)
                    + 1;
                if (presented >= RequiredInitialFallbackFrames
                    && initialFallbackObserved_.load(
                        std::memory_order_acquire)) {
                    requestFatalFallbackSession();
                }
            } else if (phase == ValidationPhase::FatalFallback
                       && api == MobileRenderAPI::OpenGLES) {
                const std::uint64_t presented =
                    fatalFallbackFrames_.fetch_add(
                        1,
                        std::memory_order_relaxed)
                    + 1;
                if (presented >= RequiredFatalFallbackFrames
                    && fatalVulkanSelected_.load(
                        std::memory_order_acquire)
                    && fatalFallbackObserved_.load(
                        std::memory_order_acquire)
                    && mediaOpenCount_.load(std::memory_order_relaxed)
                        == 1
                    && audioValidationComplete()) {
                    requestOHCodecSession();
                }
            }
        } else if ((result.status == VideoRenderStatus::SurfaceLost
                    || result.status == VideoRenderStatus::RendererError)
                   && phase_.load(std::memory_order_acquire)
                       != ValidationPhase::SwitchingSession
                   && phase_.load(std::memory_order_acquire)
                       != ValidationPhase::SwitchingToOHCodec) {
            fail(
                result.detail.empty()
                    ? "The OHOS mobile render attempt failed"
                    : result.detail);
        }
    }

    void releaseSurfaceLocked()
    {
        if (!surfaceReady_ && !selector_ && !vulkanContext_
            && !ohCodecSurface_ && !ohCodecOpenGLRenderer_
            && !ohCodecOpenGLInterop_ && !ohCodecVulkanRenderer_
            && !ohCodecVulkanInterop_ && !ohCodecFallbackSelector_
            && !ohCodecFallbackOpenGLInterop_
            && !ohCodecFallbackVulkanInterop_) {
            return;
        }
        const bool ohCodecOpenGLActive =
            phase_.load(std::memory_order_acquire)
                == ValidationPhase::OHCodecOpenGL
            && ohCodecOpenGLPhase_.load(std::memory_order_acquire)
                != OHCodecOpenGLPhase::Complete;
        const bool ohCodecVulkanActive =
            phase_.load(std::memory_order_acquire)
                == ValidationPhase::OHCodecVulkan
            && ohCodecVulkanPhase_.load(std::memory_order_acquire)
                != OHCodecVulkanPhase::Complete;
        const bool ohCodecFallbackActive =
            phase_.load(std::memory_order_acquire)
                == ValidationPhase::OHCodecFallback
            && ohCodecFallbackPhase_.load(std::memory_order_acquire)
                != OHCodecFallbackPhase::Complete;
        const auto lifecycle = ohCodecLifecycle_.load(
            std::memory_order_acquire);
        const bool ohCodecSurfaceRecreation =
            phase_.load(std::memory_order_acquire)
                == ValidationPhase::OHCodec
            && (lifecycle
                    == OHCodecLifecyclePhase::H264BackgroundPending
                || lifecycle
                    == OHCodecLifecyclePhase::H264WaitingForSurface);
        if (mediaStarted_ && player_.state() == State::Playing) {
            player_.setState(State::Paused);
        }
        if (!releaseRetainedOHCodecOutput(false)) {
            fail("OHCodec retained output could not be dropped on surface loss");
        }
        player_.setRenderCallback({}).setVideoRenderAPI({});
        player_
            .setVideoFrameScheduler({})
            .setHardwareDecodeConfig({});
        if (ohCodecOpenGLRenderer_ || ohCodecOpenGLInterop_) {
            teardownOHCodecOpenGLLocked();
        }
        if (ohCodecVulkanRenderer_ || ohCodecVulkanInterop_) {
            teardownOHCodecVulkanLocked();
        }
        if (ohCodecFallbackSelector_
            || ohCodecFallbackOpenGLInterop_
            || ohCodecFallbackVulkanInterop_) {
            teardownOHCodecFallbackLocked();
        }
        if (selector_) {
            selector_->suspendSurface();
            selector_->close();
            selector_.reset();
        }
        vulkanContext_.reset();
        ohCodecSurface_.reset();
        ohCodecSurfaceGeneration_.store(0, std::memory_order_release);
        window_ = nullptr;
        renderConfig_ = {};
        lastSelectedAPI_.store(
            MobileRenderAPI::None,
            std::memory_order_release);
        surfaceReady_ = false;
        if (ohCodecSurfaceRecreation) {
            surfaceDestroyedCount_.fetch_add(
                1,
                std::memory_order_relaxed);
            ohCodecLifecycle_.store(
                OHCodecLifecyclePhase::H264WaitingForSurface,
                std::memory_order_release);
            const std::string detail =
                "QTAV_OHOS_CHECKPOINT OHCODEC_SURFACE_REMOVED oldGeneration="
                + std::to_string(
                    oldOHCodecSurfaceGeneration_.load(
                        std::memory_order_relaxed));
            setDetail(detail);
            logMessage(LOG_INFO, detail);
        } else if (ohCodecOpenGLActive) {
            failOHCodecOpenGL(
                "The XComponent surface was released during OHCodec/OpenGL interop validation");
        } else if (ohCodecVulkanActive) {
            failOHCodecVulkan(
                "The XComponent surface was released during OHCodec/Vulkan interop validation");
        } else if (ohCodecFallbackActive) {
            failOHCodecFallback(
                "The XComponent surface was released during OHCodec selector fallback validation");
        } else {
            setDetail("XComponent surface released; playback paused");
        }
    }

    void observeAudioClock()
    {
        if (!audioSink_) {
            return;
        }
        const AudioSinkClock clock = audioSink_->clock();
        if (!clock.valid) {
            return;
        }
        audioClockSamples_.fetch_add(1, std::memory_order_relaxed);
        auto previous = maximumAudioLatencyMilliseconds_.load(
            std::memory_order_relaxed);
        while (previous < clock.latencyMilliseconds
               && !maximumAudioLatencyMilliseconds_.compare_exchange_weak(
                   previous,
                   clock.latencyMilliseconds,
                   std::memory_order_relaxed)) {
        }
    }

    bool audioValidationComplete() const
    {
        if (!audioSink_
            || !audioLifecycleRequested_.load(std::memory_order_acquire)
            || decodedAudioFrames_.load(std::memory_order_relaxed) == 0
            || audioClockSamples_.load(std::memory_order_relaxed) == 0) {
            return false;
        }
        const OHAudioStreamInfo info = audioSink_->streamInfo();
        return info.callbackFrames > 0
            && info.callbackCount > 0
            && info.renderedPcmFrames > 0
            && info.starts >= 2
            && info.flushes >= 1
            && info.drains >= 1;
    }

    std::string audioResultText() const
    {
        const OHAudioStreamInfo info = audioSink_->streamInfo();
        std::ostringstream result;
        result << " audioDecoded="
               << decodedAudioFrames_.load(std::memory_order_relaxed)
               << " audioRendered=" << info.renderedPcmFrames
               << " audioClock="
               << audioClockSamples_.load(std::memory_order_relaxed)
               << " audioLatencyMs="
               << maximumAudioLatencyMilliseconds_.load(
                      std::memory_order_relaxed)
               << " audioStarts=" << info.starts
               << " audioFlushes=" << info.flushes
               << " audioDrains=" << info.drains
               << " audioRestarts=" << info.streamRestarts;
        return result.str();
    }

    void setDetail(std::string detail)
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        detail_ = std::move(detail);
    }

    void fail(std::string detail)
    {
        failed_.store(true, std::memory_order_release);
        passed_.store(false, std::memory_order_release);
        setDetail(detail);
        logMessage(LOG_ERROR, "QTAV_OHOS_RESULT FAIL " + detail);
    }

    mutable std::mutex pipelineMutex_;
    mutable std::mutex statusMutex_;
    std::mutex controlMutex_;
    std::mutex retainedOHCodecMutex_;
    std::mutex ohCodecOpenGLQueueMutex_;
    std::mutex ohCodecVulkanQueueMutex_;
    std::mutex ohCodecFallbackQueueMutex_;
    std::condition_variable controlCondition_;
    std::thread controlWorker_;
    Player player_;
    std::shared_ptr<OHAudioAudioSink> audioSink_;
    std::unique_ptr<OHCodecSurface> ohCodecSurface_;
    std::unique_ptr<OHCodecFrame> retainedOHCodecOutput_;
    VideoFrame staleOHCodecFrame_;
    OHCodecOpenGLInteropStatistics ohCodecOpenGLH264Statistics_;
    OHCodecOpenGLInteropStatistics ohCodecOpenGLFinalStatistics_;
    OHCodecOpenGLInteropStatistics
        ohCodecFallbackOpenGLFinalStatistics_;
    OHCodecVulkanInteropStatistics ohCodecVulkanFinalStatistics_;
    std::unique_ptr<OHOSVulkanContext> vulkanContext_;
    std::unique_ptr<VVCValidationSession> vvcValidation_;
    std::shared_ptr<MobileVideoRendererSelector> selector_;
    std::shared_ptr<OHCodecOpenGLInterop> ohCodecOpenGLInterop_;
    std::shared_ptr<OHOSOpenGLVideoRenderer> ohCodecOpenGLRenderer_;
    std::shared_ptr<OHCodecVulkanInterop> ohCodecVulkanInterop_;
    std::shared_ptr<OHOSVulkanVideoRenderer> ohCodecVulkanRenderer_;
    std::shared_ptr<MobileVideoRendererSelector>
        ohCodecFallbackSelector_;
    std::shared_ptr<OHCodecOpenGLInterop>
        ohCodecFallbackOpenGLInterop_;
    std::shared_ptr<OHOSOpenGLVideoRenderer>
        ohCodecFallbackOpenGLRenderer_;
    std::shared_ptr<OHCodecVulkanInterop>
        ohCodecFallbackVulkanInterop_;
    std::shared_ptr<OHOSVulkanVideoRenderer>
        ohCodecFallbackVulkanRenderer_;
    std::deque<ScheduledOHCodecOpenGLFrame>
        ohCodecOpenGLScheduledFrames_;
    std::deque<ScheduledOHCodecVulkanFrame>
        ohCodecVulkanScheduledFrames_;
    std::deque<ScheduledOHCodecFallbackFrame>
        ohCodecFallbackScheduledFrames_;
    OHNativeWindow* window_ = nullptr;
    VideoRenderConfig renderConfig_;
    MediaStatus mediaStatus_ = MediaStatus::NoMedia;
    std::string detail_ = "Waiting for XComponent and packaged media";
    std::atomic<std::uint64_t> decodedFrames_ { 0 };
    std::atomic<std::uint64_t> decodedAudioFrames_ { 0 };
    std::atomic<std::uint64_t> audioClockSamples_ { 0 };
    std::atomic<std::int64_t> maximumAudioLatencyMilliseconds_ { 0 };
    std::atomic<std::uint64_t> initialFallbackFrames_ { 0 };
    std::atomic<std::uint64_t> fatalFallbackFrames_ { 0 };
    std::atomic<std::uint64_t> ohCodecFrames_ { 0 };
    std::atomic<std::uint64_t> ohCodecDroppedFrames_ { 0 };
    std::atomic<std::uint64_t> ohCodecOutputs_ { 0 };
    std::atomic<std::uint64_t> ohCodecStageOutputs_ { 0 };
    std::atomic<std::uint64_t> ohCodecStagePresented_ { 0 };
    std::atomic<std::uint64_t> ohCodecStageDropped_ { 0 };
    std::atomic<std::uint64_t> h264PresentedFrames_ { 0 };
    std::atomic<std::uint64_t> h264DroppedFrames_ { 0 };
    std::atomic<std::uint64_t> hevcPresentedFrames_ { 0 };
    std::atomic<std::uint64_t> hevcDroppedFrames_ { 0 };
    std::atomic<std::uint64_t> pendingOHCodecOutputs_ { 0 };
    std::atomic<std::uint64_t> maximumPendingOHCodecOutputs_ { 0 };
    std::atomic<std::uint64_t> pendingOHCodecOutputsAtStop_ { 0 };
    std::atomic<std::uint64_t> retainedOHCodecOutputsReleased_ { 0 };
    std::atomic<std::uint64_t> transitionDroppedFrames_ { 0 };
    std::atomic<std::uint64_t> pauseResumeCount_ { 0 };
    std::atomic<std::uint64_t> seekCount_ { 0 };
    std::atomic<std::uint64_t> mediaReplacementCount_ { 0 };
    std::atomic<std::uint64_t> explicitStopCount_ { 0 };
    std::atomic<std::uint64_t> backgroundTransitions_ { 0 };
    std::atomic<std::uint64_t> foregroundTransitions_ { 0 };
    std::atomic<std::uint64_t> surfaceDestroyedCount_ { 0 };
    std::atomic<std::uint64_t> surfaceRecreatedCount_ { 0 };
    std::atomic<std::uint64_t> staleSurfaceRejections_ { 0 };
    std::atomic<std::uint32_t> ohCodecSurfaceGeneration_ { 0 };
    std::atomic<std::uint32_t> oldOHCodecSurfaceGeneration_ { 0 };
    std::atomic<std::uint32_t> newOHCodecSurfaceGeneration_ { 0 };
    std::atomic<std::uint64_t> ohCodecOpenGLH264Rendered_ { 0 };
    std::atomic<std::uint64_t> ohCodecOpenGLHEVCRendered_ { 0 };
    std::atomic<std::uint64_t> ohCodecOpenGLStageRendered_ { 0 };
    std::atomic<std::uint64_t> ohCodecOpenGLDeferred_ { 0 };
    std::atomic<std::uint64_t> ohCodecOpenGLRedrawCallbacks_ { 0 };
    std::atomic<std::uint64_t> ohCodecOpenGLSeekCount_ { 0 };
    std::atomic<std::uint64_t> ohCodecOpenGLFlushCount_ { 0 };
    std::atomic<std::uint64_t> ohCodecOpenGLMediaReplacementCount_ { 0 };
    std::atomic<std::uint64_t> ohCodecOpenGLStopCount_ { 0 };
    std::atomic<std::uint64_t> ohCodecOpenGLNextFrameSerial_ { 1 };
    std::atomic<std::uint64_t> ohCodecOpenGLMaximumScheduledFrames_ {
        0
    };
    std::atomic<std::uint64_t> ohCodecOpenGLSchedulerDrops_ { 0 };
    std::atomic<std::uint64_t> ohCodecOpenGLRendererDiscards_ { 0 };
    std::atomic<std::uint32_t> ohCodecOpenGLSurfaceGeneration_ { 0 };
    std::atomic<std::uint64_t> ohCodecOpenGLRendererGeneration_ { 0 };
    std::atomic<std::uint64_t> ohCodecVulkanH264Rendered_ { 0 };
    std::atomic<std::uint64_t> ohCodecVulkanHEVCRendered_ { 0 };
    std::atomic<std::uint64_t> ohCodecVulkanStageRendered_ { 0 };
    std::atomic<std::uint64_t> ohCodecVulkanNextFrameSerial_ { 1 };
    std::atomic<std::uint64_t> ohCodecVulkanMaximumScheduledFrames_ { 0 };
    std::atomic<std::uint64_t> ohCodecVulkanSchedulerDrops_ { 0 };
    std::atomic<std::uint64_t> ohCodecVulkanRendererDiscards_ { 0 };
    std::atomic<std::uint32_t> ohCodecVulkanSurfaceGeneration_ { 0 };
    std::atomic<std::uint64_t> ohCodecFallbackVulkanPresented_ { 0 };
    std::atomic<std::uint64_t> ohCodecFallbackOpenGLPresented_ { 0 };
    std::atomic<std::uint64_t> ohCodecFallbackSoftwarePresented_ { 0 };
    std::atomic<std::uint64_t> ohCodecFallbackHardwareInputs_ { 0 };
    std::atomic<std::uint64_t> ohCodecFallbackSoftwareInputs_ { 0 };
    std::atomic<std::uint64_t> ohCodecFallbackSelectorDiscards_ { 0 };
    std::atomic<std::uint64_t> ohCodecFallbackSchedulerDrops_ { 0 };
    std::atomic<std::uint64_t> ohCodecFallbackNextFrameSerial_ { 1 };
    std::atomic<std::uint64_t>
        ohCodecFallbackMaximumScheduledFrames_ { 0 };
    std::atomic<std::uint64_t> ohCodecFallbackMediaOpenAtRebind_ { 0 };
    std::atomic<std::uint32_t>
        ohCodecFallbackVulkanSurfaceGeneration_ { 0 };
    std::atomic<std::uint32_t>
        ohCodecFallbackOpenGLSurfaceGeneration_ { 0 };
    std::atomic<std::uint64_t> mediaOpenCount_ { 0 };
    std::atomic<ValidationPhase> phase_ {
        ValidationPhase::InitialFallback
    };
    std::atomic<OHCodecLifecyclePhase> ohCodecLifecycle_ {
        OHCodecLifecyclePhase::Inactive
    };
    std::atomic<OHCodecOpenGLPhase> ohCodecOpenGLPhase_ {
        OHCodecOpenGLPhase::Inactive
    };
    std::atomic<OHCodecVulkanPhase> ohCodecVulkanPhase_ {
        OHCodecVulkanPhase::Inactive
    };
    std::atomic<OHCodecFallbackPhase> ohCodecFallbackPhase_ {
        OHCodecFallbackPhase::Inactive
    };
    std::atomic<MobileHardwareFrameFallbackRoute>
        ohCodecFallbackRoute_ {
            MobileHardwareFrameFallbackRoute::None
        };
    std::atomic<MobileRenderAPI> lastSelectedAPI_ {
        MobileRenderAPI::None
    };
    std::atomic<bool> passed_ { false };
    std::atomic<bool> failed_ { false };
    std::atomic<bool> passLogged_ { false };
    std::atomic<bool> initialFallbackObserved_ { false };
    std::atomic<bool> fatalVulkanSelected_ { false };
    std::atomic<bool> fatalFallbackObserved_ { false };
    std::atomic<bool> transitionQueued_ { false };
    std::atomic<bool> ohCodecTransitionQueued_ { false };
    std::atomic<bool> ohCodecOpenGLAbortQueued_ { false };
    std::atomic<bool> ohCodecOpenGLRenderQueued_ { false };
    std::atomic<bool> ohCodecVulkanAbortQueued_ { false };
    std::atomic<bool> ohCodecVulkanRenderQueued_ { false };
    std::atomic<bool> ohCodecVulkanDirectPassed_ { false };
    std::atomic<bool> ohCodecFallbackVulkanSelected_ { false };
    std::atomic<bool> ohCodecFallbackObserved_ { false };
    std::atomic<bool> ohCodecFallbackRouteApplied_ { false };
    std::atomic<bool> ohCodecFallbackAbortQueued_ { false };
    std::atomic<bool> ohCodecFallbackRenderQueued_ { false };
    std::atomic<bool> ohCodecFallbackFinishQueued_ { false };
    std::atomic<bool> audioLifecycleRequested_ { false };
    std::deque<OHCodecControlAction> ohCodecControlActions_;
    bool controlQuit_ = false;
    bool transitionRequested_ = false;
    bool ohCodecTransitionRequested_ = false;
    bool surfaceReady_ = false;
    bool mediaReady_ = false;
    bool mediaStarted_ = false;
};

AppSession& session()
{
    static AppSession value;
    return value;
}

void onSurfaceCreated(
    OH_NativeXComponent* component,
    void* window)
{
    session().createSurface(
        component,
        static_cast<OHNativeWindow*>(window));
}

void onSurfaceChanged(
    OH_NativeXComponent* component,
    void* window)
{
    session().changeSurface(
        component,
        static_cast<OHNativeWindow*>(window));
}

void onSurfaceDestroyed(OH_NativeXComponent*, void*)
{
    session().releaseSurface();
}

void dispatchTouchEvent(OH_NativeXComponent*, void*)
{
}

OH_NativeXComponent_Callback XComponentCallbacks {
    onSurfaceCreated,
    onSurfaceChanged,
    onSurfaceDestroyed,
    dispatchTouchEvent,
};

napi_value booleanValue(napi_env env, bool value)
{
    napi_value result = nullptr;
    napi_get_boolean(env, value, &result);
    return result;
}

napi_value undefinedValue(napi_env env)
{
    napi_value result = nullptr;
    napi_get_undefined(env, &result);
    return result;
}

bool byteArrayView(
    napi_env env,
    napi_value value,
    const std::uint8_t*& data,
    std::size_t& size)
{
    bool isTypedArray = false;
    if (napi_is_typedarray(env, value, &isTypedArray) != napi_ok
        || !isTypedArray) {
        return false;
    }
    napi_typedarray_type type = napi_int8_array;
    void* rawData = nullptr;
    napi_value arrayBuffer = nullptr;
    std::size_t byteOffset = 0;
    if (napi_get_typedarray_info(
            env,
            value,
            &type,
            &size,
            &rawData,
            &arrayBuffer,
            &byteOffset)
            != napi_ok
        || (type != napi_uint8_array
            && type != napi_uint8_clamped_array)) {
        return false;
    }
    data = static_cast<const std::uint8_t*>(rawData);
    return data && size > 0;
}

napi_value start(napi_env env, napi_callback_info info)
{
    std::size_t argumentCount = 3;
    napi_value arguments[3] { nullptr, nullptr, nullptr };
    if (napi_get_cb_info(
            env,
            info,
            &argumentCount,
            arguments,
            nullptr,
            nullptr)
            != napi_ok
        || argumentCount != 3) {
        return booleanValue(env, false);
    }

    const std::uint8_t* h264Data = nullptr;
    const std::uint8_t* hevcData = nullptr;
    const std::uint8_t* vvcData = nullptr;
    std::size_t h264Size = 0;
    std::size_t hevcSize = 0;
    std::size_t vvcSize = 0;
    if (!byteArrayView(
            env,
            arguments[0],
            h264Data,
            h264Size)
        || !byteArrayView(
            env,
            arguments[1],
            hevcData,
            hevcSize)
        || !byteArrayView(
            env,
            arguments[2],
            vvcData,
            vvcSize)) {
        return booleanValue(env, false);
    }
    return booleanValue(
        env,
        session().startMedia(
            h264Data,
            h264Size,
            hevcData,
            hevcSize,
            vvcData,
            vvcSize));
}

napi_value stop(napi_env env, napi_callback_info)
{
    session().stop();
    return undefinedValue(env);
}

napi_value status(napi_env env, napi_callback_info)
{
    const std::string value = session().status();
    napi_value result = nullptr;
    napi_create_string_utf8(env, value.c_str(), value.size(), &result);
    return result;
}

napi_value setForeground(napi_env env, napi_callback_info info)
{
    std::size_t argumentCount = 1;
    napi_value arguments[1] { nullptr };
    bool foreground = false;
    if (napi_get_cb_info(
            env,
            info,
            &argumentCount,
            arguments,
            nullptr,
            nullptr)
            != napi_ok
        || argumentCount != 1
        || napi_get_value_bool(env, arguments[0], &foreground)
            != napi_ok) {
        return undefinedValue(env);
    }
    session().setForeground(foreground);
    return undefinedValue(env);
}

napi_value init(napi_env env, napi_value exports)
{
    napi_property_descriptor properties[] {
        {
            "start",
            nullptr,
            start,
            nullptr,
            nullptr,
            nullptr,
            napi_default,
            nullptr,
        },
        {
            "stop",
            nullptr,
            stop,
            nullptr,
            nullptr,
            nullptr,
            napi_default,
            nullptr,
        },
        {
            "status",
            nullptr,
            status,
            nullptr,
            nullptr,
            nullptr,
            napi_default,
            nullptr,
        },
        {
            "setForeground",
            nullptr,
            setForeground,
            nullptr,
            nullptr,
            nullptr,
            napi_default,
            nullptr,
        },
    };
    napi_define_properties(
        env,
        exports,
        sizeof(properties) / sizeof(properties[0]),
        properties);

    napi_value exportInstance = nullptr;
    OH_NativeXComponent* component = nullptr;
    if (napi_get_named_property(
            env,
            exports,
            OH_NATIVE_XCOMPONENT_OBJ,
            &exportInstance)
            == napi_ok
        && napi_unwrap(
               env,
               exportInstance,
               reinterpret_cast<void**>(&component))
            == napi_ok
        && component) {
        const int32_t result = OH_NativeXComponent_RegisterCallback(
            component,
            &XComponentCallbacks);
        if (result != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
            logMessage(
                LOG_ERROR,
                "QTAV_OHOS_RESULT FAIL could not register XComponent callbacks");
        }
    } else {
        logMessage(
            LOG_ERROR,
            "QTAV_OHOS_RESULT FAIL could not unwrap Native XComponent");
    }
    return exports;
}

} // namespace
} // namespace qtav::ohos_example

NAPI_MODULE(entry, qtav::ohos_example::init)
