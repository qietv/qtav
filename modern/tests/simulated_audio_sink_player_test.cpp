// SPDX-License-Identifier: LGPL-2.1-or-later

#include "simulated_audio_sink.h"

#include <qtav/player.h>
#include <qtav/swresample_audio_converter.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

using Snapshot = qtav::test::SimulatedAudioSinkSnapshot;

void waitForSeek(
    qtav::Player& player,
    std::int64_t position)
{
    std::mutex mutex;
    std::condition_variable changed;
    bool complete = false;
    assert(player.seek(
        position,
        qtav::SeekFlag::FromStart,
        [&](std::int64_t result) {
            assert(result == position);
            {
                std::lock_guard<std::mutex> lock(mutex);
                complete = true;
            }
            changed.notify_all();
        }));
    std::unique_lock<std::mutex> lock(mutex);
    assert(changed.wait_for(
        lock,
        std::chrono::seconds(5),
        [&] { return complete; }));
}

bool hasTimestampAtOrAfter(
    const Snapshot& state,
    std::int64_t timestamp)
{
    return std::any_of(
        state.writeTimestamps.begin(),
        state.writeTimestamps.end(),
        [timestamp](std::int64_t value) { return value >= timestamp; });
}

void testDeviceMasterResamplingAndDrain(const char* media)
{
    const qtav::AudioFormat output {
        24'000,
        2,
        qtav::SampleFormat::S16,
        "stereo",
    };
    auto sink = std::make_shared<qtav::test::SimulatedAudioSink>(
        qtav::test::SimulatedAudioSinkConfig {
            output,
            500,
            40,
            0,
            25,
            true,
            true,
            true,
        });
    auto converter =
        std::make_shared<qtav::SwresampleAudioConverter>();
    std::mutex samplesMutex;
    std::vector<std::pair<std::int64_t, std::int64_t>> videoClockSamples;
    std::vector<std::string> audioErrors;

    qtav::Player player;
    player
        .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
            const auto masterPosition = player.position();
            std::lock_guard<std::mutex> lock(samplesMutex);
            videoClockSamples.emplace_back(
                frame.timestamp(),
                masterPosition);
        })
        .onEvent([&](const qtav::MediaEvent& event) {
            if (event.category.find("audio.") == 0) {
                std::lock_guard<std::mutex> lock(samplesMutex);
                audioErrors.push_back(event.category);
            }
            return false;
        })
        .setAudioFrameConverter(converter)
        .setAudioSink(sink);
    player.setMedia(media);
    player.setState(qtav::State::Playing);

    assert(sink->waitFor([](const Snapshot& state) {
        return state.drainCount >= 1 && state.closeCount >= 1;
    }));
    assert(player.waitFor(qtav::State::Stopped, 5'000));
    const auto state = sink->snapshot();
    assert(state.openCount == 1);
    assert(state.closeCount == 1);
    assert(state.writeCount > 0);
    assert(state.drainCount == 1);
    assert(state.queuedMilliseconds == 0);
    assert(state.deviceFormat.sampleRate == output.sampleRate);
    assert(state.deviceFormat.channels == output.channels);
    assert(state.deviceFormat.sampleFormat == output.sampleFormat);
    assert(state.clockPositionMilliseconds >= 950);
    {
        std::lock_guard<std::mutex> lock(samplesMutex);
        assert(!videoClockSamples.empty());
        for (const auto& sample : videoClockSamples) {
            assert(sample.second + 2 >= sample.first);
        }
        assert(audioErrors.empty());
    }
}

void testSeekAndMediaReplacement(const char* media)
{
    auto sink = std::make_shared<qtav::test::SimulatedAudioSink>(
        qtav::test::SimulatedAudioSinkConfig {
            {},
            500,
            30,
            0,
            10,
            true,
            true,
            true,
        });
    qtav::Player player;
    player.setAudioSink(sink);
    player.setMedia(media);
    player.setState(qtav::State::Playing);

    assert(sink->waitFor(
        [](const Snapshot& state) { return state.writeCount >= 1; }));
    player.setState(qtav::State::Paused);
    assert(player.waitFor(qtav::State::Paused, 5'000));
    assert(sink->waitFor(
        [](const Snapshot& state) { return state.pauseCount >= 1; }));

    const auto beforeSeek = sink->snapshot();
    waitForSeek(player, 500);
    assert(sink->waitFor([&](const Snapshot& state) {
        return state.flushCount > beforeSeek.flushCount;
    }));

    player.setState(qtav::State::Playing);
    assert(sink->waitFor([](const Snapshot& state) {
        return hasTimestampAtOrAfter(state, 480);
    }));

    const auto beforeReplacement = sink->snapshot();
    player.setMedia(media);
    player.setState(qtav::State::Playing);
    assert(sink->waitFor([&](const Snapshot& state) {
        return state.openCount > beforeReplacement.openCount
            && state.closeCount > beforeReplacement.closeCount;
    }));

    player.setState(qtav::State::Stopped);
    assert(player.waitFor(qtav::State::Stopped, 5'000));
    assert(sink->waitFor([](const Snapshot& state) {
        return !state.open && state.closeCount >= 2;
    }));
}

void testLoopReanchorsDeviceClock(const char* media)
{
    constexpr int repeatCount = 1;
    auto sink = std::make_shared<qtav::test::SimulatedAudioSink>(
        qtav::test::SimulatedAudioSinkConfig {
            {},
            500,
            20,
            0,
            25,
            true,
            true,
            true,
        });
    qtav::Player player;
    player.setAudioSink(sink);
    player.setRange(200, 600);
    player.setLoop(repeatCount);
    player.setMedia(media);
    player.setState(qtav::State::Playing);

    assert(sink->waitFor([](const Snapshot& state) {
        return state.drainCount >= 1 && state.closeCount >= 1;
    }));
    assert(player.waitFor(qtav::State::Stopped, 5'000));
    const auto state = sink->snapshot();
    assert(state.flushCount >= 1);
    assert(state.drainCount == repeatCount + 1);
    assert(state.writeCount > 2);
    assert(std::adjacent_find(
               state.writeTimestamps.begin(),
               state.writeTimestamps.end(),
               std::greater<std::int64_t>())
        != state.writeTimestamps.end());
    assert(state.clockPositionMilliseconds >= 550);
    assert(state.clockPositionMilliseconds <= 650);
}

void testInvalidDeviceClockFallsBackToMonotonic(const char* media)
{
    auto sink = std::make_shared<qtav::test::SimulatedAudioSink>(
        qtav::test::SimulatedAudioSinkConfig {
            {},
            2'000,
            15,
            0,
            0,
            true,
            true,
            false,
        });
    std::atomic<int> videoFrames { 0 };
    qtav::Player player;
    player
        .onVideoFrame([&](const qtav::VideoFrame&, int) {
            ++videoFrames;
        })
        .setAudioSink(sink);
    player.setPlaybackRate(20.0F);
    player.setMedia(media);
    player.setState(qtav::State::Playing);

    assert(sink->waitFor([](const Snapshot& state) {
        return state.drainCount >= 1 && state.closeCount >= 1;
    }));
    assert(player.waitFor(qtav::State::Stopped, 5'000));
    const auto state = sink->snapshot();
    assert(state.clockCount > 0);
    assert(state.writeCount > 0);
    assert(state.drainCount == 1);
    assert(state.clockPositionMilliseconds >= 950);
    assert(videoFrames.load() > 0);
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    testDeviceMasterResamplingAndDrain(argv[1]);
    testSeekAndMediaReplacement(argv[1]);
    testLoopReanchorsDeviceClock(argv[1]);
    testInvalidDeviceClockFallsBackToMonotonic(argv[1]);
    return 0;
}
