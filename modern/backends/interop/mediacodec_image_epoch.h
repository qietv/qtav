// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>

namespace qtav::detail {

struct MediaCodecImageFrameKey {
    std::uintptr_t buffer = 0;
    std::uint32_t surfaceGeneration = 0;
    std::int64_t timestampMilliseconds = 0;

    bool operator==(
        const MediaCodecImageFrameKey& other) const noexcept
    {
        return buffer == other.buffer
            && surfaceGeneration == other.surfaceGeneration
            && timestampMilliseconds == other.timestampMilliseconds;
    }
};

enum class MediaCodecProducerQueueStatus {
    Added,
    Pending,
    Retired,
    CapacityReached,
};

struct MediaCodecProducerQueueResult {
    MediaCodecProducerQueueStatus status =
        MediaCodecProducerQueueStatus::CapacityReached;
    std::uint64_t epoch = 0;
};

struct MediaCodecProducerImageAssociation {
    MediaCodecImageFrameKey key;
    std::uint64_t epoch = 0;
    bool matched = false;
    bool current = false;
};

// Tracks the producer epoch which cannot be read back from an AImage. Every
// released codec output remains represented until one acquired image consumes
// its association. Invalidation marks existing associations instead of
// erasing them, so a late image with a repeated post-seek timestamp is retired
// against the old epoch before a new-epoch image can be admitted.
//
// The caller serializes access. The tracker never evicts an unconsumed
// association: once its fixed capacity is reached, new codec outputs must stay
// unreleased until an image callback drains an older association.
class MediaCodecImageEpochTracker {
public:
    explicit MediaCodecImageEpochTracker(
        std::size_t maximumAssociations = 64,
        std::int64_t timestampToleranceNanoseconds = 2'000'000)
        : maximumAssociations_(std::max<std::size_t>(
              1,
              maximumAssociations))
        , timestampToleranceNanoseconds_(std::max<std::int64_t>(
              0,
              timestampToleranceNanoseconds))
    {
    }

    MediaCodecProducerQueueResult begin(
        const MediaCodecImageFrameKey& key)
    {
        const auto pending = std::find_if(
            associations_.begin(),
            associations_.end(),
            [&key](const Association& association) {
                return association.key == key;
            });
        if (pending != associations_.end()) {
            return {
                MediaCodecProducerQueueStatus::Pending,
                pending->epoch,
            };
        }
        const auto retired = std::find_if(
            retired_.begin(),
            retired_.end(),
            [this, &key](const Retired& entry) {
                return entry.epoch == currentEpoch_
                    && entry.key == key;
            });
        if (retired != retired_.end()) {
            return {
                MediaCodecProducerQueueStatus::Retired,
                currentEpoch_,
            };
        }
        if (associations_.size() >= maximumAssociations_) {
            return {
                MediaCodecProducerQueueStatus::CapacityReached,
                currentEpoch_,
            };
        }
        associations_.push_back({ key, currentEpoch_, false });
        return {
            MediaCodecProducerQueueStatus::Added,
            currentEpoch_,
        };
    }

    void cancel(
        const MediaCodecImageFrameKey& key,
        std::uint64_t epoch) noexcept
    {
        const auto found = std::find_if(
            associations_.begin(),
            associations_.end(),
            [&key, epoch](const Association& association) {
                return association.epoch == epoch
                    && association.key == key;
            });
        if (found != associations_.end()) {
            associations_.erase(found);
        }
    }

    MediaCodecProducerImageAssociation associateImage(
        std::int64_t timestampNanoseconds)
    {
        auto closest = associations_.end();
        std::int64_t closestDistance =
            std::numeric_limits<std::int64_t>::max();
        for (auto iterator = associations_.begin();
             iterator != associations_.end();
             ++iterator) {
            const std::int64_t expected =
                iterator->key.timestampMilliseconds * 1'000'000LL;
            const std::int64_t distance = expected > timestampNanoseconds
                ? expected - timestampNanoseconds
                : timestampNanoseconds - expected;
            // Keep the earliest released association when repeated timestamps
            // are equally close. AImageReader acquireNext preserves producer
            // order, and the old epoch must consume the first late image.
            if (distance <= timestampToleranceNanoseconds_
                && distance < closestDistance) {
                closest = iterator;
                closestDistance = distance;
            }
        }
        if (closest == associations_.end()) {
            return {};
        }

        MediaCodecProducerImageAssociation result;
        result.key = closest->key;
        result.epoch = closest->epoch;
        result.matched = true;
        result.current = !closest->invalidated
            && closest->epoch == currentEpoch_;
        retire(closest->key, closest->epoch);
        associations_.erase(closest);
        return result;
    }

    void retireCurrent(const MediaCodecImageFrameKey& key)
    {
        retire(key, currentEpoch_);
    }

    std::uint64_t invalidate() noexcept
    {
        ++currentEpoch_;
        if (currentEpoch_ == 0) {
            currentEpoch_ = 1;
        }
        for (Association& association : associations_) {
            association.invalidated = true;
        }
        return currentEpoch_;
    }

    std::uint64_t currentEpoch() const noexcept
    {
        return currentEpoch_;
    }

    std::uint64_t associationEpoch(
        const MediaCodecImageFrameKey& key) const noexcept
    {
        const auto found = std::find_if(
            associations_.begin(),
            associations_.end(),
            [&key](const Association& association) {
                return association.key == key;
            });
        return found != associations_.end() ? found->epoch : 0;
    }

    bool hasAssociation(
        const MediaCodecImageFrameKey& key,
        std::uint64_t epoch) const noexcept
    {
        return std::any_of(
            associations_.begin(),
            associations_.end(),
            [&key, epoch](const Association& association) {
                return association.epoch == epoch
                    && association.key == key;
            });
    }

    std::size_t pendingAssociations() const noexcept
    {
        return associations_.size();
    }

private:
    struct Association {
        MediaCodecImageFrameKey key;
        std::uint64_t epoch = 0;
        bool invalidated = false;
    };

    struct Retired {
        MediaCodecImageFrameKey key;
        std::uint64_t epoch = 0;
    };

    void retire(
        const MediaCodecImageFrameKey& key,
        std::uint64_t epoch)
    {
        retired_.push_back({ key, epoch });
        while (retired_.size() > maximumRetired_) {
            retired_.pop_front();
        }
    }

    std::size_t maximumAssociations_ = 64;
    std::int64_t timestampToleranceNanoseconds_ = 2'000'000;
    std::uint64_t currentEpoch_ = 1;
    std::deque<Association> associations_;
    std::deque<Retired> retired_;
    static constexpr std::size_t maximumRetired_ = 64;
};

} // namespace qtav::detail
