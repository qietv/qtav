// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/audio_processor.h>
#include <qtav/audio_sink.h>
#include <qtav/player.h>
#include <qtav/video_processor.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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

bool sameFormat(
    const qtav::AudioFormat& left,
    const qtav::AudioFormat& right)
{
    return left.sampleRate == right.sampleRate
        && left.channels == right.channels
        && left.sampleFormat == right.sampleFormat
        && left.channelLayout == right.channelLayout;
}

class CountingAudioSink final : public qtav::AudioSink {
public:
    qtav::AudioSinkCapabilities capabilities() const override
    {
        return { {}, 1, 384'000, 64, true, false };
    }

    void setEventCallback(EventCallback callback) override
    {
        callback_ = std::move(callback);
    }

    qtav::AudioSinkOpenResult open(
        const qtav::AudioFormat& decodedFormat) override
    {
        require(decodedFormat.isValid());
        open_.store(true);
        paused_.store(false);
        hasTimestamp_ = false;
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
        hasTimestamp_ = false;
        ++flushCount_;
        notify();
    }

    bool write(const qtav::AudioBufferView& buffer) override
    {
        require(buffer.isValid());
        if (!open_.load() || paused_.load()) {
            return false;
        }
        require(!hasTimestamp_ || buffer.timestamp >= lastTimestamp_);
        lastTimestamp_ = buffer.timestamp;
        hasTimestamp_ = true;
        samples_.fetch_add(buffer.samplesPerChannel);
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

    int closeCount() const noexcept { return closeCount_.load(); }
    int pauseCount() const noexcept { return pauseCount_.load(); }
    int writeCount() const noexcept { return writeCount_.load(); }
    int drainCount() const noexcept { return drainCount_.load(); }
    std::int64_t samples() const noexcept { return samples_.load(); }

private:
    void notify() { changed_.notify_all(); }

    EventCallback callback_;
    std::mutex waitMutex_;
    std::condition_variable changed_;
    std::atomic<bool> open_ { false };
    std::atomic<bool> paused_ { false };
    std::atomic<int> openCount_ { 0 };
    std::atomic<int> closeCount_ { 0 };
    std::atomic<int> pauseCount_ { 0 };
    std::atomic<int> flushCount_ { 0 };
    std::atomic<int> writeCount_ { 0 };
    std::atomic<int> drainCount_ { 0 };
    std::atomic<std::int64_t> samples_ { 0 };
    std::int64_t lastTimestamp_ = 0;
    bool hasTimestamp_ = false;
};

struct OwnedAudioBuffer {
    qtav::AudioFormat format;
    int samplesPerChannel = 0;
    std::vector<std::vector<std::uint8_t>> planes;
    std::vector<int> lineSizes;
    std::int64_t timestamp = 0;
    std::int64_t duration = 0;

    explicit operator bool() const noexcept { return samplesPerChannel > 0; }

    void assign(const qtav::AudioBufferView& input)
    {
        require(input.isValid());
        format = input.format;
        samplesPerChannel = input.samplesPerChannel;
        lineSizes = input.lineSizes;
        timestamp = input.timestamp;
        duration = input.duration;
        planes.resize(input.planes.size());
        for (std::size_t index = 0; index < planes.size(); ++index) {
            planes[index].resize(static_cast<std::size_t>(lineSizes[index]));
            std::memcpy(
                planes[index].data(),
                input.planes[index],
                planes[index].size());
        }
    }

    qtav::AudioBufferView view() const
    {
        qtav::AudioBufferView result;
        result.format = format;
        result.samplesPerChannel = samplesPerChannel;
        result.lineSizes = lineSizes;
        result.timestamp = timestamp;
        result.duration = duration;
        for (const auto& plane : planes) {
            result.planes.push_back(plane.data());
        }
        return result;
    }

    void clear()
    {
        *this = {};
    }
};

class BufferedAudioProcessor final : public qtav::AudioFrameProcessor {
public:
    explicit BufferedAudioProcessor(bool failFirst = false)
        : failFirst_(failFirst)
    {
    }

    qtav::AudioProcessorOpenResult open(
        const qtav::AudioFormat& format) override
    {
        require(format.isValid());
        format_ = format;
        open_ = true;
        ++openCount_;
        notify();
        return { true, {} };
    }

    qtav::AudioProcessingResult process(
        const qtav::AudioBufferView& buffer) override
    {
        require(open_);
        require(buffer.isValid());
        require(sameFormat(format_, buffer.format));
        const int call = ++processCount_;
        inputSamples_.fetch_add(buffer.samplesPerChannel);
        notify();
        if (failFirst_ && call == 1) {
            return { false, {}, "intentional processor failure" };
        }

        qtav::AudioProcessingResult result { true, {}, {} };
        if (pending_) {
            output_ = std::move(pending_);
            outputSamples_.fetch_add(output_.samplesPerChannel);
            result.buffers.push_back(output_.view());
        }
        pending_.assign(buffer);
        return result;
    }

    qtav::AudioProcessingResult drain() override
    {
        require(open_);
        ++drainCount_;
        notify();
        if (!pending_) {
            return { true, {}, {} };
        }
        output_ = std::move(pending_);
        pending_.clear();
        outputSamples_.fetch_add(output_.samplesPerChannel);
        return { true, { output_.view() }, {} };
    }

    bool reset() override
    {
        require(open_);
        pending_.clear();
        output_.clear();
        ++resetCount_;
        notify();
        return true;
    }

    void close() noexcept override
    {
        open_ = false;
        pending_.clear();
        output_.clear();
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

    int openCount() const noexcept { return openCount_.load(); }
    int processCount() const noexcept { return processCount_.load(); }
    int drainCount() const noexcept { return drainCount_.load(); }
    int resetCount() const noexcept { return resetCount_.load(); }
    int closeCount() const noexcept { return closeCount_.load(); }
    std::int64_t inputSamples() const noexcept { return inputSamples_.load(); }
    std::int64_t outputSamples() const noexcept { return outputSamples_.load(); }

private:
    void notify() { changed_.notify_all(); }

    bool failFirst_ = false;
    bool open_ = false;
    qtav::AudioFormat format_;
    OwnedAudioBuffer pending_;
    OwnedAudioBuffer output_;
    std::mutex waitMutex_;
    std::condition_variable changed_;
    std::atomic<int> openCount_ { 0 };
    std::atomic<int> processCount_ { 0 };
    std::atomic<int> drainCount_ { 0 };
    std::atomic<int> resetCount_ { 0 };
    std::atomic<int> closeCount_ { 0 };
    std::atomic<std::int64_t> inputSamples_ { 0 };
    std::atomic<std::int64_t> outputSamples_ { 0 };
};

class CountingVideoProcessor final : public qtav::VideoFrameProcessor {
public:
    explicit CountingVideoProcessor(
        bool bypass = false,
        bool invalidResult = false)
        : bypass_(bypass)
        , invalidResult_(invalidResult)
    {
    }

    qtav::VideoProcessorOpenResult open(
        const qtav::VideoProcessorFormat& format) override
    {
        require(format.isValid());
        open_ = true;
        hasTimestamp_ = false;
        ++openCount_;
        notify();
        return { true, bypass_, {} };
    }

    qtav::VideoProcessingResult process(
        const qtav::VideoFrame& frame) override
    {
        require(open_);
        require(frame.isValid());
        require(!hasTimestamp_ || frame.timestamp() >= lastTimestamp_);
        lastTimestamp_ = frame.timestamp();
        hasTimestamp_ = true;
        ++processCount_;
        notify();
        if (invalidResult_) {
            return { true, false, {}, {} };
        }
        return { true, false, frame, {} };
    }

    bool drain() override
    {
        require(open_);
        ++drainCount_;
        notify();
        return true;
    }

    bool reset() override
    {
        require(open_);
        hasTimestamp_ = false;
        ++resetCount_;
        notify();
        return true;
    }

    void close() noexcept override
    {
        open_ = false;
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

    int openCount() const noexcept { return openCount_.load(); }
    int processCount() const noexcept { return processCount_.load(); }
    int drainCount() const noexcept { return drainCount_.load(); }
    int resetCount() const noexcept { return resetCount_.load(); }
    int closeCount() const noexcept { return closeCount_.load(); }

private:
    void notify() { changed_.notify_all(); }

    bool bypass_ = false;
    bool invalidResult_ = false;
    bool open_ = false;
    std::mutex waitMutex_;
    std::condition_variable changed_;
    std::atomic<int> openCount_ { 0 };
    std::atomic<int> processCount_ { 0 };
    std::atomic<int> drainCount_ { 0 };
    std::atomic<int> resetCount_ { 0 };
    std::atomic<int> closeCount_ { 0 };
    std::int64_t lastTimestamp_ = 0;
    bool hasTimestamp_ = false;
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

void testBufferedNaturalDrain(const char* media)
{
    auto sink = std::make_shared<CountingAudioSink>();
    auto audio = std::make_shared<BufferedAudioProcessor>();
    auto video = std::make_shared<CountingVideoProcessor>();
    std::atomic<int> decodedAudio { 0 };
    std::atomic<int> presentedVideo { 0 };
    qtav::Player player;
    player
        .setAudioFrameProcessor(audio)
        .setAudioSink(sink)
        .setVideoFrameProcessor(video)
        .onAudioFrame([&](const qtav::AudioFrame& frame, int) {
            require(frame.isValid());
            ++decodedAudio;
        })
        .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
            require(frame.isValid());
            ++presentedVideo;
        });
    player.setMedia(media);
    player.setState(qtav::State::Playing);

    require(audio->waitFor([&] { return audio->processCount() > 1; }));
    require(video->waitFor([&] { return video->processCount() > 0; }));
    require(player.waitFor(qtav::State::Stopped, 10'000));
    require(decodedAudio.load() > 0);
    require(presentedVideo.load() > 0);
    require(audio->openCount() == 1);
    require(audio->processCount() > 1);
    require(audio->drainCount() == 2);
    require(audio->closeCount() == 1);
    require(audio->inputSamples() == audio->outputSamples());
    require(sink->samples() == audio->outputSamples());
    require(sink->drainCount() == 1);
    require(video->openCount() >= 1);
    require(video->processCount() > 0);
    require(video->drainCount() == 1);
    require(video->closeCount() >= 1);
}

void testPauseSeekAndStop(const char* media)
{
    auto sink = std::make_shared<CountingAudioSink>();
    auto audio = std::make_shared<BufferedAudioProcessor>();
    auto video = std::make_shared<CountingVideoProcessor>();
    qtav::Player player;
    player
        .setAudioFrameProcessor(audio)
        .setAudioSink(sink)
        .setVideoFrameProcessor(video);
    player.setLoop(-1);
    player.setMedia(media);
    player.setState(qtav::State::Playing);
    require(audio->waitFor([&] { return audio->processCount() >= 2; }));
    require(video->waitFor([&] { return video->processCount() >= 1; }));

    const int audioResetsBeforePause = audio->resetCount();
    const int videoResetsBeforePause = video->resetCount();
    player.setState(qtav::State::Paused);
    require(player.waitFor(qtav::State::Paused, 5'000));
    require(sink->waitFor([&] { return sink->pauseCount() >= 1; }));
    require(audio->resetCount() == audioResetsBeforePause);
    require(video->resetCount() == videoResetsBeforePause);

    player.setState(qtav::State::Playing);
    require(player.waitFor(qtav::State::Playing, 5'000));
    waitForSeek(player, 400);
    require(audio->waitFor([&] {
        return audio->resetCount() > audioResetsBeforePause;
    }));
    require(video->waitFor([&] {
        return video->resetCount() > videoResetsBeforePause;
    }));

    player.setState(qtav::State::Stopped);
    require(player.waitFor(qtav::State::Stopped, 5'000));
    require(audio->waitFor([&] { return audio->closeCount() >= 1; }));
    require(video->waitFor([&] { return video->closeCount() >= 1; }));
}

void testLoopDrainAndReset(const char* media)
{
    auto sink = std::make_shared<CountingAudioSink>();
    auto audio = std::make_shared<BufferedAudioProcessor>();
    auto video = std::make_shared<CountingVideoProcessor>();
    qtav::Player player;
    player
        .setAudioFrameProcessor(audio)
        .setAudioSink(sink)
        .setVideoFrameProcessor(video);
    player.setLoop(1);
    player.setMedia(media);
    player.setState(qtav::State::Playing);
    require(audio->waitFor([&] { return audio->drainCount() >= 2; }));
    require(audio->waitFor([&] { return audio->resetCount() >= 1; }));
    require(video->waitFor([&] { return video->resetCount() >= 1; }));
    require(player.waitFor(qtav::State::Stopped, 10'000));
    require(audio->drainCount() == 4);
    require(video->drainCount() == 2);
    require(sink->drainCount() == 2);
    require(audio->closeCount() == 1);
    require(video->closeCount() >= 1);
}

void testVideoBypass(const char* media)
{
    auto video = std::make_shared<CountingVideoProcessor>(true);
    std::atomic<int> presented { 0 };
    qtav::Player player;
    player
        .setVideoFrameProcessor(video)
        .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
            require(frame.isValid());
            ++presented;
        });
    player.setMedia(media);
    player.setState(qtav::State::Playing);
    require(video->waitFor([&] { return video->openCount() >= 1; }));
    require(player.waitFor(qtav::State::Stopped, 10'000));
    require(presented.load() > 0);
    require(video->openCount() >= 1);
    require(video->processCount() == 0);
    require(video->drainCount() == 0);
    require(video->closeCount() >= 1);
}

void testAudioFailureClosesOutput(const char* media)
{
    auto sink = std::make_shared<CountingAudioSink>();
    auto audio = std::make_shared<BufferedAudioProcessor>(true);
    std::mutex mutex;
    std::condition_variable changed;
    bool sawFailure = false;
    qtav::Player player;
    player
        .setAudioFrameProcessor(audio)
        .setAudioSink(sink)
        .onEvent([&](const qtav::MediaEvent& event) {
            if (event.category == "audio.processor.process") {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    sawFailure = true;
                }
                changed.notify_all();
                return true;
            }
            return false;
        });
    player.setMedia(media);
    player.setState(qtav::State::Playing);
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(changed.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return sawFailure; }));
    }
    require(sink->waitFor([&] { return sink->closeCount() >= 1; }));
    require(audio->processCount() == 1);
    require(audio->closeCount() >= 1);
    player.setState(qtav::State::Stopped);
    require(player.waitFor(qtav::State::Stopped, 5'000));
}

void testVideoContractFailureClosesProcessor(const char* media)
{
    auto video = std::make_shared<CountingVideoProcessor>(false, true);
    std::mutex mutex;
    std::condition_variable changed;
    bool sawFailure = false;
    qtav::Player player;
    player
        .setVideoFrameProcessor(video)
        .onEvent([&](const qtav::MediaEvent& event) {
            if (event.category == "video.processor.contract") {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    sawFailure = true;
                }
                changed.notify_all();
                return true;
            }
            return false;
        });
    player.setMedia(media);
    player.setState(qtav::State::Playing);
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(changed.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return sawFailure; }));
    }
    require(video->processCount() == 1);
    require(video->closeCount() >= 1);
    player.setState(qtav::State::Stopped);
    require(player.waitFor(qtav::State::Stopped, 5'000));
}

} // namespace

int main(int argc, char** argv)
{
    require(argc == 2);
    testBufferedNaturalDrain(argv[1]);
    testPauseSeekAndStop(argv[1]);
    testLoopDrainAndReset(argv[1]);
    testVideoBypass(argv[1]);
    testAudioFailureClosesOutput(argv[1]);
    testVideoContractFailureClosesProcessor(argv[1]);
    return 0;
}
