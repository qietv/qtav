// SPDX-License-Identifier: LGPL-2.1-or-later

#if defined(NDEBUG)
#  undef NDEBUG
#endif

#include <qtav/qtav.h>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

int main(int argc, char** argv)
{
    assert(argc == 2);

    qtav::Player player;
    qtav::PacketBufferPolicy buffering;
    buffering.initialBufferMilliseconds = 100;
    buffering.rebufferMilliseconds = 100;
    qtav::LivePlaybackPolicy live;
    live.enabled = true;
    live.maximumQueuedVideoFrames = 2;
    live.lateVideoFrameThresholdMilliseconds = 10;

    std::mutex mutex;
    std::condition_variable finished;
    bool ended = false;
    bool failed = false;
    std::vector<std::int64_t> timestamps;
    player
        .setPacketBufferPolicy(buffering)
        .setLivePlaybackPolicy(live)
        .onMediaStatus(
            [&](qtav::MediaStatus, qtav::MediaStatus current) {
                if (current == qtav::MediaStatus::EndOfMedia
                    || current == qtav::MediaStatus::Invalid) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        ended = current == qtav::MediaStatus::EndOfMedia;
                        failed = current == qtav::MediaStatus::Invalid;
                    }
                    finished.notify_all();
                }
                return false;
            })
        .onVideoFrame(
            [&](const qtav::VideoFrame& frame, int) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    timestamps.push_back(frame.timestamp());
                }
                // Deliberately slower than the 60 fps fixture. Decode must
                // retain a bounded newest-frame window instead of accumulating
                // presentation latency behind this consumer.
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            });

    player.setMedia(argv[1]);
    player.setState(qtav::State::Playing);
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(finished.wait_for(
            lock,
            std::chrono::seconds(15),
            [&] { return ended || failed; }));
    }

    const auto statistics = player.playbackStatistics();
    assert(!failed);
    assert(ended);
    assert(!timestamps.empty());
    for (std::size_t index = 1; index < timestamps.size(); ++index) {
        assert(timestamps[index] > timestamps[index - 1]);
    }
    assert(timestamps.back() >= 3'500);
    assert(statistics.decodedVideoFrames > timestamps.size());
    assert(statistics.videoQueueOverflowDrops > 0);
    assert(statistics.lowLatencyVideoQueueDrops > 0);
    assert(
        statistics.lowLatencyVideoQueueDrops
        <= statistics.videoQueueOverflowDrops);
    assert(statistics.maximumQueuedVideoFrames <= 2);
    assert(
        statistics.deliveredVideoFrames
        == static_cast<std::uint64_t>(timestamps.size()));
    assert(player.state() == qtav::State::Stopped);
    return 0;
}
