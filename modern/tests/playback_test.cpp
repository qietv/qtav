// SPDX-License-Identifier: LGPL-2.1-or-later

#if defined(NDEBUG)
#  undef NDEBUG
#endif

#include <qtav/player.h>
#include <qtav/video_render_api.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <memory>
#include <thread>
#include <utility>

namespace {

class CountingRenderer final : public qtav::VideoRenderAPI {
public:
    explicit CountingRenderer(
        std::atomic<int>& count,
        bool renderResult = true)
        : count_(count)
        , renderResult_(renderResult)
    {
    }

    qtav::VideoRenderCapabilities capabilities() const override
    {
        return {};
    }
    void setEventCallback(EventCallback callback) override
    {
        callback_ = std::move(callback);
    }
    bool open(const qtav::VideoRenderConfig&) override { return true; }
    bool configure(const qtav::VideoRenderConfig&) override { return true; }
    bool render(const qtav::VideoFrame& frame) override
    {
        assert(frame);
        ++count_;
        return renderResult_;
    }
    void close() noexcept override {}

private:
    std::atomic<int>& count_;
    bool renderResult_ = true;
    EventCallback callback_;
};

class BlockingRenderer final : public qtav::VideoRenderAPI {
public:
    qtav::VideoRenderCapabilities capabilities() const override
    {
        return {};
    }
    void setEventCallback(EventCallback callback) override
    {
        callback_ = std::move(callback);
    }
    bool open(const qtav::VideoRenderConfig&) override { return true; }
    bool configure(const qtav::VideoRenderConfig&) override { return true; }
    bool render(const qtav::VideoFrame& frame) override
    {
        assert(frame);
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        const bool released = changed_.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return released_; });
        assert(released);
        return true;
    }
    void close() noexcept override {}

    bool waitUntilEntered()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return entered_; });
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool released_ = false;
    EventCallback callback_;
};

class DetailedRenderer final : public qtav::VideoRenderAPI {
public:
    DetailedRenderer(
        qtav::VideoRenderAttemptStatus status,
        std::atomic<int>& count,
        std::uint32_t retryAfterMilliseconds = 0,
        qtav::VideoRenderRetryReason retryReason =
            qtav::VideoRenderRetryReason::Unspecified)
        : status_(status)
        , count_(count)
        , retryAfterMilliseconds_(retryAfterMilliseconds)
        , retryReason_(retryReason)
    {
    }

    qtav::VideoRenderCapabilities capabilities() const override { return {}; }
    void setEventCallback(EventCallback) override {}
    bool open(const qtav::VideoRenderConfig&) override { return true; }
    bool configure(const qtav::VideoRenderConfig&) override { return true; }
    qtav::VideoRenderAttemptResult renderDetailed(
        const qtav::VideoFrame& frame) override
    {
        assert(frame);
        ++count_;
        return {
            status_,
            retryAfterMilliseconds_,
            "scripted detailed renderer result",
            retryReason_,
        };
    }
    bool render(const qtav::VideoFrame& frame) override
    {
        return renderDetailed(frame).frameConsumed();
    }
    void close() noexcept override {}

private:
    qtav::VideoRenderAttemptStatus status_;
    std::atomic<int>& count_;
    std::uint32_t retryAfterMilliseconds_ = 0;
    qtav::VideoRenderRetryReason retryReason_ =
        qtav::VideoRenderRetryReason::Unspecified;
};

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);

    qtav::Player player;
    std::mutex mutex;
    std::condition_variable finished;
    bool reachedEnd = false;
    bool failed = false;
    std::atomic<int> videoFrames { 0 };
    std::atomic<int> audioFrames { 0 };
    std::atomic<int> renderedFrames { 0 };
    std::atomic<int> rejectedRenderAttempts { 0 };
    std::atomic<int> deferredRenderAttempts { 0 };
    std::atomic<int> discardedRenderAttempts { 0 };
    std::atomic<int> surfaceLostRenderAttempts { 0 };
    std::atomic<int> fatalRenderAttempts { 0 };
    std::atomic<int> structuredRetryAttempts { 0 };
    int firstRenderKey = 1;
    int secondRenderKey = 2;
    int rejectingRenderKey = 3;
    int deferredRenderKey = 4;
    int discardedRenderKey = 5;
    int surfaceLostRenderKey = 6;
    int fatalRenderKey = 7;
    int structuredRetryKey = 8;
    auto firstRenderer = std::make_shared<CountingRenderer>(renderedFrames);
    auto secondRenderer = std::make_shared<CountingRenderer>(renderedFrames);
    auto rejectingRenderer = std::make_shared<CountingRenderer>(
        rejectedRenderAttempts,
        false);
    auto deferredRenderer = std::make_shared<DetailedRenderer>(
        qtav::VideoRenderAttemptStatus::DeferredUntilRedraw,
        deferredRenderAttempts);
    auto discardedRenderer = std::make_shared<DetailedRenderer>(
        qtav::VideoRenderAttemptStatus::Discarded,
        discardedRenderAttempts);
    auto surfaceLostRenderer = std::make_shared<DetailedRenderer>(
        qtav::VideoRenderAttemptStatus::SurfaceLost,
        surfaceLostRenderAttempts);
    auto fatalRenderer = std::make_shared<DetailedRenderer>(
        qtav::VideoRenderAttemptStatus::FatalError,
        fatalRenderAttempts);
    auto structuredRetryRenderer = std::make_shared<DetailedRenderer>(
        qtav::VideoRenderAttemptStatus::RetryAfterBackoff,
        structuredRetryAttempts,
        3,
        qtav::VideoRenderRetryReason::
            DeviceContextBusyReservationAware);
    const auto emptyRender = player.renderVideoDetailed();
    assert(emptyRender.status == qtav::VideoRenderStatus::NoFrame);
    assert(emptyRender.frameSequence == 0);
    assert(player.renderVideo() < 0.0);

    player
        .onMediaStatus([&](qtav::MediaStatus, qtav::MediaStatus status) {
            if (status == qtav::MediaStatus::EndOfMedia
                || status == qtav::MediaStatus::Invalid) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    reachedEnd = status == qtav::MediaStatus::EndOfMedia;
                    failed = status == qtav::MediaStatus::Invalid;
                }
                finished.notify_all();
            }
            return false;
        })
        .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
            assert(frame);
            assert(frame.width() == 160);
            assert(frame.height() == 90);
            ++videoFrames;
        })
        .onAudioFrame([&](const qtav::AudioFrame& frame, int) {
            assert(frame);
            assert(frame.sampleRate() == 48'000);
            ++audioFrames;
        })
        .setVideoRenderer([&](const qtav::VideoFrame& frame, void*) {
            assert(frame);
            ++renderedFrames;
        })
        .setVideoRenderAPI(firstRenderer, &firstRenderKey)
        .setVideoRenderAPI(secondRenderer, &secondRenderKey)
        .setVideoRenderAPI(rejectingRenderer, &rejectingRenderKey)
        .setVideoRenderAPI(deferredRenderer, &deferredRenderKey)
        .setVideoRenderAPI(discardedRenderer, &discardedRenderKey)
        .setVideoRenderAPI(surfaceLostRenderer, &surfaceLostRenderKey)
        .setVideoRenderAPI(fatalRenderer, &fatalRenderKey)
        .setVideoRenderAPI(structuredRetryRenderer, &structuredRetryKey)
        .setRenderCallback([&](void* opaque) {
            const auto rejectedBefore = rejectedRenderAttempts.load();
            if (!opaque) {
                assert(player.renderVideo() >= 0.0);
                return;
            }

            const auto result = player.renderVideoDetailed(opaque);
            assert(result.frameSequence > 0);
            assert(result.presentationGeneration > 0);
            assert(result.status != qtav::VideoRenderStatus::PlayerStateBusy);
            if (opaque == &rejectingRenderKey) {
                assert(
                    result.status
                    == qtav::VideoRenderStatus::RendererBusy);
                assert(result.retryAfterMilliseconds == 1);
                assert(!result.detail.empty());
                assert(
                    rejectedRenderAttempts.load()
                    == rejectedBefore + 1);
            } else if (opaque == &deferredRenderKey) {
                assert(
                    result.status
                    == qtav::VideoRenderStatus::RendererDeferred);
            } else if (opaque == &discardedRenderKey) {
                assert(
                    result.status
                    == qtav::VideoRenderStatus::FrameDiscarded);
            } else if (opaque == &surfaceLostRenderKey) {
                assert(
                    result.status
                    == qtav::VideoRenderStatus::SurfaceLost);
            } else if (opaque == &fatalRenderKey) {
                assert(
                    result.status
                    == qtav::VideoRenderStatus::RendererError);
            } else if (opaque == &structuredRetryKey) {
                assert(
                    result.status
                    == qtav::VideoRenderStatus::RendererBusy);
                assert(result.retryAfterMilliseconds == 3);
                assert(
                    result.retryReason
                    == qtav::VideoRenderRetryReason::
                        DeviceContextBusyReservationAware);
            } else {
                assert(result.status == qtav::VideoRenderStatus::Rendered);
            }
        });

    player.setPlaybackRate(4.0F);
    player.setMedia(argv[1]);
    player.setState(qtav::State::Playing);

    std::unique_lock<std::mutex> lock(mutex);
    const bool completed = finished.wait_for(
        lock,
        std::chrono::seconds(10),
        [&] { return reachedEnd || failed; });
    lock.unlock();

    assert(completed);
    assert(!failed);
    assert(reachedEnd);
    assert(videoFrames.load() >= 10);
    assert(audioFrames.load() > 0);
    assert(renderedFrames.load() == videoFrames.load() * 3);
    assert(rejectedRenderAttempts.load() == videoFrames.load());
    assert(deferredRenderAttempts.load() == videoFrames.load());
    assert(discardedRenderAttempts.load() == videoFrames.load());
    assert(surfaceLostRenderAttempts.load() == videoFrames.load());
    assert(fatalRenderAttempts.load() == videoFrames.load());
    assert(player.state() == qtav::State::Stopped);

    qtav::Player invalidationPlayer;
    auto blockingRenderer = std::make_shared<BlockingRenderer>();
    std::mutex renderRequestMutex;
    std::condition_variable renderRequestChanged;
    bool renderRequested = false;
    bool renderFinished = false;
    qtav::VideoRenderResult invalidatedRender;
    invalidationPlayer
        .setVideoRenderAPI(blockingRenderer)
        .setRenderCallback([&](void*) {
            {
                std::lock_guard<std::mutex> requestLock(
                    renderRequestMutex);
                renderRequested = true;
            }
            renderRequestChanged.notify_all();
        });
    std::thread renderThread([&] {
        {
            std::unique_lock<std::mutex> requestLock(
                renderRequestMutex);
            const bool requested = renderRequestChanged.wait_for(
                requestLock,
                std::chrono::seconds(5),
                [&] { return renderRequested; });
            assert(requested);
        }
        invalidatedRender = invalidationPlayer.renderVideoDetailed();
        {
            std::lock_guard<std::mutex> requestLock(renderRequestMutex);
            renderFinished = true;
        }
        renderRequestChanged.notify_all();
    });
    invalidationPlayer.setMedia(argv[1]);
    invalidationPlayer.setState(qtav::State::Playing);
    assert(blockingRenderer->waitUntilEntered());
    assert(invalidationPlayer.seek(0));
    invalidationPlayer.setVideoRenderAPI({});
    blockingRenderer->release();
    {
        std::unique_lock<std::mutex> requestLock(renderRequestMutex);
        assert(renderRequestChanged.wait_for(
            requestLock,
            std::chrono::seconds(5),
            [&] { return renderFinished; }));
    }
    renderThread.join();
    assert(
        invalidatedRender.status
        == qtav::VideoRenderStatus::FrameDiscarded);
    assert(invalidatedRender.frameSequence > 0);
    assert(!invalidatedRender.detail.empty());
    invalidationPlayer.setState(qtav::State::Stopped);
    assert(invalidationPlayer.waitFor(qtav::State::Stopped, 5'000));

    qtav::Player scheduledPlayer;
    std::mutex scheduledMutex;
    std::condition_variable scheduledFinished;
    bool scheduledReachedEnd = false;
    bool scheduledFailed = false;
    std::atomic<int> scheduledFrames { 0 };
    std::atomic<int> fallbackFrames { 0 };
    scheduledPlayer
        .onMediaStatus(
            [&](qtav::MediaStatus, qtav::MediaStatus status) {
                if (status == qtav::MediaStatus::EndOfMedia
                    || status == qtav::MediaStatus::Invalid) {
                    {
                        std::lock_guard<std::mutex> scheduledLock(
                            scheduledMutex);
                        scheduledReachedEnd =
                            status == qtav::MediaStatus::EndOfMedia;
                        scheduledFailed =
                            status == qtav::MediaStatus::Invalid;
                    }
                    scheduledFinished.notify_all();
                }
                return false;
            })
        .setVideoFrameScheduler(
            [&](const qtav::VideoFrame& frame,
                int,
                std::int64_t monotonicNanoseconds) {
                assert(frame);
                assert(frame.width() == 160);
                assert(frame.height() == 90);
                assert(monotonicNanoseconds > 0);
                ++scheduledFrames;
                return true;
            })
        .onVideoFrame(
            [&](const qtav::VideoFrame&, int) {
                ++fallbackFrames;
            });
    scheduledPlayer.setPlaybackRate(4.0F);
    scheduledPlayer.setMedia(argv[1]);
    scheduledPlayer.setState(qtav::State::Playing);

    std::unique_lock<std::mutex> scheduledLock(scheduledMutex);
    const bool scheduledCompleted = scheduledFinished.wait_for(
        scheduledLock,
        std::chrono::seconds(10),
        [&] { return scheduledReachedEnd || scheduledFailed; });
    scheduledLock.unlock();

    const qtav::PlaybackStatistics statistics =
        scheduledPlayer.playbackStatistics();
    assert(scheduledCompleted);
    assert(!scheduledFailed);
    assert(scheduledReachedEnd);
    assert(scheduledFrames.load() >= 10);
    assert(fallbackFrames.load() == 0);
    assert(statistics.decodedVideoFrames
        >= statistics.deliveredVideoFrames);
    assert(statistics.deliveredVideoFrames
        == static_cast<std::uint64_t>(scheduledFrames.load()));
    assert(statistics.videoQueueOverflowDrops == 0);
    assert(statistics.lateVideoDrops == 0);
    assert(statistics.maximumQueuedVideoFrames == 0);
    assert(scheduledPlayer.state() == qtav::State::Stopped);
    return 0;
}
