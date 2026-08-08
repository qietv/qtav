// SPDX-License-Identifier: LGPL-2.1-or-later

#include "vvc_validation_session.h"

#include <hilog/log.h>
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avcodec_base.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include <qtav/ohcodec_hardware_decoder.h>
#include <qtav/player.h>

namespace qtav::ohos_example {
namespace {

constexpr std::uint32_t LogDomain = 0xD003900;
constexpr const char* LogTag = "QtAVCoreOHOS";
constexpr const char* VVCMediaPath =
    "/data/storage/el2/base/files/qtav-ohos-test-vvc.mp4";
constexpr std::uint64_t RequiredFullStreamFrames = 600;
constexpr std::uint64_t RequiredLifecycleFrames = 30;
constexpr std::uint64_t RequiredSoftwareFallbackFrames = 30;
constexpr std::int64_t SeekTargetMilliseconds = 4'000;

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

enum class Phase {
    Inactive,
    FullPlayback,
    LifecycleWarmup,
    PauseSeekPending,
    AfterSeek,
    SurfaceRemovalPending,
    WaitingForSurface,
    AfterSurfaceRecreation,
    StopPending,
    SoftwareFallback,
    FinishPending,
    Complete,
    Failed,
};

const char* phaseName(Phase phase) noexcept
{
    switch (phase) {
    case Phase::Inactive:
        return "inactive";
    case Phase::FullPlayback:
        return "full-playback";
    case Phase::LifecycleWarmup:
        return "lifecycle-warmup";
    case Phase::PauseSeekPending:
        return "pause-seek-pending";
    case Phase::AfterSeek:
        return "after-seek";
    case Phase::SurfaceRemovalPending:
        return "surface-removal-pending";
    case Phase::WaitingForSurface:
        return "waiting-for-surface";
    case Phase::AfterSurfaceRecreation:
        return "after-surface-recreation";
    case Phase::StopPending:
        return "stop-pending";
    case Phase::SoftwareFallback:
        return "software-fallback";
    case Phase::FinishPending:
        return "finish-pending";
    case Phase::Complete:
        return "complete";
    case Phase::Failed:
        return "failed";
    }
    return "unknown";
}

enum class Action {
    StartLifecycle,
    PauseResumeSeek,
    RequestSurfaceRecreation,
    StopHardware,
    Finish,
    Abort,
};

bool writeMediaFile(
    const std::uint8_t* data,
    std::size_t size)
{
    std::ofstream output(
        VVCMediaPath,
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

} // namespace

class VVCValidationSession::Impl final {
public:
    Impl()
    {
        player_
            .onMediaStatus(
                [this](MediaStatus, MediaStatus current) {
                    if (current == MediaStatus::Invalid) {
                        fail("QtAVCore reported invalid VVC media");
                    } else if (current == MediaStatus::EndOfMedia
                               && phase_.load(std::memory_order_acquire)
                                   == Phase::FullPlayback) {
                        fullEos_.fetch_add(1, std::memory_order_relaxed);
                        if (fullStreamFrames_.load(
                                std::memory_order_relaxed)
                            != RequiredFullStreamFrames) {
                            fail(
                                "VVC EOS arrived without exactly 600 hardware frames");
                        } else {
                            queue(Action::StartLifecycle);
                        }
                    }
                    return false;
                })
            .onEvent([this](const MediaEvent& event) {
                if (event.category == "decoder.hardware.fallback") {
                    if (phase_.load(std::memory_order_acquire)
                        == Phase::SoftwareFallback) {
                        fallbackEvents_.fetch_add(
                            1,
                            std::memory_order_relaxed);
                    } else {
                        unexpectedFallbackEvents_.fetch_add(
                            1,
                            std::memory_order_relaxed);
                    }
                }
                logMessage(
                    event.error == 0 ? LOG_INFO : LOG_WARN,
                    event.category + ": " + event.detail);
                return false;
            })
            .onVideoFrame([this](const VideoFrame&, int) {
                decodedFrames_.fetch_add(1, std::memory_order_relaxed);
            });
        player_.setLoop(0);
        worker_ = std::thread([this] { controlLoop(); });
    }

    ~Impl()
    {
        stop();
        {
            std::lock_guard<std::mutex> lock(controlMutex_);
            quit_ = true;
        }
        controlCondition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        player_
            .onMediaStatus({})
            .onEvent({})
            .onVideoFrame({});
    }

    bool start(
        const std::uint8_t* media,
        std::size_t size,
        OHNativeWindow* window)
    {
        if (!media || size <= 1 || !writeMediaFile(media, size)) {
            fail("Could not stage the supplied VVC sample in app storage");
            return false;
        }

        OH_AVCapability* capability =
            OH_AVCodec_GetCapabilityByCategory(
                OH_AVCODEC_MIMETYPE_VIDEO_VVC,
                false,
                HARDWARE);
        const char* name = capability
            ? OH_AVCapability_GetName(capability)
            : nullptr;
        if (!name || !*name) {
            fail("The connected OHOS device exposes no hardware VVC decoder");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            decoderName_ = name;
            detail_ = "Hardware VVC capability: " + decoderName_;
        }
        active_.store(true, std::memory_order_release);
        mediaReady_.store(true, std::memory_order_release);
        logMessage(
            LOG_INFO,
            "QTAV_OHOS_VVC_CAPABILITY hardware=1 decoder="
                + std::string(name));
        if (window) {
            setSurface(window);
        }
        return phase_.load(std::memory_order_acquire) != Phase::Failed;
    }

    void setSurface(OHNativeWindow* window)
    {
        if (!window || !active_.load(std::memory_order_acquire)) {
            return;
        }
        OHCodecSurface replacement(window);
        if (!replacement) {
            fail("VVC validation could not retain the XComponent surface");
            return;
        }

        const Phase phase = phase_.load(std::memory_order_acquire);
        if (phase == Phase::Inactive) {
            {
                std::lock_guard<std::mutex> lock(surfaceMutex_);
                surface_ = replacement;
                originalSurfaceGeneration_ = replacement.generation();
            }
            startFullPlayback(replacement);
            return;
        }
        if (phase != Phase::WaitingForSurface) {
            return;
        }

        VideoFrame stale;
        std::uint32_t oldGeneration = 0;
        {
            std::lock_guard<std::mutex> lock(surfaceMutex_);
            stale = staleFrame_;
            oldGeneration = originalSurfaceGeneration_;
        }
        if (oldGeneration == 0
            || replacement.generation() == oldGeneration
            || !stale
            || ohCodecFrame(stale, replacement)) {
            fail("VVC surface recreation did not reject the stale generation");
            return;
        }
        staleRejections_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(surfaceMutex_);
            surface_ = replacement;
            recreatedSurfaceGeneration_ = replacement.generation();
        }
        surfaceRecreations_.fetch_add(1, std::memory_order_relaxed);
        stageFrames_.store(0, std::memory_order_relaxed);
        phase_.store(
            Phase::AfterSurfaceRecreation,
            std::memory_order_release);
        configureHardwarePlayback(replacement);
        mediaOpens_.fetch_add(1, std::memory_order_relaxed);
        player_.setMedia(VVCMediaPath);
        player_.setState(State::Playing);
        logMessage(
            LOG_INFO,
            "QTAV_OHOS_CHECKPOINT VVC_SURFACE_RECREATED oldGeneration="
                + std::to_string(oldGeneration)
                + " newGeneration="
                + std::to_string(replacement.generation()));
    }

    void clearSurface()
    {
        if (!active_.load(std::memory_order_acquire)) {
            return;
        }
        const Phase phase = phase_.load(std::memory_order_acquire);
        if (phase != Phase::SurfaceRemovalPending
            && phase != Phase::WaitingForSurface) {
            return;
        }
        player_.setState(State::Stopped);
        if (!player_.waitFor(State::Stopped, 4'000)) {
            fail("VVC playback did not stop before surface removal");
            return;
        }
        player_
            .setVideoFrameScheduler({})
            .setHardwareDecodeConfig({});
        {
            std::lock_guard<std::mutex> lock(surfaceMutex_);
            surface_ = {};
        }
        phase_.store(Phase::WaitingForSurface, std::memory_order_release);
        logMessage(
            LOG_INFO,
            "QTAV_OHOS_CHECKPOINT OHCODEC_SURFACE_REMOVED codec=vvc");
    }

    void setForeground(bool foreground)
    {
        if (!active_.load(std::memory_order_acquire)) {
            return;
        }
        logMessage(
            LOG_INFO,
            foreground
                ? "QTAV_OHOS_LIFECYCLE FOREGROUND_OBSERVED codec=vvc"
                : "QTAV_OHOS_LIFECYCLE BACKGROUND_OBSERVED codec=vvc");
    }

    void stop()
    {
        if (!active_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        player_.setState(State::Stopped);
        player_.waitFor(State::Stopped, 4'000);
        player_
            .setVideoFrameScheduler({})
            .setHardwareDecodeConfig({});
        std::lock_guard<std::mutex> lock(surfaceMutex_);
        surface_ = {};
        staleFrame_ = {};
    }

    bool active() const noexcept
    {
        return active_.load(std::memory_order_acquire);
    }

    std::string status() const
    {
        std::string detail;
        std::string decoder;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            detail = detail_;
            decoder = decoderName_;
        }
        std::ostringstream result;
        result << "QtAVCore OHOS VVC validation: "
               << (phase_.load(std::memory_order_acquire) == Phase::Complete
                       ? "PASS"
                   : phase_.load(std::memory_order_acquire) == Phase::Failed
                       ? "FAIL"
                       : "RUNNING")
               << "\nphase="
               << phaseName(phase_.load(std::memory_order_acquire))
               << " decoder=" << decoder
               << " fullFrames="
               << fullStreamFrames_.load(std::memory_order_relaxed)
               << " lifecycleFrames="
               << lifecycleFrames_.load(std::memory_order_relaxed)
               << " softwareFrames="
               << softwareFrames_.load(std::memory_order_relaxed)
               << " maxPending="
               << maximumPendingFrames_.load(std::memory_order_relaxed);
        if (!detail.empty()) {
            result << '\n' << detail;
        }
        return result.str();
    }

private:
    void startFullPlayback(const OHCodecSurface& surface)
    {
        fullStreamFrames_.store(0, std::memory_order_relaxed);
        decodedFrames_.store(0, std::memory_order_relaxed);
        phase_.store(Phase::FullPlayback, std::memory_order_release);
        configureHardwarePlayback(surface);
        mediaOpens_.store(1, std::memory_order_relaxed);
        player_.setMedia(VVCMediaPath);
        player_.setState(State::Playing);
        logMessage(
            LOG_INFO,
            "QTAV_OHOS_CHECKPOINT VVC_FULL_STREAM_STARTED wrapper=vvc_ohcodec generation="
                + std::to_string(surface.generation()));
    }

    void configureHardwarePlayback(const OHCodecSurface& surface)
    {
        OHCodecHardwareDecodeOptions options;
        options.allowSoftwareFallback = false;
        options.extraHardwareFrames = 4;
        const HardwareDecodeConfig config =
            ohCodecHardwareDecodeConfig(surface, options);
        if (!config.device
            || config.deviceType != HardwareDeviceType::OHCodec
            || config.decoderWrapper != "ohcodec") {
            fail("The VVC OHCodec hardware configuration is invalid");
            return;
        }
        player_
            .setVideoFrameScheduler(
                [this, surface](
                    const VideoFrame& frame,
                    int,
                    std::int64_t deadline) {
                    return consumeHardwareFrame(
                        frame,
                        deadline,
                        surface);
                })
            .setHardwareDecodeConfig(config);
    }

    bool consumeHardwareFrame(
        const VideoFrame& frame,
        std::int64_t deadline,
        const OHCodecSurface& surface)
    {
        const std::uint64_t pending =
            pendingFrames_.fetch_add(1, std::memory_order_relaxed) + 1;
        updateMaximum(maximumPendingFrames_, pending);
        auto output = ohCodecFrame(frame, surface);
        if (!output) {
            finishPendingFrame();
            fail("VVC hardware playback received a non-OHCodec or stale frame");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(surfaceMutex_);
            staleFrame_ = frame;
        }
        if (!output.presentAt(deadline)) {
            finishPendingFrame();
            fail("VVC timed direct-surface presentation failed");
            return false;
        }
        finishPendingFrame();
        hardwareFrames_.fetch_add(1, std::memory_order_relaxed);

        const Phase phase = phase_.load(std::memory_order_acquire);
        if (phase == Phase::FullPlayback) {
            fullStreamFrames_.fetch_add(1, std::memory_order_relaxed);
        } else if (phase == Phase::LifecycleWarmup
                   || phase == Phase::AfterSeek
                   || phase == Phase::AfterSurfaceRecreation) {
            lifecycleFrames_.fetch_add(1, std::memory_order_relaxed);
            const std::uint64_t stage =
                stageFrames_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (stage >= RequiredLifecycleFrames) {
                advanceHardwareLifecycle(phase);
            }
        }
        return true;
    }

    void advanceHardwareLifecycle(Phase phase)
    {
        Phase next = phase;
        Action action = Action::Abort;
        switch (phase) {
        case Phase::LifecycleWarmup:
            next = Phase::PauseSeekPending;
            action = Action::PauseResumeSeek;
            break;
        case Phase::AfterSeek:
            next = Phase::SurfaceRemovalPending;
            action = Action::RequestSurfaceRecreation;
            break;
        case Phase::AfterSurfaceRecreation:
            next = Phase::StopPending;
            action = Action::StopHardware;
            break;
        default:
            return;
        }
        auto expected = phase;
        if (phase_.compare_exchange_strong(
                expected,
                next,
                std::memory_order_acq_rel)) {
            queue(action);
        }
    }

    bool consumeSoftwareFrame(const VideoFrame& frame)
    {
        if (frame.hasHardwareFrame()) {
            fail("The forced VVC software fallback received a stale hardware frame");
            return false;
        }
        const std::uint64_t count =
            softwareFrames_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count >= RequiredSoftwareFallbackFrames) {
            auto expected = Phase::SoftwareFallback;
            if (phase_.compare_exchange_strong(
                    expected,
                    Phase::FinishPending,
                    std::memory_order_acq_rel)) {
                queue(Action::Finish);
            }
        }
        return true;
    }

    void queue(Action action)
    {
        {
            std::lock_guard<std::mutex> lock(controlMutex_);
            actions_.push_back(action);
        }
        controlCondition_.notify_one();
    }

    void controlLoop()
    {
        for (;;) {
            Action action = Action::Abort;
            {
                std::unique_lock<std::mutex> lock(controlMutex_);
                controlCondition_.wait(lock, [this] {
                    return quit_ || !actions_.empty();
                });
                if (quit_) {
                    return;
                }
                action = actions_.front();
                actions_.pop_front();
            }
            switch (action) {
            case Action::StartLifecycle:
                startLifecycle();
                break;
            case Action::PauseResumeSeek:
                pauseResumeSeek();
                break;
            case Action::RequestSurfaceRecreation:
                requestSurfaceRecreation();
                break;
            case Action::StopHardware:
                stopHardwareAndStartFallback();
                break;
            case Action::Finish:
                finish();
                break;
            case Action::Abort:
                player_.setState(State::Stopped);
                player_.waitFor(State::Stopped, 4'000);
                break;
            }
        }
    }

    void startLifecycle()
    {
        OHCodecSurface surface;
        {
            std::lock_guard<std::mutex> lock(surfaceMutex_);
            surface = surface_;
        }
        if (!surface || fullEos_.load(std::memory_order_relaxed) != 1) {
            fail("VVC lifecycle could not start after the complete EOS run");
            return;
        }
        stageFrames_.store(0, std::memory_order_relaxed);
        phase_.store(Phase::LifecycleWarmup, std::memory_order_release);
        configureHardwarePlayback(surface);
        mediaOpens_.fetch_add(1, std::memory_order_relaxed);
        player_.setMedia(VVCMediaPath);
        player_.setState(State::Playing);
    }

    void pauseResumeSeek()
    {
        player_.setState(State::Paused);
        if (!player_.waitFor(State::Paused, 3'000)) {
            fail("VVC pause did not complete");
            return;
        }
        player_.setState(State::Playing);
        if (!player_.waitFor(State::Playing, 3'000)) {
            fail("VVC resume did not complete");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        player_.setState(State::Paused);
        if (!player_.waitFor(State::Paused, 3'000)) {
            fail("VVC did not pause before seek");
            return;
        }
        pauseResume_.fetch_add(1, std::memory_order_relaxed);

        auto completed = std::make_shared<std::atomic<bool>>(false);
        auto position = std::make_shared<std::atomic<std::int64_t>>(-1);
        if (!player_.seek(
                SeekTargetMilliseconds,
                SeekFlag::FromStart,
                [completed, position](std::int64_t value) {
                    position->store(value, std::memory_order_release);
                    completed->store(true, std::memory_order_release);
                })) {
            fail("VVC seek request was rejected");
            return;
        }
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(5);
        while (!completed->load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!completed->load(std::memory_order_acquire)
            || position->load(std::memory_order_acquire) < 0) {
            fail("VVC seek/flush did not complete");
            return;
        }
        seeks_.fetch_add(1, std::memory_order_relaxed);
        flushes_.fetch_add(1, std::memory_order_relaxed);
        stageFrames_.store(0, std::memory_order_relaxed);
        phase_.store(Phase::AfterSeek, std::memory_order_release);
        player_.setState(State::Playing);
        logMessage(
            LOG_INFO,
            "QTAV_OHOS_CHECKPOINT VVC_SEEK_FLUSH targetMs="
                + std::to_string(SeekTargetMilliseconds)
                + " callbackMs="
                + std::to_string(position->load(std::memory_order_acquire)));
    }

    void requestSurfaceRecreation()
    {
        player_.setState(State::Paused);
        if (!player_.waitFor(State::Paused, 3'000)) {
            fail("VVC did not pause before surface recreation");
            return;
        }
        logMessage(
            LOG_INFO,
            "QTAV_OHOS_LIFECYCLE BACKGROUND_REQUEST codec=vvc");
        setDetail("Waiting for a real VVC XComponent surface recreation");
    }

    void stopHardwareAndStartFallback()
    {
        player_.setState(State::Stopped);
        if (!player_.waitFor(State::Stopped, 4'000)) {
            fail("VVC explicit hardware stop did not complete");
            return;
        }
        explicitStops_.fetch_add(1, std::memory_order_relaxed);
        player_.setVideoFrameScheduler({});

        HardwareDecodeConfig forcedUnavailable;
        forcedUnavailable.deviceType = HardwareDeviceType::OHCodec;
        forcedUnavailable.allowSoftwareFallback = true;
        forcedUnavailable.extraHardwareFrames = 4;
        forcedUnavailable.requireSuppliedDevice = true;
        forcedUnavailable.decoderWrapper = "ohcodec";
        softwareFrames_.store(0, std::memory_order_relaxed);
        phase_.store(Phase::SoftwareFallback, std::memory_order_release);
        player_
            .setVideoFrameScheduler(
                [this](
                    const VideoFrame& frame,
                    int,
                    std::int64_t) {
                    return consumeSoftwareFrame(frame);
                })
            .setHardwareDecodeConfig(forcedUnavailable);
        mediaOpens_.fetch_add(1, std::memory_order_relaxed);
        player_.setMedia(VVCMediaPath);
        player_.setState(State::Playing);
        logMessage(
            LOG_INFO,
            "QTAV_OHOS_CHECKPOINT VVC_FORCED_UNAVAILABLE_FALLBACK_STARTED");
    }

    void finish()
    {
        player_.setState(State::Stopped);
        if (!player_.waitFor(State::Stopped, 4'000)) {
            fail("VVC software fallback stop did not complete");
            return;
        }
        const PlaybackStatistics playback = player_.playbackStatistics();
        std::string decoder;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            decoder = decoderName_;
        }
        const bool passed =
            fullStreamFrames_.load(std::memory_order_relaxed)
                    == RequiredFullStreamFrames
            && fullEos_.load(std::memory_order_relaxed) == 1
            && hardwareFrames_.load(std::memory_order_relaxed)
                    >= RequiredFullStreamFrames
                        + RequiredLifecycleFrames * 3
            && pauseResume_.load(std::memory_order_relaxed) == 1
            && seeks_.load(std::memory_order_relaxed) == 1
            && flushes_.load(std::memory_order_relaxed) == 1
            && explicitStops_.load(std::memory_order_relaxed) == 1
            && surfaceRecreations_.load(std::memory_order_relaxed) == 1
            && staleRejections_.load(std::memory_order_relaxed) == 1
            && softwareFrames_.load(std::memory_order_relaxed)
                    >= RequiredSoftwareFallbackFrames
            && fallbackEvents_.load(std::memory_order_relaxed) == 1
            && unexpectedFallbackEvents_.load(std::memory_order_relaxed) == 0
            && maximumPendingFrames_.load(std::memory_order_relaxed) == 1
            && pendingFrames_.load(std::memory_order_relaxed) == 0
            && playback.maximumQueuedVideoFrames == 0
            && playback.videoQueueOverflowDrops == 0
            && mediaOpens_.load(std::memory_order_relaxed) == 4
            && !decoder.empty();
        if (!passed) {
            fail("VVC hardware/software lifecycle counters did not satisfy the acceptance matrix");
            return;
        }

        phase_.store(Phase::Complete, std::memory_order_release);
        passed_.store(true, std::memory_order_release);
        const std::string result =
            "QTAV_OHOS_VVC_RESULT PASS frames=600 decoder=" + decoder
            + " wrapper=vvc_ohcodec hardwareFrames="
            + std::to_string(hardwareFrames_.load(std::memory_order_relaxed))
            + " eos=1 pauseResume=1 seek=1 flush=1 stop=1"
              " surfaceRecreation=1 staleRejected=1 maxPending=1"
              " softwareFallback="
            + std::to_string(softwareFrames_.load(std::memory_order_relaxed))
            + " fallbackEvents=1 hardwareFallback=0 maxQueued=0"
              " cpuMap=0 transfer=0 staging=0 upload=0";
        logMessage(LOG_INFO, result);
        logMessage(
            LOG_INFO,
            "QTAV_OHOS_RESULT PASS vvc frames=600 decoder=" + decoder
                + " wrapper=vvc_ohcodec fallback=software");
        setDetail(result);
    }

    void fail(std::string detail)
    {
        Phase previous = phase_.exchange(
            Phase::Failed,
            std::memory_order_acq_rel);
        passed_.store(false, std::memory_order_release);
        setDetail(detail);
        if (previous != Phase::Failed) {
            logMessage(LOG_ERROR, "QTAV_OHOS_RESULT FAIL " + detail);
            queue(Action::Abort);
        }
    }

    void setDetail(std::string detail)
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        detail_ = std::move(detail);
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

    void finishPendingFrame() noexcept
    {
        const auto previous = pendingFrames_.fetch_sub(
            1,
            std::memory_order_relaxed);
        if (previous == 0) {
            pendingFrames_.store(0, std::memory_order_relaxed);
        }
    }

    mutable std::mutex stateMutex_;
    std::mutex surfaceMutex_;
    std::mutex controlMutex_;
    std::condition_variable controlCondition_;
    std::deque<Action> actions_;
    std::thread worker_;
    Player player_;
    OHCodecSurface surface_;
    VideoFrame staleFrame_;
    std::string decoderName_;
    std::string detail_ = "Waiting for VVC media and XComponent surface";
    std::uint32_t originalSurfaceGeneration_ = 0;
    std::uint32_t recreatedSurfaceGeneration_ = 0;
    std::atomic<Phase> phase_ { Phase::Inactive };
    std::atomic<std::uint64_t> fullStreamFrames_ { 0 };
    std::atomic<std::uint64_t> lifecycleFrames_ { 0 };
    std::atomic<std::uint64_t> stageFrames_ { 0 };
    std::atomic<std::uint64_t> softwareFrames_ { 0 };
    std::atomic<std::uint64_t> hardwareFrames_ { 0 };
    std::atomic<std::uint64_t> decodedFrames_ { 0 };
    std::atomic<std::uint64_t> pendingFrames_ { 0 };
    std::atomic<std::uint64_t> maximumPendingFrames_ { 0 };
    std::atomic<std::uint64_t> fullEos_ { 0 };
    std::atomic<std::uint64_t> pauseResume_ { 0 };
    std::atomic<std::uint64_t> seeks_ { 0 };
    std::atomic<std::uint64_t> flushes_ { 0 };
    std::atomic<std::uint64_t> explicitStops_ { 0 };
    std::atomic<std::uint64_t> surfaceRecreations_ { 0 };
    std::atomic<std::uint64_t> staleRejections_ { 0 };
    std::atomic<std::uint64_t> fallbackEvents_ { 0 };
    std::atomic<std::uint64_t> unexpectedFallbackEvents_ { 0 };
    std::atomic<std::uint64_t> mediaOpens_ { 0 };
    std::atomic<bool> mediaReady_ { false };
    std::atomic<bool> active_ { false };
    std::atomic<bool> passed_ { false };
    bool quit_ = false;
};

VVCValidationSession::VVCValidationSession()
    : impl_(std::make_unique<Impl>())
{
}

VVCValidationSession::~VVCValidationSession() = default;

bool VVCValidationSession::start(
    const std::uint8_t* media,
    std::size_t size,
    OHNativeWindow* window)
{
    return impl_->start(media, size, window);
}

void VVCValidationSession::setSurface(OHNativeWindow* window)
{
    impl_->setSurface(window);
}

void VVCValidationSession::clearSurface()
{
    impl_->clearSurface();
}

void VVCValidationSession::setForeground(bool foreground)
{
    impl_->setForeground(foreground);
}

void VVCValidationSession::stop()
{
    impl_->stop();
}

bool VVCValidationSession::active() const noexcept
{
    return impl_->active();
}

std::string VVCValidationSession::status() const
{
    return impl_->status();
}

} // namespace qtav::ohos_example
