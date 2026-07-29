// SPDX-License-Identifier: LGPL-2.1-or-later

#include "simulated_audio_sink.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace qtav::test {
namespace {

bool sameFormat(const AudioFormat& left, const AudioFormat& right)
{
    return left.sampleRate == right.sampleRate
        && left.channels == right.channels
        && left.sampleFormat == right.sampleFormat
        && left.channelLayout == right.channelLayout;
}

} // namespace

struct SimulatedAudioSink::Impl {
    struct QueuedBuffer {
        std::int64_t timestamp = 0;
        std::int64_t duration = 0;
        std::int64_t consumed = 0;
    };

    explicit Impl(SimulatedAudioSinkConfig value)
        : config(std::move(value))
        , clockValid(config.clockInitiallyValid)
        , clockPosition(std::max<std::int64_t>(
              0,
              config.initialClockPositionMilliseconds))
    {
        config.queueCapacityMilliseconds =
            std::max<std::int64_t>(0, config.queueCapacityMilliseconds);
        config.deviceLatencyMilliseconds =
            std::max<std::int64_t>(0, config.deviceLatencyMilliseconds);
        config.autoAdvanceOnClockMilliseconds =
            std::max<std::int64_t>(
                0,
                config.autoAdvanceOnClockMilliseconds);
    }

    SimulatedAudioSinkSnapshot snapshotLocked() const
    {
        SimulatedAudioSinkSnapshot result;
        result.open = open;
        result.paused = paused;
        result.clockValid = clockValid;
        result.decodedFormat = decodedFormat;
        result.deviceFormat = deviceFormat;
        result.clockPositionMilliseconds = clockPosition;
        result.queuedMilliseconds = queuedDuration;
        result.reportedLatencyMilliseconds =
            config.deviceLatencyMilliseconds + queuedDuration;
        result.openCount = openCount;
        result.closeCount = closeCount;
        result.pauseCount = pauseCount;
        result.resumeCount = resumeCount;
        result.flushCount = flushCount;
        result.writeCount = writeCount;
        result.rejectedWriteCount = rejectedWriteCount;
        result.drainCount = drainCount;
        result.underrunCount = underrunCount;
        result.clockCount = clockCount;
        result.writeTimestamps = writeTimestamps;
        return result;
    }

    bool advanceLocked(std::int64_t milliseconds, bool reportUnderrun)
    {
        if (!open || paused || milliseconds <= 0) {
            return false;
        }

        auto remaining = milliseconds;
        if (!queue.empty() && startupDelayRemaining > 0) {
            const auto delay = std::min(remaining, startupDelayRemaining);
            startupDelayRemaining -= delay;
            remaining -= delay;
        }

        while (remaining > 0 && !queue.empty()) {
            auto& front = queue.front();
            if (front.consumed == 0) {
                clockPosition =
                    std::max<std::int64_t>(0, front.timestamp);
            }
            const auto available = front.duration - front.consumed;
            const auto consumed = std::min(remaining, available);
            front.consumed += consumed;
            queuedDuration -= consumed;
            remaining -= consumed;
            clockPosition = std::max<std::int64_t>(
                0,
                front.timestamp + front.consumed);
            if (front.consumed >= front.duration) {
                queue.pop_front();
            }
        }

        if (!queue.empty()) {
            underrunActive = false;
            return false;
        }
        if (remaining <= 0 || !reportUnderrun || underrunActive) {
            return false;
        }
        underrunActive = true;
        ++underrunCount;
        return true;
    }

    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    SimulatedAudioSinkConfig config;
    EventCallback callback;
    AudioFormat decodedFormat;
    AudioFormat deviceFormat;
    std::deque<QueuedBuffer> queue;
    std::vector<std::int64_t> writeTimestamps;
    bool open = false;
    bool paused = false;
    bool clockValid = true;
    bool needsAnchor = true;
    bool underrunActive = false;
    std::int64_t clockPosition = 0;
    std::int64_t queuedDuration = 0;
    std::int64_t startupDelayRemaining = 0;
    int openCount = 0;
    int closeCount = 0;
    int pauseCount = 0;
    int resumeCount = 0;
    int flushCount = 0;
    int writeCount = 0;
    int rejectedWriteCount = 0;
    int drainCount = 0;
    int underrunCount = 0;
    mutable int clockCount = 0;
};

SimulatedAudioSink::SimulatedAudioSink(SimulatedAudioSinkConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

SimulatedAudioSink::~SimulatedAudioSink() = default;

AudioSinkCapabilities SimulatedAudioSink::capabilities() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    AudioSinkCapabilities result;
    if (impl_->config.negotiatedFormat.isValid()) {
        result.sampleFormats = {
            impl_->config.negotiatedFormat.sampleFormat,
        };
        result.minimumSampleRate =
            impl_->config.negotiatedFormat.sampleRate;
        result.maximumSampleRate =
            impl_->config.negotiatedFormat.sampleRate;
        result.maximumChannels = impl_->config.negotiatedFormat.channels;
    } else {
        result.minimumSampleRate = 1;
        result.maximumSampleRate = 384'000;
        result.maximumChannels = 64;
    }
    result.supportsPause = impl_->config.supportsPause;
    result.hasDeviceClock = impl_->config.hasDeviceClock;
    return result;
}

void SimulatedAudioSink::setEventCallback(EventCallback callback)
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->callback = std::move(callback);
    }
    impl_->changed.notify_all();
}

AudioSinkOpenResult SimulatedAudioSink::open(
    const AudioFormat& decodedFormat)
{
    AudioSinkOpenResult result;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!decodedFormat.isValid()) {
            result.error = "invalid decoded audio format";
            return result;
        }
        impl_->decodedFormat = decodedFormat;
        impl_->deviceFormat = impl_->config.negotiatedFormat.isValid()
            ? impl_->config.negotiatedFormat
            : decodedFormat;
        impl_->queue.clear();
        impl_->queuedDuration = 0;
        impl_->startupDelayRemaining =
            impl_->config.deviceLatencyMilliseconds;
        impl_->clockPosition = std::max<std::int64_t>(
            0,
            impl_->config.initialClockPositionMilliseconds);
        impl_->clockValid = impl_->config.clockInitiallyValid;
        impl_->needsAnchor = true;
        impl_->underrunActive = false;
        impl_->open = true;
        impl_->paused = false;
        ++impl_->openCount;
        result = { true, impl_->deviceFormat, {} };
    }
    impl_->changed.notify_all();
    return result;
}

void SimulatedAudioSink::close() noexcept
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->open = false;
        impl_->paused = false;
        impl_->queue.clear();
        impl_->queuedDuration = 0;
        impl_->startupDelayRemaining = 0;
        ++impl_->closeCount;
    }
    impl_->changed.notify_all();
}

void SimulatedAudioSink::pause(bool paused)
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->open || impl_->paused == paused) {
            return;
        }
        impl_->paused = paused;
        if (paused) {
            ++impl_->pauseCount;
        } else {
            ++impl_->resumeCount;
        }
    }
    impl_->changed.notify_all();
}

void SimulatedAudioSink::flush()
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->queue.clear();
        impl_->queuedDuration = 0;
        impl_->startupDelayRemaining =
            impl_->config.deviceLatencyMilliseconds;
        impl_->needsAnchor = true;
        impl_->underrunActive = false;
        ++impl_->flushCount;
    }
    impl_->changed.notify_all();
}

bool SimulatedAudioSink::write(const AudioBufferView& buffer)
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->open || impl_->paused || !buffer.isValid()
            || !sameFormat(buffer.format, impl_->deviceFormat)
            || buffer.duration <= 0) {
            ++impl_->rejectedWriteCount;
            return false;
        }
        if (impl_->config.queueCapacityMilliseconds > 0
            && impl_->queuedDuration + buffer.duration
                > impl_->config.queueCapacityMilliseconds) {
            ++impl_->rejectedWriteCount;
            return false;
        }
        if (impl_->needsAnchor) {
            impl_->clockPosition =
                std::max<std::int64_t>(0, buffer.timestamp);
            impl_->needsAnchor = false;
        }
        impl_->queue.push_back({
            buffer.timestamp,
            buffer.duration,
            0,
        });
        impl_->queuedDuration += buffer.duration;
        impl_->writeTimestamps.push_back(buffer.timestamp);
        impl_->underrunActive = false;
        ++impl_->writeCount;
    }
    impl_->changed.notify_all();
    return true;
}

bool SimulatedAudioSink::drain()
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->open) {
            return false;
        }
        if (!impl_->queue.empty()) {
            const auto& last = impl_->queue.back();
            impl_->clockPosition = std::max<std::int64_t>(
                0,
                last.timestamp + last.duration);
        }
        impl_->queue.clear();
        impl_->queuedDuration = 0;
        impl_->startupDelayRemaining = 0;
        impl_->underrunActive = false;
        ++impl_->drainCount;
    }
    impl_->changed.notify_all();
    return true;
}

AudioSinkClock SimulatedAudioSink::clock() const noexcept
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ++impl_->clockCount;
    if (impl_->config.autoAdvanceOnClockMilliseconds > 0) {
        impl_->advanceLocked(
            impl_->config.autoAdvanceOnClockMilliseconds,
            false);
    }
    return {
        impl_->open && impl_->config.hasDeviceClock && impl_->clockValid,
        impl_->clockPosition,
        impl_->config.deviceLatencyMilliseconds
            + impl_->queuedDuration,
    };
}

void SimulatedAudioSink::advance(std::int64_t milliseconds)
{
    EventCallback callback;
    bool underrun = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        underrun =
            impl_->advanceLocked(std::max<std::int64_t>(0, milliseconds), true);
        if (underrun) {
            callback = impl_->callback;
        }
    }
    impl_->changed.notify_all();
    if (callback) {
        callback({
            AudioSinkEventType::Underrun,
            "simulated audio queue underrun",
        });
    }
}

void SimulatedAudioSink::setClockValid(bool valid)
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->clockValid = valid;
    }
    impl_->changed.notify_all();
}

SimulatedAudioSinkSnapshot SimulatedAudioSink::snapshot() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->snapshotLocked();
}

bool SimulatedAudioSink::waitFor(
    const std::function<bool(const SimulatedAudioSinkSnapshot&)>& predicate,
    std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lock(impl_->mutex);
    return impl_->changed.wait_for(lock, timeout, [&] {
        return predicate(impl_->snapshotLocked());
    });
}

} // namespace qtav::test
