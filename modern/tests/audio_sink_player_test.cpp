// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/audio_sink.h>
#include <qtav/player.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace {

void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

class DeterministicAudioSink final : public qtav::AudioSink {
public:
    explicit DeterministicAudioSink(
        bool requiresConversion = false,
        bool blockFirstWrite = false,
        bool simulateQueuedClock = false)
        : requiresConversion_(requiresConversion)
        , blockFirstWrite_(blockFirstWrite)
        , simulateQueuedClock_(simulateQueuedClock)
    {
    }

    qtav::AudioSinkCapabilities capabilities() const override
    {
        return {
            {},
            1,
            384'000,
            64,
            true,
            true,
        };
    }

    void setEventCallback(EventCallback callback) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(callback);
    }

    qtav::AudioSinkOpenResult open(
        const qtav::AudioFormat& decodedFormat) override
    {
        assert(decodedFormat.isValid());
        open_.store(true);
        paused_.store(false);
        clockPosition_.store(0);
        ++openCount_;
        notifyChanged();
        auto deviceFormat = decodedFormat;
        if (requiresConversion_) {
            ++deviceFormat.sampleRate;
        }
        return { true, std::move(deviceFormat), {} };
    }

    void close() noexcept override
    {
        open_.store(false);
        ++closeCount_;
        notifyChanged();
    }

    void pause(bool paused) override
    {
        paused_.store(paused);
        paused ? ++pauseCount_ : ++resumeCount_;
        notifyChanged();
    }

    void flush() override
    {
        ++flushCount_;
        notifyChanged();
    }

    bool write(const qtav::AudioBufferView& buffer) override
    {
        assert(buffer.isValid());
        if (!open_.load() || paused_.load()) {
            return false;
        }
        if (blockFirstWrite_ && !firstWriteSeen_.exchange(true)) {
            std::unique_lock<std::mutex> lock(firstWriteMutex_);
            firstWriteEntered_ = true;
            firstWriteChanged_.notify_all();
            firstWriteChanged_.wait(lock, [&] { return releaseFirstWrite_; });
        }
        {
            std::unique_lock<std::mutex> lock(blockedWriteMutex_);
            if (blockNextWrite_) {
                blockNextWrite_ = false;
                blockedWriteEntered_ = true;
                blockedWriteChanged_.notify_all();
                blockedWriteChanged_.wait(
                    lock,
                    [&] { return releaseBlockedWrite_; });
            }
        }
        if (!simulateQueuedClock_) {
            clockPosition_.store(
                std::max<std::int64_t>(
                    clockPosition_.load(),
                    buffer.timestamp + buffer.duration));
        }
        ++writeCount_;
        notifyChanged();

        EventCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (reportUnderrun_) {
                reportUnderrun_ = false;
                callback = callback_;
            }
        }
        if (callback) {
            callback({
                qtav::AudioSinkEventType::Underrun,
                "deterministic test underrun",
            });
        }
        return true;
    }

    qtav::AudioSinkClock clock() const noexcept override
    {
        ++clockCount_;
        const auto step = paused_.load()
            ? 0
            : simulateQueuedClock_ ? 1 : 50;
        return {
            open_.load() && clockValid_.load(),
            clockPosition_.fetch_add(step) + step,
            10,
        };
    }

    void setClockValid(bool valid)
    {
        clockValid_.store(valid);
        notifyChanged();
    }

    void reportUnderrunOnNextWrite()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reportUnderrun_ = true;
    }

    void blockNextWrite()
    {
        std::lock_guard<std::mutex> lock(blockedWriteMutex_);
        blockNextWrite_ = true;
        blockedWriteEntered_ = false;
        releaseBlockedWrite_ = false;
    }

    bool waitForBlockedWrite(
        std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        std::unique_lock<std::mutex> lock(blockedWriteMutex_);
        return blockedWriteChanged_.wait_for(
            lock,
            timeout,
            [&] { return blockedWriteEntered_; });
    }

    void releaseBlockedWrite()
    {
        {
            std::lock_guard<std::mutex> lock(blockedWriteMutex_);
            releaseBlockedWrite_ = true;
        }
        blockedWriteChanged_.notify_all();
    }

    bool waitFor(
        const std::function<bool()>& predicate,
        std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        std::unique_lock<std::mutex> lock(waitMutex_);
        return changed_.wait_for(lock, timeout, predicate);
    }

    int openCount() const noexcept { return openCount_.load(); }
    int closeCount() const noexcept { return closeCount_.load(); }
    int pauseCount() const noexcept { return pauseCount_.load(); }
    int resumeCount() const noexcept { return resumeCount_.load(); }
    int flushCount() const noexcept { return flushCount_.load(); }
    int writeCount() const noexcept { return writeCount_.load(); }
    int clockCount() const noexcept { return clockCount_.load(); }

    bool waitForFirstWrite(
        std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        std::unique_lock<std::mutex> lock(firstWriteMutex_);
        return firstWriteChanged_.wait_for(
            lock,
            timeout,
            [&] { return firstWriteEntered_; });
    }

    void releaseFirstWrite()
    {
        {
            std::lock_guard<std::mutex> lock(firstWriteMutex_);
            releaseFirstWrite_ = true;
        }
        firstWriteChanged_.notify_all();
    }

private:
    void notifyChanged()
    {
        {
            std::lock_guard<std::mutex> lock(waitMutex_);
            ++revision_;
        }
        changed_.notify_all();
    }

    mutable std::mutex mutex_;
    std::mutex waitMutex_;
    std::condition_variable changed_;
    std::uint64_t revision_ = 0;
    EventCallback callback_;
    bool reportUnderrun_ = false;
    bool requiresConversion_ = false;
    bool blockFirstWrite_ = false;
    bool simulateQueuedClock_ = false;
    std::atomic<bool> firstWriteSeen_ { false };
    std::mutex firstWriteMutex_;
    std::condition_variable firstWriteChanged_;
    bool firstWriteEntered_ = false;
    bool releaseFirstWrite_ = false;
    std::mutex blockedWriteMutex_;
    std::condition_variable blockedWriteChanged_;
    bool blockNextWrite_ = false;
    bool blockedWriteEntered_ = false;
    bool releaseBlockedWrite_ = false;
    std::atomic<bool> open_ { false };
    std::atomic<bool> paused_ { false };
    std::atomic<bool> clockValid_ { true };
    mutable std::atomic<std::int64_t> clockPosition_ { 0 };
    std::atomic<int> openCount_ { 0 };
    std::atomic<int> closeCount_ { 0 };
    std::atomic<int> pauseCount_ { 0 };
    std::atomic<int> resumeCount_ { 0 };
    std::atomic<int> flushCount_ { 0 };
    std::atomic<int> writeCount_ { 0 };
    mutable std::atomic<int> clockCount_ { 0 };
};

void testLifecycleAndClock(const char* media)
{
    auto sink = std::make_shared<DeterministicAudioSink>();
    qtav::Player player;
    std::atomic<int> audioFrames { 0 };
    std::atomic<int> eventPhase { 0 };
    std::atomic<int> underruns { 0 };

    player
        .onAudioFrame([&](const qtav::AudioFrame& frame, int) {
            assert(frame);
            ++audioFrames;
        })
        .onEvent([&](const qtav::MediaEvent& event) {
            if (event.category != "audio.sink.underrun") {
                return false;
            }
            ++underruns;
            if (eventPhase.load() == 1) {
                player.setState(qtav::State::Paused);
            } else if (eventPhase.load() == 2) {
                eventPhase.store(3);
                sink->reportUnderrunOnNextWrite();
                player.setMedia(media);
            } else if (eventPhase.load() == 3) {
                player.setState(qtav::State::Paused);
            }
            return true;
        })
        .setAudioSink(sink);

    eventPhase.store(1);
    sink->reportUnderrunOnNextWrite();
    player.setMedia(media);
    player.setState(qtav::State::Playing);

    assert(player.waitFor(qtav::State::Paused, 5'000));
    assert(sink->waitFor([&] { return sink->pauseCount() >= 1; }));
    assert(sink->openCount() == 1);
    assert(sink->writeCount() >= 1);
    assert(underruns.load() == 1);
    assert(sink->clockCount() > 0);

    const int flushesBeforeSeek = sink->flushCount();
    std::mutex seekMutex;
    std::condition_variable seekChanged;
    bool seeked = false;
    assert(player.seek(
        400,
        qtav::SeekFlag::FromStart,
        [&](std::int64_t position) {
            assert(position == 400);
            {
                std::lock_guard<std::mutex> lock(seekMutex);
                seeked = true;
            }
            seekChanged.notify_all();
        }));
    {
        std::unique_lock<std::mutex> lock(seekMutex);
        assert(seekChanged.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return seeked; }));
    }
    assert(sink->flushCount() > flushesBeforeSeek);

    const int opensBeforeReplacement = sink->openCount();
    const int closesBeforeReplacement = sink->closeCount();
    const int flushesBeforeReplacement = sink->flushCount();
    eventPhase.store(2);
    sink->reportUnderrunOnNextWrite();
    player.setState(qtav::State::Playing);

    assert(sink->waitFor(
        [&] { return sink->openCount() > opensBeforeReplacement; }));
    assert(sink->closeCount() > closesBeforeReplacement);
    assert(sink->flushCount() > flushesBeforeReplacement);
    assert(sink->resumeCount() >= 2);
    assert(player.waitFor(qtav::State::Paused, 5'000));
    assert(underruns.load() >= 3);

    auto replacementSink = std::make_shared<DeterministicAudioSink>();
    const int closesBeforeSinkReplacement = sink->closeCount();
    player.setAudioSink(replacementSink);
    assert(sink->waitFor(
        [&] { return sink->closeCount() > closesBeforeSinkReplacement; }));
    player.setState(qtav::State::Playing);
    assert(replacementSink->waitFor(
        [&] { return replacementSink->writeCount() >= 1; }));
    assert(replacementSink->waitFor(
        [&] { return audioFrames.load() >= 1; }));
    player.setState(qtav::State::Stopped);
    assert(player.waitFor(qtav::State::Stopped, 5'000));
    assert(replacementSink->waitFor(
        [&] { return replacementSink->closeCount() >= 1; }));
}

void testShutdownClosesSink(const char* media)
{
    auto sink = std::make_shared<DeterministicAudioSink>();
    {
        qtav::Player player;
        player
            .onEvent([&](const qtav::MediaEvent& event) {
                if (event.category == "audio.sink.underrun") {
                    player.setState(qtav::State::Paused);
                    return true;
                }
                return false;
            })
            .setAudioSink(sink);
        sink->reportUnderrunOnNextWrite();
        player.setMedia(media);
        player.setState(qtav::State::Playing);
        assert(player.waitFor(qtav::State::Paused, 5'000));
        assert(sink->openCount() == 1);
        assert(sink->closeCount() == 0);
    }
    assert(sink->closeCount() == 1);
}

void testUnsupportedConversionKeepsFrameCallback(const char* media)
{
    auto sink = std::make_shared<DeterministicAudioSink>(true);
    qtav::Player player;
    std::mutex mutex;
    std::condition_variable changed;
    bool formatEvent = false;
    std::atomic<int> audioFrames { 0 };

    player
        .onAudioFrame([&](const qtav::AudioFrame&, int) {
            ++audioFrames;
            changed.notify_all();
        })
        .onEvent([&](const qtav::MediaEvent& event) {
            if (event.category != "audio.sink.format") {
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                formatEvent = true;
            }
            changed.notify_all();
            return true;
        })
        .setAudioSink(sink);
    player.setMedia(media);
    player.setState(qtav::State::Playing);

    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return formatEvent && audioFrames.load() >= 1; }));
    }
    player.setState(qtav::State::Stopped);
    assert(player.waitFor(qtav::State::Stopped, 5'000));
    assert(audioFrames.load() >= 1);
    assert(sink->openCount() == 1);
    assert(sink->closeCount() == 1);
    assert(sink->writeCount() == 0);
}

void testUiAndRenderIsolation(const char* media)
{
    auto sink = std::make_shared<DeterministicAudioSink>(false, true);
    qtav::Player player;

    std::mutex renderMutex;
    std::condition_variable renderChanged;
    bool renderEntered = false;
    bool releaseRender = false;

    player
        .setAudioSink(sink)
        .setRenderCallback([&](void*) {
            std::unique_lock<std::mutex> lock(renderMutex);
            renderEntered = true;
            renderChanged.notify_all();
            renderChanged.wait(lock, [&] { return releaseRender; });
        });

    player.setMedia(media);
    player.setState(qtav::State::Playing);

    assert(sink->waitForFirstWrite());
    {
        std::unique_lock<std::mutex> lock(renderMutex);
        assert(renderChanged.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return renderEntered; }));
    }

    std::thread writeWatchdog([sink] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        sink->releaseFirstWrite();
    });
    const auto queryStarted = std::chrono::steady_clock::now();
    static_cast<void>(player.position());
    const auto queryElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - queryStarted);
    sink->releaseFirstWrite();
    writeWatchdog.join();
    assert(queryElapsed < std::chrono::milliseconds(100));

    assert(sink->waitFor([&] { return sink->writeCount() >= 3; }));
    {
        std::lock_guard<std::mutex> lock(renderMutex);
        assert(renderEntered);
        assert(!releaseRender);
        releaseRender = true;
    }
    renderChanged.notify_all();

    player.setState(qtav::State::Stopped);
    assert(player.waitFor(qtav::State::Stopped, 5'000));
}

void testPlayingSeekWaitsForDeviceClock(const char* media)
{
    auto sink = std::make_shared<DeterministicAudioSink>();
    qtav::Player player;
    std::mutex mutex;
    std::condition_variable changed;
    bool buffering = false;
    bool loadedAfterBuffering = false;
    bool seeked = false;
    bool failed = false;

    player
        .onMediaStatus(
            [&](qtav::MediaStatus, qtav::MediaStatus status) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (status == qtav::MediaStatus::Buffering) {
                        buffering = true;
                    } else if (
                        status == qtav::MediaStatus::Loaded
                        && buffering) {
                        loadedAfterBuffering = true;
                    } else if (status == qtav::MediaStatus::Invalid) {
                        failed = true;
                    }
                }
                changed.notify_all();
                return false;
            })
        .setAudioSink(sink);
    player.setMedia(media);
    player.setState(qtav::State::Playing);

    assert(sink->waitFor([&] { return sink->writeCount() >= 1; }));
    sink->setClockValid(false);
    assert(player.seek(
        400,
        qtav::SeekFlag::FromStart,
        [&](std::int64_t position) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                seeked = position == 400;
                failed = failed || position < 0;
            }
            changed.notify_all();
        }));
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return (buffering && seeked) || failed; }));
    }
    assert(!failed);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto frozenPosition = player.position();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    assert(player.position() == frozenPosition);
    assert(player.mediaStatus() == qtav::MediaStatus::Buffering);

    sink->setClockValid(true);
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return loadedAfterBuffering || failed; }));
    }
    assert(!failed);
    assert(player.mediaStatus() == qtav::MediaStatus::Loaded);

    player.setState(qtav::State::Stopped);
    assert(player.waitFor(qtav::State::Stopped, 5'000));
}

void testPresentationDoesNotWaitForBlockedSinkWrite(const char* media)
{
    auto sink = std::make_shared<DeterministicAudioSink>(
        false,
        false,
        true);
    qtav::Player player;
    std::atomic<int> videoFrames { 0 };

    player
        .setAudioSink(sink)
        .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
            assert(frame);
            ++videoFrames;
        });
    player.setMedia(media);
    player.setState(qtav::State::Playing);

    require(sink->waitFor([&] {
        return sink->writeCount() >= 20 && videoFrames.load() >= 2;
    }));
    sink->blockNextWrite();
    require(sink->waitForBlockedWrite());

    const auto framesBefore = videoFrames.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    const auto framesWhileBlocked = videoFrames.load();
    sink->releaseBlockedWrite();

    require(framesWhileBlocked > framesBefore);
    player.setState(qtav::State::Stopped);
    require(player.waitFor(qtav::State::Stopped, 5'000));
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    testLifecycleAndClock(argv[1]);
    testShutdownClosesSink(argv[1]);
    testUnsupportedConversionKeepsFrameCallback(argv[1]);
    testUiAndRenderIsolation(argv[1]);
    testPlayingSeekWaitsForDeviceClock(argv[1]);
    testPresentationDoesNotWaitForBlockedSinkWrite(argv[1]);
    return 0;
}
