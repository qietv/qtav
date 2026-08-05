// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ohos_vulkan_context.h"

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>
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

enum class ValidationPhase {
    InitialFallback,
    SwitchingSession,
    FatalFallback,
    SwitchingToOHCodec,
    OHCodec,
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
    case ValidationPhase::Complete:
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
        std::size_t hevcSize)
    {
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
        player_.setState(State::Stopped);
        releaseRetainedOHCodecOutput(false);
        player_
            .setVideoFrameScheduler({})
            .setHardwareDecodeConfig({});
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        mediaStarted_ = false;
        mediaReady_ = false;
    }

    void setForeground(bool foreground)
    {
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
        releaseSurfaceLocked();
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
        if (!selector_ || !window || window != window_) {
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
            renderConfig_.surfaceSize = {
                static_cast<int>(width),
                static_cast<int>(height),
            };
            renderConfig_.aspectRatio = VideoAspectRatioMode::Fit;
            selector_->suspendSurface();
            selector_->configure(renderConfig_);
            if (!selector_->recreateSurface()) {
                fail(
                    "The OHOS mobile selector could not follow the XComponent resize");
            }
        }
    }

    void releaseSurface()
    {
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        releaseSurfaceLocked();
    }

    std::string status() const
    {
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
        if (failed_.load(std::memory_order_acquire)) {
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
        phase_.store(
            ValidationPhase::Complete,
            std::memory_order_release);
        passed_.store(true, std::memory_order_release);
        bool expected = false;
        if (!passLogged_.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
            return;
        }

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

        const std::string message =
            "QTAV_OHOS_RESULT PASS software selector initialGLES="
            + std::to_string(
                initialFallbackFrames_.load(std::memory_order_relaxed))
            + " fatalVulkan="
            + std::to_string(InjectFatalAfterVulkanFrames)
            + " fatalGLES="
            + std::to_string(
                fatalFallbackFrames_.load(std::memory_order_relaxed))
            + " mediaOpen=2 ohcodecWrapper=ohcodec"
            + " ohcodecH264Presented="
            + std::to_string(
                h264PresentedFrames_.load(std::memory_order_relaxed))
            + " ohcodecHEVCPresented="
            + std::to_string(
                hevcPresentedFrames_.load(std::memory_order_relaxed))
            + " ohcodecDropped="
            + std::to_string(
                ohCodecDroppedFrames_.load(std::memory_order_relaxed))
            + " ohcodecLifecycle=pass" + audioResultText();
        setDetail(message);
        logMessage(LOG_INFO, message);
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
            && !ohCodecSurface_) {
            return;
        }
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
    std::condition_variable controlCondition_;
    std::thread controlWorker_;
    Player player_;
    std::shared_ptr<OHAudioAudioSink> audioSink_;
    std::unique_ptr<OHCodecSurface> ohCodecSurface_;
    std::unique_ptr<OHCodecFrame> retainedOHCodecOutput_;
    VideoFrame staleOHCodecFrame_;
    std::unique_ptr<OHOSVulkanContext> vulkanContext_;
    std::shared_ptr<MobileVideoRendererSelector> selector_;
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
    std::atomic<std::uint64_t> mediaOpenCount_ { 0 };
    std::atomic<ValidationPhase> phase_ {
        ValidationPhase::InitialFallback
    };
    std::atomic<OHCodecLifecyclePhase> ohCodecLifecycle_ {
        OHCodecLifecyclePhase::Inactive
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
    std::size_t argumentCount = 2;
    napi_value arguments[2] { nullptr, nullptr };
    if (napi_get_cb_info(
            env,
            info,
            &argumentCount,
            arguments,
            nullptr,
            nullptr)
            != napi_ok
        || argumentCount != 2) {
        return booleanValue(env, false);
    }

    const std::uint8_t* h264Data = nullptr;
    const std::uint8_t* hevcData = nullptr;
    std::size_t h264Size = 0;
    std::size_t hevcSize = 0;
    if (!byteArrayView(
            env,
            arguments[0],
            h264Data,
            h264Size)
        || !byteArrayView(
            env,
            arguments[1],
            hevcData,
            hevcSize)) {
        return booleanValue(env, false);
    }
    return booleanValue(
        env,
        session().startMedia(
            h264Data,
            h264Size,
            hevcData,
            hevcSize));
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
