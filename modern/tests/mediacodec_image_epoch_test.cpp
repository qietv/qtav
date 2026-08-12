// SPDX-License-Identifier: LGPL-2.1-or-later

#include "mediacodec_image_epoch.h"

#include <cstdlib>
#include <iostream>

namespace {

using Key = qtav::detail::MediaCodecImageFrameKey;
using QueueStatus = qtav::detail::MediaCodecProducerQueueStatus;
using Tracker = qtav::detail::MediaCodecImageEpochTracker;

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void testSeekAndRepeatedTimestamp(const char* path)
{
    Tracker tracker;
    const Key oldFrame { 0x1000U, 7, 4'000 };
    const Key newFrame { 0x2000U, 7, 4'000 };

    const auto oldQueue = tracker.begin(oldFrame);
    expect(
        oldQueue.status == QueueStatus::Added && oldQueue.epoch == 1,
        path);
    expect(tracker.invalidate() == 2, path);
    const auto newQueue = tracker.begin(newFrame);
    expect(
        newQueue.status == QueueStatus::Added && newQueue.epoch == 2,
        path);

    const auto lateOld = tracker.associateImage(4'000'000'000LL);
    expect(
        lateOld.matched && !lateOld.current
            && lateOld.key == oldFrame && lateOld.epoch == 1,
        path);
    const auto current = tracker.associateImage(4'000'000'000LL);
    expect(
        current.matched && current.current
            && current.key == newFrame && current.epoch == 2,
        path);
}

void testReplacementAndBoundedInvalidation(const char* path)
{
    Tracker tracker(2);
    const Key first { 0x3000U, 9, 1'000 };
    const Key second { 0x4000U, 9, 2'000 };
    const Key replacement { 0x5000U, 9, 3'000 };

    expect(tracker.begin(first).status == QueueStatus::Added, path);
    expect(tracker.begin(second).status == QueueStatus::Added, path);
    tracker.invalidate();
    expect(tracker.pendingAssociations() == 2, path);
    expect(
        tracker.begin(replacement).status
            == QueueStatus::CapacityReached,
        path);

    const auto lateFirst = tracker.associateImage(1'000'000'000LL);
    expect(lateFirst.matched && !lateFirst.current, path);
    expect(
        tracker.begin(replacement).status == QueueStatus::Added,
        path);
    const auto lateSecond = tracker.associateImage(2'000'000'000LL);
    expect(lateSecond.matched && !lateSecond.current, path);
    const auto replacementImage =
        tracker.associateImage(3'000'000'000LL);
    expect(
        replacementImage.matched && replacementImage.current
            && replacementImage.key == replacement,
        path);
}

void testOverlappingInvalidation(const char* path)
{
    Tracker tracker;
    const Key rendering { 0x6000U, 11, 8'000 };
    const auto queued = tracker.begin(rendering);
    expect(queued.status == QueueStatus::Added, path);
    tracker.invalidate();
    expect(
        tracker.associationEpoch(rendering) == queued.epoch
            && tracker.currentEpoch() != queued.epoch,
        path);
    const auto retry = tracker.begin(rendering);
    expect(
        retry.status == QueueStatus::Pending
            && retry.epoch == queued.epoch,
        path);
    const auto late = tracker.associateImage(8'000'000'000LL);
    expect(late.matched && !late.current, path);
    expect(
        tracker.begin(rendering).status == QueueStatus::Added,
        path);
}

void testUnprovenAndCancelledImages(const char* path)
{
    Tracker tracker;
    expect(!tracker.associateImage(0).matched, path);

    const Key cancelled { 0x7000U, 13, 0 };
    const auto queued = tracker.begin(cancelled);
    expect(queued.status == QueueStatus::Added, path);
    tracker.cancel(cancelled, queued.epoch);
    expect(!tracker.associateImage(0).matched, path);
}

void runPath(const char* path)
{
    testSeekAndRepeatedTimestamp(path);
    testReplacementAndBoundedInvalidation(path);
    testOverlappingInvalidation(path);
    testUnprovenAndCancelledImages(path);
}

} // namespace

int main()
{
    // Both Android interops instantiate this tracker independently. Running
    // the lifecycle matrix twice keeps the two integration expectations
    // explicit even though the epoch proof is shared and platform-free.
    runPath("Vulkan producer-epoch lifecycle failed");
    runPath("OpenGL ES producer-epoch lifecycle failed");
    return 0;
}
