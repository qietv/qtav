// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace qtav::detail {

// Fixed-capacity single-producer/single-consumer PCM queue. configure() and
// clear() require the consumer to be stopped. push() and pop() may then run
// concurrently without locks or allocation.
class AAudioPcmQueue final {
public:
    struct PopResult {
        int frames = 0;
        std::int64_t firstTimestampNanoseconds = 0;
    };

    bool configure(int capacityFrames, int channels, int sampleRate)
    {
        if (capacityFrames <= 0 || channels <= 0 || sampleRate <= 0) {
            return false;
        }
        const auto samples =
            static_cast<std::uint64_t>(capacityFrames)
            * static_cast<std::uint64_t>(channels);
        if (samples
            > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            return false;
        }
        samples_.assign(static_cast<std::size_t>(samples), 0.0F);
        timestamps_.assign(
            static_cast<std::size_t>(capacityFrames),
            0);
        capacityFrames_ = capacityFrames;
        channels_ = channels;
        sampleRate_ = sampleRate;
        clear();
        return true;
    }

    void clear() noexcept
    {
        readFrame_.store(0, std::memory_order_relaxed);
        writeFrame_.store(0, std::memory_order_relaxed);
    }

    int capacityFrames() const noexcept
    {
        return capacityFrames_;
    }

    std::uint64_t queuedFrames() const noexcept
    {
        const std::uint64_t written =
            writeFrame_.load(std::memory_order_acquire);
        const std::uint64_t read =
            readFrame_.load(std::memory_order_acquire);
        return written >= read ? written - read : 0;
    }

    bool canPush(int frames) const noexcept
    {
        if (frames <= 0 || frames > capacityFrames_) {
            return false;
        }
        return queuedFrames()
                + static_cast<std::uint64_t>(frames)
            <= static_cast<std::uint64_t>(capacityFrames_);
    }

    bool push(
        const float* source,
        int frames,
        std::int64_t timestampMilliseconds) noexcept
    {
        if (!source || !canPush(frames)) {
            return false;
        }
        const std::uint64_t written =
            writeFrame_.load(std::memory_order_relaxed);
        const int first = static_cast<int>(
            written % static_cast<std::uint64_t>(capacityFrames_));
        const int firstFrames =
            std::min(frames, capacityFrames_ - first);
        copySamples(
            samples_.data()
                + static_cast<std::size_t>(first)
                    * static_cast<std::size_t>(channels_),
            source,
            firstFrames);
        if (firstFrames < frames) {
            copySamples(
                samples_.data(),
                source
                    + static_cast<std::size_t>(firstFrames)
                        * static_cast<std::size_t>(channels_),
                frames - firstFrames);
        }
        const std::int64_t baseNanoseconds =
            timestampMilliseconds * 1'000'000LL;
        for (int offset = 0; offset < frames; ++offset) {
            const int index = (first + offset) % capacityFrames_;
            timestamps_[static_cast<std::size_t>(index)] =
                baseNanoseconds
                + static_cast<std::int64_t>(offset)
                    * 1'000'000'000LL
                    / static_cast<std::int64_t>(sampleRate_);
        }
        writeFrame_.store(
            written + static_cast<std::uint64_t>(frames),
            std::memory_order_release);
        return true;
    }

    PopResult pop(float* destination, int maximumFrames) noexcept
    {
        if (!destination || maximumFrames <= 0
            || capacityFrames_ <= 0) {
            return {};
        }
        const std::uint64_t read =
            readFrame_.load(std::memory_order_relaxed);
        const std::uint64_t written =
            writeFrame_.load(std::memory_order_acquire);
        const std::uint64_t available =
            written >= read ? written - read : 0;
        const int frames = static_cast<int>(
            std::min<std::uint64_t>(
                available,
                static_cast<std::uint64_t>(maximumFrames)));
        if (frames <= 0) {
            return {};
        }
        const int first = static_cast<int>(
            read % static_cast<std::uint64_t>(capacityFrames_));
        const int firstFrames =
            std::min(frames, capacityFrames_ - first);
        const std::int64_t timestamp =
            timestamps_[static_cast<std::size_t>(first)];
        copySamples(
            destination,
            samples_.data()
                + static_cast<std::size_t>(first)
                    * static_cast<std::size_t>(channels_),
            firstFrames);
        if (firstFrames < frames) {
            copySamples(
                destination
                    + static_cast<std::size_t>(firstFrames)
                        * static_cast<std::size_t>(channels_),
                samples_.data(),
                frames - firstFrames);
        }
        readFrame_.store(
            read + static_cast<std::uint64_t>(frames),
            std::memory_order_release);
        return { frames, timestamp };
    }

private:
    void copySamples(
        float* destination,
        const float* source,
        int frames) const noexcept
    {
        const auto count =
            static_cast<std::size_t>(frames)
            * static_cast<std::size_t>(channels_);
        std::memcpy(
            destination,
            source,
            count * sizeof(float));
    }

    std::vector<float> samples_;
    std::vector<std::int64_t> timestamps_;
    int capacityFrames_ = 0;
    int channels_ = 0;
    int sampleRate_ = 0;
    alignas(64) std::atomic<std::uint64_t> readFrame_ { 0 };
    alignas(64) std::atomic<std::uint64_t> writeFrame_ { 0 };
};

} // namespace qtav::detail
