// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/player.h>
#include <qtav/swresample_audio_converter.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

const qtav::AudioFormat OutputFormat {
    16'000,
    2,
    qtav::SampleFormat::S16,
    "stereo",
};

class ConvertedAudioSink final : public qtav::AudioSink {
public:
    qtav::AudioSinkCapabilities capabilities() const override
    {
        return {
            { qtav::SampleFormat::S16 },
            16'000,
            16'000,
            2,
            true,
            false,
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
        assert(decodedFormat.sampleRate == 8'000);
        assert(decodedFormat.channels == 1);
        assert(decodedFormat.sampleFormat == qtav::SampleFormat::S16);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
            paused_ = false;
            ++openCount_;
        }
        changed_.notify_all();
        return { true, OutputFormat, {} };
    }

    void close() noexcept override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = false;
            ++closeCount_;
        }
        changed_.notify_all();
    }

    void pause(bool paused) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            paused_ = paused;
            if (paused) {
                ++pauseCount_;
            }
        }
        changed_.notify_all();
    }

    void flush() override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++flushCount_;
            hasPreviousEnd_ = false;
        }
        changed_.notify_all();
    }

    bool write(const qtav::AudioBufferView& buffer) override
    {
        assert(buffer.isValid());
        assert(buffer.format.sampleRate == OutputFormat.sampleRate);
        assert(buffer.format.channels == OutputFormat.channels);
        assert(buffer.format.sampleFormat == OutputFormat.sampleFormat);
        assert(buffer.format.channelLayout == OutputFormat.channelLayout);
        assert(buffer.planes.size() == 1);
        assert(buffer.lineSizes.size() == 1);
        assert(buffer.lineSizes[0]
            == buffer.samplesPerChannel * OutputFormat.channels
                * static_cast<int>(sizeof(std::int16_t)));

        bool nonZero = false;
        bool stereoEqual = true;
        for (int sample = 0; sample < buffer.samplesPerChannel; ++sample) {
            std::int16_t left = 0;
            std::int16_t right = 0;
            std::memcpy(
                &left,
                buffer.planes[0]
                    + static_cast<std::size_t>(sample * 4),
                sizeof(left));
            std::memcpy(
                &right,
                buffer.planes[0]
                    + static_cast<std::size_t>(sample * 4 + 2),
                sizeof(right));
            nonZero = nonZero || left != 0 || right != 0;
            stereoEqual = stereoEqual && left == right;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || paused_) {
                return false;
            }
            if (checkContinuity_ && hasPreviousEnd_) {
                continuous_ =
                    continuous_ && buffer.timestamp == previousEnd_;
            }
            previousEnd_ = buffer.timestamp + buffer.duration;
            hasPreviousEnd_ = true;
            totalSamples_ += buffer.samplesPerChannel;
            nonZero_ = nonZero_ || nonZero;
            stereoEqual_ = stereoEqual_ && stereoEqual;
            ++writeCount_;
            if (captureAfterSeek_) {
                afterSeekTimestamp_ = buffer.timestamp;
                captureAfterSeek_ = false;
            }
        }
        changed_.notify_all();
        return true;
    }

    qtav::AudioSinkClock clock() const noexcept override
    {
        return {};
    }

    bool waitFor(
        const std::function<bool()>& predicate,
        std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            lock.unlock();
            const bool done = predicate();
            lock.lock();
            if (done) {
                return true;
            }
            if (changed_.wait_until(lock, deadline)
                == std::cv_status::timeout) {
                lock.unlock();
                return predicate();
            }
        }
    }

    void captureNextTimestamp()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        captureAfterSeek_ = true;
        afterSeekTimestamp_.reset();
    }

    void setCheckContinuity(bool value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        checkContinuity_ = value;
        hasPreviousEnd_ = false;
    }

    int openCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return openCount_;
    }
    int closeCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return closeCount_;
    }
    int pauseCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return pauseCount_;
    }
    int flushCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return flushCount_;
    }
    int writeCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return writeCount_;
    }
    std::int64_t totalSamples() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return totalSamples_;
    }
    std::int64_t previousEnd() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return previousEnd_;
    }
    bool continuous() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return continuous_;
    }
    bool nonZero() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return nonZero_;
    }
    bool stereoEqual() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stereoEqual_;
    }
    std::optional<std::int64_t> afterSeekTimestamp() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return afterSeekTimestamp_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    EventCallback callback_;
    bool open_ = false;
    bool paused_ = false;
    bool checkContinuity_ = true;
    bool continuous_ = true;
    bool nonZero_ = false;
    bool stereoEqual_ = true;
    bool hasPreviousEnd_ = false;
    bool captureAfterSeek_ = false;
    int openCount_ = 0;
    int closeCount_ = 0;
    int pauseCount_ = 0;
    int flushCount_ = 0;
    int writeCount_ = 0;
    std::int64_t totalSamples_ = 0;
    std::int64_t previousEnd_ = 0;
    std::optional<std::int64_t> afterSeekTimestamp_;
};

void testNaturalDrain(const char* media)
{
    auto sink = std::make_shared<ConvertedAudioSink>();
    auto converter =
        std::make_shared<qtav::SwresampleAudioConverter>();

    const qtav::AudioFormat input {
        8'000,
        1,
        qtav::SampleFormat::S16,
        "mono",
    };
    auto planarOutput = OutputFormat;
    planarOutput.sampleFormat = qtav::SampleFormat::S16Planar;
    assert(!converter->open(input, planarOutput).success);
    converter->close();

    std::atomic<int> decodedFrames { 0 };
    std::mutex eventMutex;
    std::vector<std::string> errors;
    qtav::Player player;
    player
        .onAudioFrame([&](const qtav::AudioFrame&, int) {
            ++decodedFrames;
        })
        .onEvent([&](const qtav::MediaEvent& event) {
            if (event.category.find("audio.converter") == 0) {
                std::lock_guard<std::mutex> lock(eventMutex);
                errors.push_back(event.category);
            }
            return false;
        })
        .setAudioFrameConverter(converter)
        .setAudioSink(sink);
    player.setMedia(media);
    player.setState(qtav::State::Playing);

    assert(sink->waitFor([&] { return sink->closeCount() >= 1; }));
    assert(player.waitFor(qtav::State::Stopped, 5'000));
    assert(sink->openCount() == 1);
    assert(sink->writeCount() >= 1);
    assert(decodedFrames.load() >= 1);
    assert(sink->totalSamples() == 16'000);
    assert(sink->previousEnd() == 1'000);
    assert(sink->continuous());
    assert(sink->nonZero());
    assert(sink->stereoEqual());
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        assert(errors.empty());
    }
}

void testSeekResetsTimeline(const char* media)
{
    auto sink = std::make_shared<ConvertedAudioSink>();
    auto converter =
        std::make_shared<qtav::SwresampleAudioConverter>();
    sink->setCheckContinuity(false);

    qtav::Player player;
    player
        .setAudioFrameConverter(converter)
        .setAudioSink(sink);
    player.setMedia(media);
    player.setState(qtav::State::Playing);
    assert(sink->waitFor([&] { return sink->writeCount() >= 1; }));

    player.setState(qtav::State::Paused);
    assert(player.waitFor(qtav::State::Paused, 5'000));
    assert(sink->waitFor([&] { return sink->pauseCount() >= 1; }));

    std::mutex seekMutex;
    std::condition_variable seekChanged;
    bool seeked = false;
    assert(player.seek(
        500,
        qtav::SeekFlag::FromStart,
        [&](std::int64_t position) {
            assert(position == 500);
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
    assert(sink->flushCount() >= 1);

    sink->captureNextTimestamp();
    player.setState(qtav::State::Playing);
    assert(sink->waitFor([&] {
        return sink->afterSeekTimestamp().has_value();
    }));
    const auto timestamp = *sink->afterSeekTimestamp();
    assert(timestamp >= 480);
    assert(timestamp <= 550);

    player.setState(qtav::State::Stopped);
    assert(player.waitFor(qtav::State::Stopped, 5'000));
    assert(sink->waitFor([&] { return sink->closeCount() >= 1; }));
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    testNaturalDrain(argv[1]);
    testSeekResetsTimeline(argv[1]);
    return 0;
}
