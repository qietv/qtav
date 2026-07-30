// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/player.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace {

void testPrepareSeekAndPause(const char* media)
{
    qtav::Player player;
    std::mutex mutex;
    std::condition_variable changed;
    bool prepared = false;
    bool preparedAgain = false;
    bool seeked = false;
    bool ended = false;
    bool failed = false;
    std::atomic<int> framesAfterSeek { 0 };

    player
        .onMediaStatus([&](qtav::MediaStatus, qtav::MediaStatus status) {
            if (status == qtav::MediaStatus::EndOfMedia
                || status == qtav::MediaStatus::Invalid) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    ended = status == qtav::MediaStatus::EndOfMedia;
                    failed = status == qtav::MediaStatus::Invalid;
                }
                changed.notify_all();
            }
            return false;
        })
        .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
            if (frame.timestamp() >= 600) {
                ++framesAfterSeek;
            }
        });

    player.setPlaybackRate(8.0F);
    player.setMedia(media);
    player.prepare(250, [&](std::int64_t position, bool* boost) {
        assert(position == 250);
        assert(boost && *boost);
        {
            std::lock_guard<std::mutex> lock(mutex);
            prepared = true;
        }
        changed.notify_all();
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return prepared || failed; }));
    }
    assert(!failed);
    assert(player.state() == qtav::State::Paused);
    assert(player.mediaStatus() == qtav::MediaStatus::Loaded);

    player.prepare(400, [&](std::int64_t position, bool* boost) {
        assert(position == 400);
        assert(boost && *boost);
        {
            std::lock_guard<std::mutex> lock(mutex);
            preparedAgain = true;
        }
        changed.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return preparedAgain || failed; }));
    }

    assert(player.seek(600, qtav::SeekFlag::FromStart, [&](std::int64_t position) {
        assert(position == 600);
        {
            std::lock_guard<std::mutex> lock(mutex);
            seeked = true;
        }
        changed.notify_all();
    }));
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return seeked || failed; }));
    }

    player.setState(qtav::State::Playing);
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return ended || failed; }));
    }
    assert(!failed);
    assert(ended);
    assert(framesAfterSeek.load() > 0);
}

void testRangeLoop(const char* media)
{
    qtav::Player player;
    std::mutex mutex;
    std::condition_variable changed;
    bool ended = false;
    bool failed = false;
    std::atomic<int> frames { 0 };

    player
        .onMediaStatus([&](qtav::MediaStatus, qtav::MediaStatus status) {
            if (status == qtav::MediaStatus::EndOfMedia
                || status == qtav::MediaStatus::Invalid) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    ended = status == qtav::MediaStatus::EndOfMedia;
                    failed = status == qtav::MediaStatus::Invalid;
                }
                changed.notify_all();
            }
            return false;
        })
        .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
            assert(frame.timestamp() >= 100);
            assert(frame.timestamp() < 350);
            ++frames;
        });

    player.setPlaybackRate(8.0F);
    player.setRange(100, 350);
    player.setLoop(1);
    player.setMedia(media);
    player.setState(qtav::State::Playing);

    std::unique_lock<std::mutex> lock(mutex);
    assert(changed.wait_for(
        lock,
        std::chrono::seconds(5),
        [&] { return ended || failed; }));
    lock.unlock();

    assert(!failed);
    assert(ended);
    // Four frames per pass at 12 fps in [100ms, 350ms), played twice.
    assert(frames.load() >= 6);
}

void testPlayingSeekBuffering(const char* media)
{
    qtav::Player player;
    std::mutex mutex;
    std::condition_variable changed;
    bool buffering = false;
    bool loadedAfterBuffering = false;
    bool seeked = false;
    bool ended = false;
    bool failed = false;
    std::atomic<bool> seekRequested { false };

    player
        .onMediaStatus(
            [&](qtav::MediaStatus, qtav::MediaStatus status) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (status == qtav::MediaStatus::Buffering) {
                        buffering = true;
                    } else if (status == qtav::MediaStatus::Loaded
                               && buffering) {
                        loadedAfterBuffering = true;
                    } else if (
                        status == qtav::MediaStatus::EndOfMedia) {
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
                600,
                qtav::SeekFlag::FromStart,
                [&](std::int64_t position) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        seeked = position == 600;
                        failed = failed || position < 0;
                    }
                    changed.notify_all();
                });
            if (!accepted) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    failed = true;
                }
                changed.notify_all();
            }
        });

    player.setPlaybackRate(8.0F);
    player.setMedia(media);
    player.setState(qtav::State::Playing);

    std::unique_lock<std::mutex> lock(mutex);
    assert(changed.wait_for(
        lock,
        std::chrono::seconds(5),
        [&] { return ended || failed; }));
    lock.unlock();

    assert(!failed);
    assert(ended);
    assert(seekRequested.load());
    assert(seeked);
    assert(buffering);
    assert(loadedAfterBuffering);
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    testPrepareSeekAndPause(argv[1]);
    testRangeLoop(argv[1]);
    testPlayingSeekBuffering(argv[1]);
    return 0;
}
