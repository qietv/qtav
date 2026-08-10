// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <qtav/audio_sink.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qtav::test {

struct SimulatedAudioSinkConfig {
    AudioFormat negotiatedFormat;
    std::int64_t queueCapacityMilliseconds = 2'000;
    std::int64_t deviceLatencyMilliseconds = 0;
    std::int64_t initialClockPositionMilliseconds = 0;
    std::int64_t autoAdvanceOnClockMilliseconds = 0;
    bool supportsPause = true;
    bool hasDeviceClock = true;
    bool clockInitiallyValid = true;
    // Model a real device after time stretching: queue and clock duration are
    // derived from PCM samples, while writeTimestamps retain media time.
    bool useSampleDuration = false;
};

struct SimulatedAudioSinkSnapshot {
    bool open = false;
    bool paused = false;
    bool clockValid = false;
    AudioFormat decodedFormat;
    AudioFormat deviceFormat;
    std::int64_t clockPositionMilliseconds = 0;
    std::int64_t queuedMilliseconds = 0;
    std::int64_t reportedLatencyMilliseconds = 0;
    int openCount = 0;
    int closeCount = 0;
    int pauseCount = 0;
    int resumeCount = 0;
    int flushCount = 0;
    int writeCount = 0;
    int rejectedWriteCount = 0;
    int drainCount = 0;
    int underrunCount = 0;
    int clockCount = 0;
    std::vector<std::int64_t> writeTimestamps;
};

class SimulatedAudioSink final : public AudioSink {
public:
    explicit SimulatedAudioSink(SimulatedAudioSinkConfig config = {});
    ~SimulatedAudioSink() override;

    SimulatedAudioSink(const SimulatedAudioSink&) = delete;
    SimulatedAudioSink& operator=(const SimulatedAudioSink&) = delete;

    AudioSinkCapabilities capabilities() const override;
    void setEventCallback(EventCallback callback) override;
    AudioSinkOpenResult open(const AudioFormat& decodedFormat) override;
    void close() noexcept override;
    void pause(bool paused) override;
    void flush() override;
    bool write(const AudioBufferView& buffer) override;
    bool drain() override;
    AudioSinkClock clock() const noexcept override;

    // Advances the simulated device without consulting a wall clock.
    void advance(std::int64_t milliseconds);
    void setClockValid(bool valid);
    SimulatedAudioSinkSnapshot snapshot() const;
    bool waitFor(
        const std::function<bool(const SimulatedAudioSinkSnapshot&)>& predicate,
        std::chrono::milliseconds timeout = std::chrono::seconds(5)) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav::test
