// SPDX-License-Identifier: LGPL-2.1-or-later

#if defined(NDEBUG)
#  undef NDEBUG
#endif

#include <qtav/player.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    assert(argc == 2);

    qtav::Player player;
    qtav::PacketBufferPolicy policy;
    policy.initialBufferMilliseconds = 300;
    policy.rebufferMilliseconds = 350;
    policy.maximumBufferMilliseconds = 1'000;
    policy.maximumBufferBytes = 2U * 1024U * 1024U;
    policy.underflowDetectionMilliseconds = 80;
    player.setPacketBufferPolicy(policy);

    std::mutex mutex;
    std::condition_variable changed;
    std::vector<qtav::PacketBufferStatus> statuses;
    bool ended = false;
    bool failed = false;
    bool seekCompleted = false;
    std::atomic<bool> seekRequested { false };

    player
        .onPacketBufferStatus(
            [&](const qtav::PacketBufferStatus& status) {
                assert(status.progress >= 0.0);
                assert(status.progress <= 1.0);
                assert(status.bufferedMilliseconds >= 0);
                assert(status.bufferedBytes <= policy.maximumBufferBytes);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    statuses.push_back(status);
                }
                changed.notify_all();
            })
        .onMediaStatus(
            [&](qtav::MediaStatus, qtav::MediaStatus status) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (status == qtav::MediaStatus::EndOfMedia) {
                        ended = true;
                    } else if (status == qtav::MediaStatus::Invalid) {
                        failed = true;
                    }
                }
                changed.notify_all();
                return false;
            })
        .onVideoFrame([&](const qtav::VideoFrame&, int) {
            if (seekRequested.exchange(true)) {
                return;
            }
            const bool accepted = player.seek(
                500,
                qtav::SeekFlag::FromStart,
                [&](std::int64_t position) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        seekCompleted = position == 500;
                        failed = failed || position < 0;
                    }
                    changed.notify_all();
                });
            if (!accepted) {
                std::lock_guard<std::mutex> lock(mutex);
                failed = true;
                changed.notify_all();
            }
        });

    player.setPlaybackRate(4.0F);
    player.setMedia(argv[1]);
    player.setState(qtav::State::Playing);

    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            std::chrono::seconds(10),
            [&] { return ended || failed; }));
    }

    assert(!failed);
    assert(ended);
    assert(seekRequested.load());
    assert(seekCompleted);

    bool initialStarted = false;
    bool initialCompleted = false;
    bool seekStarted = false;
    bool seekBufferCompleted = false;
    bool observedBytes = false;
    for (const auto& status : statuses) {
        observedBytes = observedBytes || status.bufferedBytes > 0;
        if (status.reason
            == qtav::PacketBufferingReason::InitialPlayback) {
            initialStarted = initialStarted || status.buffering;
            initialCompleted = initialCompleted || !status.buffering;
        } else if (status.reason
                   == qtav::PacketBufferingReason::Seek) {
            seekStarted = seekStarted || status.buffering;
            seekBufferCompleted = seekBufferCompleted || !status.buffering;
        }
    }
    assert(initialStarted);
    assert(initialCompleted);
    assert(seekStarted);
    assert(seekBufferCompleted);
    assert(observedBytes);

    const auto finalStatus = player.packetBufferStatus();
    assert(!finalStatus.buffering);
    assert(finalStatus.progress == 1.0);

    qtav::Player diskPlayer;
    qtav::PacketBufferPolicy diskPolicy;
    diskPolicy.initialBufferMilliseconds = 1'500;
    diskPolicy.rebufferMilliseconds = 1'500;
    diskPolicy.maximumBufferMilliseconds = 100;
    diskPolicy.maximumBufferBytes = 1'024;
    diskPolicy.underflowDetectionMilliseconds = 80;
    diskPolicy.diskCache.enabled = true;
    diskPolicy.diskCache.maximumCacheMilliseconds = 2'000;
    diskPolicy.diskCache.maximumCacheBytes = 8U * 1024U * 1024U;
    diskPlayer.setPacketBufferPolicy(diskPolicy);

    std::mutex diskMutex;
    std::condition_variable diskChanged;
    bool diskObserved = false;
    bool diskFailed = false;
    std::string observedDiskPath;
    diskPlayer
        .onPacketBufferStatus(
            [&](const qtav::PacketBufferStatus& status) {
                assert(
                    status.bufferedBytes
                    == status.memoryBufferedBytes
                        + status.diskBufferedBytes);
                if (status.diskBufferedBytes > 0) {
                    std::lock_guard<std::mutex> lock(diskMutex);
                    diskObserved = true;
                    observedDiskPath = status.diskCachePath;
                    diskChanged.notify_all();
                }
            })
        .onMediaStatus(
            [&](qtav::MediaStatus, qtav::MediaStatus status) {
                if (status == qtav::MediaStatus::Invalid) {
                    std::lock_guard<std::mutex> lock(diskMutex);
                    diskFailed = true;
                    diskChanged.notify_all();
                }
                return false;
            });
    diskPlayer.setMedia(argv[1]);
    diskPlayer.setState(qtav::State::Playing);
    {
        std::unique_lock<std::mutex> lock(diskMutex);
        assert(diskChanged.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return diskObserved || diskFailed; }));
    }
    assert(!diskFailed);
    assert(diskObserved);
    assert(!observedDiskPath.empty());
    assert(std::filesystem::exists(observedDiskPath));

    auto memoryOnlyPolicy = diskPlayer.packetBufferPolicy();
    memoryOnlyPolicy.diskCache.enabled = false;
    diskPlayer.setPacketBufferPolicy(memoryOnlyPolicy);
    assert(diskPlayer.clearPacketDiskCache());
    assert(diskPlayer.packetDiskCachePath().empty());
    assert(!std::filesystem::exists(observedDiskPath));
    diskPlayer.setState(qtav::State::Stopped);
    assert(diskPlayer.waitFor(qtav::State::Stopped, 5'000));
    return 0;
}
