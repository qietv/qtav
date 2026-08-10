// SPDX-License-Identifier: LGPL-2.1-or-later

#if defined(NDEBUG)
#  undef NDEBUG
#endif

#include <qtav/player.h>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::int64_t waitForOperation(
    qtav::Player& player,
    std::mutex& mutex,
    std::condition_variable& changed,
    const std::function<bool(qtav::Player::SeekCallback)>& start)
{
    bool completed = false;
    std::int64_t result = -2;
    assert(start([&](std::int64_t position) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            result = position;
            completed = true;
        }
        changed.notify_all();
    }));
    std::unique_lock<std::mutex> lock(mutex);
    assert(changed.wait_for(lock, 5s, [&] { return completed; }));
    return result;
}

void testPausedAccurateSeekAndStepping(const char* media)
{
    qtav::Player unloaded;
    assert(!unloaded.stepForward());
    assert(!unloaded.stepBackward());

    qtav::Player player;
    std::mutex mutex;
    std::condition_variable changed;
    bool prepared = false;
    bool failed = false;
    std::vector<std::int64_t> frames;

    player
        .onMediaStatus([&](qtav::MediaStatus, qtav::MediaStatus status) {
            if (status == qtav::MediaStatus::Invalid) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    failed = true;
                }
                changed.notify_all();
            }
            return false;
        })
        .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                frames.push_back(frame.timestamp());
            }
            changed.notify_all();
        });

    player.setMedia(media);
    player.prepare(550, [&](std::int64_t position, bool*) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            prepared = position == 550;
        }
        changed.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            5s,
            [&] { return prepared || failed; }));
    }
    assert(!failed);
    assert(player.state() == qtav::State::Paused);

    const auto scannedBackward = waitForOperation(
        player,
        mutex,
        changed,
        [&](qtav::Player::SeekCallback callback) {
            return player.stepBackward(std::move(callback));
        });
    assert(scannedBackward >= 0);
    assert(scannedBackward < 550);

    const auto accurate = waitForOperation(
        player,
        mutex,
        changed,
        [&](qtav::Player::SeekCallback callback) {
            return player.seek(
                455,
                qtav::SeekFlag::Accurate,
                std::move(callback));
        });
    assert(accurate >= 455);
    assert(accurate < 650);
    assert(player.state() == qtav::State::Paused);
    assert(player.position() == accurate);
    {
        std::lock_guard<std::mutex> lock(mutex);
        assert(frames.size() == 2);
        assert(frames.back() == accurate);
    }

    const auto forward = waitForOperation(
        player,
        mutex,
        changed,
        [&](qtav::Player::SeekCallback callback) {
            return player.stepForward(std::move(callback));
        });
    assert(forward > accurate);
    assert(player.state() == qtav::State::Paused);

    const auto backward = waitForOperation(
        player,
        mutex,
        changed,
        [&](qtav::Player::SeekCallback callback) {
            return player.stepBackward(std::move(callback));
        });
    assert(backward == accurate);

    const auto backwardAgain = waitForOperation(
        player,
        mutex,
        changed,
        [&](qtav::Player::SeekCallback callback) {
            return player.stepBackward(std::move(callback));
        });
    assert(backwardAgain >= 0);
    assert(backwardAgain < backward);

    const auto forwardAgain = waitForOperation(
        player,
        mutex,
        changed,
        [&](qtav::Player::SeekCallback callback) {
            return player.stepForward(std::move(callback));
        });
    assert(forwardAgain == backward);
    assert(player.state() == qtav::State::Paused);
    {
        std::lock_guard<std::mutex> lock(mutex);
        assert(frames.size() == 6);
        assert(frames[0] == scannedBackward);
        assert(frames[1] == accurate);
        assert(frames[2] == forward);
        assert(frames[3] == backward);
        assert(frames[4] == backwardAgain);
        assert(frames[5] == forwardAgain);
    }

    player.setState(qtav::State::Stopped);
    assert(player.waitFor(qtav::State::Stopped, 5'000));
}

void testAccuratePrepare(const char* media)
{
    qtav::Player player;
    std::mutex mutex;
    std::condition_variable changed;
    bool completed = false;
    std::int64_t preparedPosition = -1;
    std::int64_t framePosition = -1;

    player.onVideoFrame([&](const qtav::VideoFrame& frame, int) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            framePosition = frame.timestamp();
        }
        changed.notify_all();
    });
    player.setMedia(media);
    player.prepare(
        455,
        [&](std::int64_t position, bool* boost) {
            assert(boost && *boost);
            {
                std::lock_guard<std::mutex> lock(mutex);
                preparedPosition = position;
                completed = true;
            }
            changed.notify_all();
        },
        qtav::SeekFlag::Accurate);

    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, 5s, [&] { return completed; }));
    }
    assert(preparedPosition >= 455);
    assert(preparedPosition < 650);
    assert(framePosition == preparedPosition);
    assert(player.position() == preparedPosition);
    assert(player.state() == qtav::State::Paused);
}

void testPlayingAccurateSeek(const char* media)
{
    qtav::Player player;
    std::mutex mutex;
    std::condition_variable changed;
    bool failed = false;
    bool seekIssued = false;
    bool seekCompleted = false;
    std::int64_t actual = -1;
    std::vector<std::int64_t> postRequestFrames;
    std::vector<std::int64_t> postRequestAudioFrames;

    player
        .onMediaStatus([&](qtav::MediaStatus, qtav::MediaStatus status) {
            if (status == qtav::MediaStatus::Invalid) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    failed = true;
                }
                changed.notify_all();
            }
            return false;
        })
        .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
            bool request = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!seekIssued) {
                    seekIssued = true;
                    request = true;
                } else {
                    postRequestFrames.push_back(frame.timestamp());
                }
            }
            if (request) {
                const bool accepted = player.seek(
                    1'455,
                    qtav::SeekFlag::Accurate,
                    [&](std::int64_t position) {
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            actual = position;
                            seekCompleted = true;
                        }
                        changed.notify_all();
                    });
                assert(accepted);
            }
        })
        .onAudioFrame([&](const qtav::AudioFrame& frame, int) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (seekIssued) {
                    postRequestAudioFrames.push_back(frame.timestamp());
                }
            }
            changed.notify_all();
        });

    player.setMedia(media);
    // Keep the playback worker from winning a short-fixture natural-end race
    // before the first presented-frame callback can issue the seek under
    // heavy validation load.
    player.setLoop(-1);
    player.setState(qtav::State::Playing);
    bool completionObserved = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        completionObserved = changed.wait_for(
            lock,
            5s,
            [&] { return seekCompleted || failed; });
    }
    if (!completionObserved) {
        const auto buffering = player.packetBufferStatus();
        std::cerr << "accurate seek timed out: state="
                  << static_cast<int>(player.state())
                  << " status=" << static_cast<int>(player.mediaStatus())
                  << " position=" << player.position()
                  << " issued=" << seekIssued
                  << " buffering=" << buffering.buffering
                  << " buffered=" << buffering.bufferedMilliseconds
                  << " target=" << buffering.targetMilliseconds << '\n';
    }
    assert(completionObserved);
    assert(!failed);
    assert(actual >= 1'455);
    assert(actual < 1'650);
    assert(player.state() == qtav::State::Playing);
    {
        std::lock_guard<std::mutex> lock(mutex);
        assert(!postRequestFrames.empty());
        assert(postRequestFrames.front() == actual);
        for (const auto timestamp : postRequestFrames) {
            assert(timestamp >= 1'455);
        }
    }
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            2s,
            [&] { return !postRequestAudioFrames.empty() || failed; }));
        assert(!failed);
        for (const auto timestamp : postRequestAudioFrames) {
            assert(timestamp >= 1'455);
        }
    }

    player.setState(qtav::State::Stopped);
    assert(player.waitFor(qtav::State::Stopped, 5'000));
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    testAccuratePrepare(argv[1]);
    testPausedAccurateSeekAndStepping(argv[1]);
    testPlayingAccurateSeek(argv[1]);
    return 0;
}
