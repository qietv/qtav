// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/audio_sink.h>
#include <qtav/player.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace {

class DeterministicAudioSink final : public qtav::AudioSink {
public:
    explicit DeterministicAudioSink(bool requiresConversion = false)
        : requiresConversion_(requiresConversion)
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
        clockPosition_.store(
            std::max<std::int64_t>(
                clockPosition_.load(),
                buffer.timestamp + buffer.duration));
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
        const auto step = paused_.load() ? 0 : 50;
        return {
            open_.load(),
            clockPosition_.fetch_add(step) + step,
            10,
        };
    }

    void reportUnderrunOnNextWrite()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reportUnderrun_ = true;
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
    std::atomic<bool> open_ { false };
    std::atomic<bool> paused_ { false };
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
    assert(audioFrames.load() >= 1);
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
        .onAudioFrame([&](const qtav::AudioFrame&, int) { ++audioFrames; })
        .onEvent([&](const qtav::MediaEvent& event) {
            if (event.category != "audio.sink.format") {
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                formatEvent = true;
            }
            changed.notify_all();
            player.setState(qtav::State::Stopped);
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
            [&] { return formatEvent; }));
    }
    assert(player.waitFor(qtav::State::Stopped, 5'000));
    assert(audioFrames.load() >= 1);
    assert(sink->openCount() == 1);
    assert(sink->closeCount() == 1);
    assert(sink->writeCount() == 0);
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    testLifecycleAndClock(argv[1]);
    testShutdownClosesSink(argv[1]);
    testUnsupportedConversionKeepsFrameCallback(argv[1]);
    return 0;
}
