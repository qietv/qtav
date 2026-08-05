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
constexpr const char* MediaPath =
    "/data/storage/el2/base/files/qtav-ohos-test.mp4";
constexpr std::uint64_t RequiredInitialFallbackFrames = 20;
constexpr std::uint64_t InjectFatalAfterVulkanFrames = 12;
constexpr std::uint64_t RequiredFatalFallbackFrames = 30;
constexpr std::uint64_t RequiredOHCodecFrames = 30;

enum class ValidationPhase {
    InitialFallback,
    SwitchingSession,
    FatalFallback,
    SwitchingToOHCodec,
    OHCodec,
    Complete,
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

    bool startMedia(const std::uint8_t* data, std::size_t size)
    {
        if (!data || size == 0) {
            fail("The packaged test media is empty");
            return false;
        }
        std::ofstream output(
            MediaPath,
            std::ios::binary | std::ios::trunc);
        if (!output
            || !output.write(
                reinterpret_cast<const char*>(data),
                static_cast<std::streamsize>(size))) {
            fail("Could not write the packaged test media to app storage");
            return false;
        }
        output.close();

        bool shouldStart = false;
        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            mediaReady_ = true;
            shouldStart = surfaceReady_;
        }
        setDetail(
            "Packaged media ready (" + std::to_string(size)
            + " bytes)");
        if (shouldStart) {
            startPlayer();
        }
        return true;
    }

    void stop()
    {
        player_.setState(State::Stopped);
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        mediaStarted_ = false;
        mediaReady_ = false;
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
        releaseSurfaceLocked();
        window_ = window;
        renderConfig_ = {};
        renderConfig_.surfaceSize = {
            static_cast<int>(width),
            static_cast<int>(height),
        };
        renderConfig_.aspectRatio = VideoAspectRatioMode::Fit;
        phase_.store(
            ValidationPhase::InitialFallback,
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
            {
                std::unique_lock<std::mutex> lock(controlMutex_);
                controlCondition_.wait(lock, [this] {
                    return controlQuit_ || transitionRequested_
                        || ohCodecTransitionRequested_;
                });
                if (controlQuit_) {
                    return;
                }
                if (ohCodecTransitionRequested_) {
                    ohCodecTransitionRequested_ = false;
                    startOHCodec = true;
                } else {
                    transitionRequested_ = false;
                }
            }

            std::lock_guard<std::mutex> lock(pipelineMutex_);
            if (!surfaceReady_ || !mediaStarted_
                || (!startOHCodec && !selector_)) {
                continue;
            }
            if (startOHCodec) {
                startOHCodecLocked();
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
        const OHCodecSurface schedulerSurface = *surface;
        ohCodecSurface_ = std::move(surface);
        phase_.store(
            ValidationPhase::OHCodec,
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
    }

    bool consumeOHCodecFrame(
        const VideoFrame& frame,
        std::int64_t monotonicNanoseconds,
        const OHCodecSurface& surfaceToken)
    {
        if (phase_.load(std::memory_order_acquire)
            != ValidationPhase::OHCodec
            || !frame.hasHardwareFrame()) {
            return false;
        }
        const auto generation =
            ohCodecSurfaceGeneration_.load(std::memory_order_acquire);
        auto output = ohCodecFrame(frame, surfaceToken);
        if (!output || output.surfaceGeneration() != generation) {
            fail("OHCodec produced a foreign or stale hardware output");
            return false;
        }

        const std::uint64_t ordinal =
            ohCodecOutputs_.fetch_add(1, std::memory_order_relaxed) + 1;
        const bool shouldDrop = ordinal % 10 == 0;
        if (!(shouldDrop ? output.drop()
                         : output.presentAt(monotonicNanoseconds))) {
            fail(
                shouldDrop
                    ? "OHCodec explicit output drop failed"
                    : "OHCodec timed surface presentation failed");
            return false;
        }

        const std::uint64_t dropped = shouldDrop
            ? ohCodecDroppedFrames_.fetch_add(
                  1,
                  std::memory_order_relaxed)
                + 1
            : ohCodecDroppedFrames_.load(std::memory_order_relaxed);
        const std::uint64_t frames = shouldDrop
            ? ohCodecFrames_.load(std::memory_order_relaxed)
            : ohCodecFrames_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (frames >= RequiredOHCodecFrames && dropped >= 3) {
            phase_.store(
                ValidationPhase::Complete,
                std::memory_order_release);
            passed_.store(true, std::memory_order_release);
            bool expected = false;
            if (passLogged_.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel)) {
                const std::string message =
                    "QTAV_OHOS_RESULT PASS software selector initialGLES="
                    + std::to_string(
                        initialFallbackFrames_.load(
                            std::memory_order_relaxed))
                    + " fatalVulkan="
                    + std::to_string(InjectFatalAfterVulkanFrames)
                    + " fatalGLES="
                    + std::to_string(
                        fatalFallbackFrames_.load(
                            std::memory_order_relaxed))
                    + " mediaOpen=1 ohcodecWrapper=ohcodec"
                    + " ohcodecPresented=" + std::to_string(frames)
                    + " ohcodecDropped=" + std::to_string(dropped)
                    + " ohcodecGeneration="
                    + std::to_string(generation)
                    + audioResultText();
                setDetail(message);
                logMessage(LOG_INFO, message);
            }
        }
        return true;
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
            player_.setMedia(MediaPath);
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
        if (mediaStarted_ && player_.state() == State::Playing) {
            player_.setState(State::Paused);
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
        setDetail("XComponent surface released; playback paused");
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
    std::condition_variable controlCondition_;
    std::thread controlWorker_;
    Player player_;
    std::shared_ptr<OHAudioAudioSink> audioSink_;
    std::unique_ptr<OHCodecSurface> ohCodecSurface_;
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
    std::atomic<std::uint32_t> ohCodecSurfaceGeneration_ { 0 };
    std::atomic<std::uint64_t> mediaOpenCount_ { 0 };
    std::atomic<ValidationPhase> phase_ {
        ValidationPhase::InitialFallback
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

napi_value start(napi_env env, napi_callback_info info)
{
    std::size_t argumentCount = 1;
    napi_value arguments[1] { nullptr };
    if (napi_get_cb_info(
            env,
            info,
            &argumentCount,
            arguments,
            nullptr,
            nullptr)
            != napi_ok
        || argumentCount != 1) {
        return booleanValue(env, false);
    }

    bool isTypedArray = false;
    if (napi_is_typedarray(env, arguments[0], &isTypedArray) != napi_ok
        || !isTypedArray) {
        return booleanValue(env, false);
    }
    napi_typedarray_type type = napi_int8_array;
    std::size_t length = 0;
    void* data = nullptr;
    napi_value arrayBuffer = nullptr;
    std::size_t byteOffset = 0;
    if (napi_get_typedarray_info(
            env,
            arguments[0],
            &type,
            &length,
            &data,
            &arrayBuffer,
            &byteOffset)
            != napi_ok
        || (type != napi_uint8_array
            && type != napi_uint8_clamped_array)) {
        return booleanValue(env, false);
    }
    return booleanValue(
        env,
        session().startMedia(
            static_cast<const std::uint8_t*>(data),
            length));
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
