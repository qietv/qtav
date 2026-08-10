// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/audio_time_stretcher.h>
#include <qtav/player.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace {

#define require(condition)                                               \
    do {                                                                 \
        if (!(condition)) {                                              \
            std::fprintf(                                                \
                stderr,                                                  \
                "require failed at %s:%d: %s\n",                        \
                __FILE__,                                                \
                __LINE__,                                                \
                #condition);                                             \
            std::abort();                                                \
        }                                                                \
    } while (false)

class CountingAudioSink final : public qtav::AudioSink {
public:
    qtav::AudioSinkCapabilities capabilities() const override
    {
        return { {}, 1, 384'000, 64, true, false };
    }

    void setEventCallback(EventCallback callback) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(callback);
    }

    qtav::AudioSinkOpenResult open(
        const qtav::AudioFormat& decodedFormat) override
    {
        require(decodedFormat.isValid());
        open_.store(true);
        paused_.store(false);
        ++openCount_;
        notify();
        return { true, decodedFormat, {} };
    }

    void close() noexcept override
    {
        open_.store(false);
        ++closeCount_;
        notify();
    }

    void pause(bool paused) override
    {
        paused_.store(paused);
        if (paused) {
            ++pauseCount_;
        }
        notify();
    }

    void flush() override
    {
        ++flushCount_;
        notify();
    }

    bool write(const qtav::AudioBufferView& buffer) override
    {
        require(buffer.isValid());
        if (!open_.load() || paused_.load()) {
            return false;
        }
        ++writeCount_;
        notify();
        return true;
    }

    bool drain() override
    {
        ++drainCount_;
        notify();
        return true;
    }

    qtav::AudioSinkClock clock() const noexcept override { return {}; }

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
    int flushCount() const noexcept { return flushCount_.load(); }
    int writeCount() const noexcept { return writeCount_.load(); }
    int drainCount() const noexcept { return drainCount_.load(); }

private:
    void notify()
    {
        changed_.notify_all();
    }

    mutable std::mutex mutex_;
    std::mutex waitMutex_;
    std::condition_variable changed_;
    EventCallback callback_;
    std::atomic<bool> open_ { false };
    std::atomic<bool> paused_ { false };
    std::atomic<int> openCount_ { 0 };
    std::atomic<int> closeCount_ { 0 };
    std::atomic<int> pauseCount_ { 0 };
    std::atomic<int> flushCount_ { 0 };
    std::atomic<int> writeCount_ { 0 };
    std::atomic<int> drainCount_ { 0 };
};

class CountingTimeStretcher final : public qtav::AudioTimeStretcher {
public:
    qtav::AudioTimeStretchOpenResult open(
        const qtav::AudioFormat& format,
        double playbackRate) override
    {
        require(format.isValid());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
            rates_.push_back(playbackRate);
        }
        ++openCount_;
        notify();
        return { true, {} };
    }

    qtav::AudioTimeStretchResult process(
        const qtav::AudioBufferView& buffer) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            require(open_);
        }
        ++processCount_;
        notify();
        return { true, { buffer }, {} };
    }

    qtav::AudioTimeStretchResult drain() override
    {
        ++drainCount_;
        notify();
        return { true, {}, {} };
    }

    bool reset() override
    {
        ++resetCount_;
        notify();
        return true;
    }

    void close() noexcept override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = false;
        }
        ++closeCount_;
        notify();
    }

    bool waitFor(
        const std::function<bool()>& predicate,
        std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        std::unique_lock<std::mutex> lock(waitMutex_);
        return changed_.wait_for(lock, timeout, predicate);
    }

    bool usedRate(double rate) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto value : rates_) {
            if (value > rate - 0.000001 && value < rate + 0.000001) {
                return true;
            }
        }
        return false;
    }

    int openCount() const noexcept { return openCount_.load(); }
    int processCount() const noexcept { return processCount_.load(); }
    int drainCount() const noexcept { return drainCount_.load(); }
    int resetCount() const noexcept { return resetCount_.load(); }
    int closeCount() const noexcept { return closeCount_.load(); }

private:
    void notify() { changed_.notify_all(); }

    mutable std::mutex mutex_;
    std::mutex waitMutex_;
    std::condition_variable changed_;
    bool open_ = false;
    std::vector<double> rates_;
    std::atomic<int> openCount_ { 0 };
    std::atomic<int> processCount_ { 0 };
    std::atomic<int> drainCount_ { 0 };
    std::atomic<int> resetCount_ { 0 };
    std::atomic<int> closeCount_ { 0 };
};

void waitForSeek(qtav::Player& player, std::int64_t position)
{
    std::mutex mutex;
    std::condition_variable changed;
    bool complete = false;
    require(player.seek(
        position,
        qtav::SeekFlag::FromStart,
        [&](std::int64_t result) {
            require(result == position);
            {
                std::lock_guard<std::mutex> lock(mutex);
                complete = true;
            }
            changed.notify_all();
        }));
    std::unique_lock<std::mutex> lock(mutex);
    require(changed.wait_for(
        lock,
        std::chrono::seconds(5),
        [&] { return complete; }));
}

void testBypassAndTransitions(const char* media)
{
    auto sink = std::make_shared<CountingAudioSink>();
    auto stretcher = std::make_shared<CountingTimeStretcher>();
    qtav::Player player;
    player
        .setAudioTimeStretcher(stretcher)
        .setAudioSink(sink);
    player.setLoop(-1);
    player.setMedia(media);
    player.setState(qtav::State::Playing);

    require(sink->waitFor([&] { return sink->writeCount() >= 1; }));
    require(stretcher->openCount() == 0);
    require(stretcher->processCount() == 0);

    player.setPlaybackRate(1.5F);
    require(stretcher->waitFor([&] {
        return stretcher->usedRate(1.5)
            && stretcher->processCount() >= 1;
    }));

    player.setPlaybackRate(0.75F);
    require(stretcher->waitFor([&] {
        return stretcher->usedRate(0.75)
            && stretcher->processCount() >= 2;
    }));

    const int resetsBeforePause = stretcher->resetCount();
    const int writesBeforePause = sink->writeCount();
    player.setState(qtav::State::Paused);
    require(player.waitFor(qtav::State::Paused, 5'000));
    require(sink->waitFor([&] { return sink->pauseCount() >= 1; }));
    require(stretcher->resetCount() == resetsBeforePause);

    player.setState(qtav::State::Playing);
    require(sink->waitFor([&] {
        return sink->writeCount() > writesBeforePause;
    }));
    require(stretcher->resetCount() == resetsBeforePause);
    const int resetsBeforeSeek = stretcher->resetCount();
    waitForSeek(player, 400);
    require(stretcher->waitFor([&] {
        return stretcher->resetCount() > resetsBeforeSeek;
    }));

    player.setState(qtav::State::Stopped);
    require(player.waitFor(qtav::State::Stopped, 5'000));
    require(stretcher->waitFor([&] {
        return stretcher->closeCount() >= 1
            && stretcher->resetCount() > resetsBeforeSeek;
    }));
}

void testNaturalDrain(const char* media)
{
    auto sink = std::make_shared<CountingAudioSink>();
    auto stretcher = std::make_shared<CountingTimeStretcher>();
    qtav::Player player;
    player
        .setAudioTimeStretcher(stretcher)
        .setAudioSink(sink);
    player.setPlaybackRate(1.5F);
    player.setMedia(media);
    player.setState(qtav::State::Playing);

    require(stretcher->waitFor([&] {
        return stretcher->openCount() >= 1
            && stretcher->processCount() >= 1;
    }));
    require(sink->waitFor([&] {
        return sink->drainCount() >= 1
            && sink->closeCount() >= 1;
    }));
    require(player.waitFor(qtav::State::Stopped, 5'000));
    require(stretcher->openCount() >= 1);
    require(stretcher->processCount() >= 1);
    require(stretcher->drainCount() >= 1);
    require(sink->drainCount() == 1);
    require(stretcher->closeCount() == 1);
}

} // namespace

int main(int argc, char** argv)
{
    require(argc == 2);
    testBypassAndTransitions(argv[1]);
    testNaturalDrain(argv[1]);
    return 0;
}
