// SPDX-License-Identifier: LGPL-2.1-or-later

#if defined(NDEBUG)
#  undef NDEBUG
#endif

#include <qtav/qtav.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct SessionResult {
    bool ended = false;
    bool invalid = false;
    std::int64_t lastVideoTimestamp = -1;
    std::vector<qtav::NetworkRecoveryStatus> recovery;
    std::vector<qtav::PacketBufferStatus> buffering;
    qtav::PlaybackStatistics statistics;
    bool externalTrackSwitched = false;
    bool externalTrackSwitchFailed = false;
    bool timedOut = false;
};

void configureNetwork(qtav::Player& player, std::uint32_t attempts,
                      std::int64_t delayMilliseconds)
{
    qtav::NetworkRecoveryPolicy recovery;
    recovery.maximumAttempts = attempts;
    recovery.initialRetryDelayMilliseconds = delayMilliseconds;
    recovery.maximumRetryDelayMilliseconds = delayMilliseconds * 4;

    qtav::PacketBufferPolicy buffering;
    buffering.initialBufferMilliseconds = 100;
    buffering.rebufferMilliseconds = 100;
    buffering.maximumBufferMilliseconds = 500;
    buffering.underflowDetectionMilliseconds = 30;

    player
        .setNetworkRecoveryPolicy(recovery)
        .setPacketBufferPolicy(buffering);
    // Exercise Player-level recovery deterministically after the protocol
    // returns an error instead of allowing HTTP's internal retry loop to hide
    // the injected disconnect.
    player.setProperty("avformat.reconnect", "0");
    player.setProperty("avformat.reconnect_on_network_error", "0");
    player.setProperty("avformat.reconnect_max_retries", "0");
    player.setProperty("avformat.rw_timeout", "500000");
}

SessionResult runSession(const std::string& url,
                         std::uint32_t attempts,
                         std::int64_t delayMilliseconds,
                         std::chrono::seconds timeout,
                         const std::string& externalAudioUrl = {})
{
    SessionResult result;
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> switchRequested { false };
    qtav::Player player;
    configureNetwork(player, attempts, delayMilliseconds);
    player
        .onNetworkRecoveryStatus(
            [&](const qtav::NetworkRecoveryStatus& status) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    result.recovery.push_back(status);
                }
                changed.notify_all();
            })
        .onStateChanged([&](qtav::State state) {
            if (state != qtav::State::Playing || externalAudioUrl.empty()
                || switchRequested.exchange(true)) {
                return;
            }
            const auto info = player.mediaInfo();
            const auto external = std::find_if(
                info.tracks.begin(),
                info.tracks.end(),
                [](const qtav::TrackInfo& track) {
                    return track.type == qtav::MediaType::Audio
                        && track.external;
                });
            if (external == info.tracks.end()) {
                std::lock_guard<std::mutex> lock(mutex);
                result.externalTrackSwitchFailed = true;
                changed.notify_all();
                return;
            }
            const bool switched = player.setActiveTrack(
                qtav::MediaType::Audio,
                external->index);
            std::lock_guard<std::mutex> lock(mutex);
            result.externalTrackSwitched = switched;
            result.externalTrackSwitchFailed = !switched;
            changed.notify_all();
        })
        .onPacketBufferStatus(
            [&](const qtav::PacketBufferStatus& status) {
                std::lock_guard<std::mutex> lock(mutex);
                result.buffering.push_back(status);
            })
        .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
            std::lock_guard<std::mutex> lock(mutex);
            result.lastVideoTimestamp = std::max(
                result.lastVideoTimestamp,
                frame.timestamp());
        })
        .onMediaStatus(
            [&](qtav::MediaStatus, qtav::MediaStatus status) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    result.ended = status == qtav::MediaStatus::EndOfMedia;
                    result.invalid = status == qtav::MediaStatus::Invalid;
                }
                changed.notify_all();
                return false;
            });

    player.setPlaybackRate(4.0F);
    if (!externalAudioUrl.empty()) {
        assert(player.setExternalMedia(
            qtav::MediaType::Audio,
            externalAudioUrl));
    }
    player.setMedia(url);
    player.setState(qtav::State::Playing);
    {
        std::unique_lock<std::mutex> lock(mutex);
        const bool completed = changed.wait_for(
            lock,
            timeout,
            [&] {
                return result.ended || result.invalid
                    || result.externalTrackSwitchFailed;
            });
        result.timedOut = !completed;
    }
    if (result.timedOut || result.externalTrackSwitchFailed) {
        player.setState(qtav::State::Stopped);
        player.waitFor(qtav::State::Stopped, 5'000);
    }
    result.statistics = player.playbackStatistics();
    assert(player.state() == qtav::State::Stopped);
    return result;
}

bool observed(const SessionResult& result,
              qtav::NetworkRecoveryState state,
              qtav::NetworkRecoveryOperation operation)
{
    return std::any_of(
        result.recovery.begin(),
        result.recovery.end(),
        [state, operation](const qtav::NetworkRecoveryStatus& status) {
            return status.state == state && status.operation == operation;
        });
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    const std::string baseUrl = argv[1];

    const auto openRecovered = runSession(
        baseUrl + "/open-retry.mp4",
        3,
        10,
        std::chrono::seconds(20));
    assert(openRecovered.ended);
    assert(!openRecovered.invalid);
    assert(openRecovered.lastVideoTimestamp >= 6'500);
    assert(observed(
        openRecovered,
        qtav::NetworkRecoveryState::Waiting,
        qtav::NetworkRecoveryOperation::Open));
    assert(observed(
        openRecovered,
        qtav::NetworkRecoveryState::Recovered,
        qtav::NetworkRecoveryOperation::Open));
    assert(openRecovered.statistics.networkRecoveryAttempts == 1);
    assert(openRecovered.statistics.successfulNetworkRecoveries == 1);
    assert(openRecovered.statistics.failedNetworkRecoveries == 0);

    const auto readRecovered = runSession(
        baseUrl + "/read-retry.mp4",
        3,
        10,
        std::chrono::seconds(25));
    assert(readRecovered.ended);
    assert(!readRecovered.invalid);
    assert(readRecovered.lastVideoTimestamp >= 6'500);
    assert(observed(
        readRecovered,
        qtav::NetworkRecoveryState::Waiting,
        qtav::NetworkRecoveryOperation::Read));
    assert(observed(
        readRecovered,
        qtav::NetworkRecoveryState::Recovered,
        qtav::NetworkRecoveryOperation::Read));
    assert(std::any_of(
        readRecovered.buffering.begin(),
        readRecovered.buffering.end(),
        [](const qtav::PacketBufferStatus& status) {
            return status.reason
                == qtav::PacketBufferingReason::NetworkRecovery;
        }));
    assert(readRecovered.statistics.networkRecoveryAttempts >= 1);
    assert(readRecovered.statistics.successfulNetworkRecoveries == 1);
    assert(readRecovered.statistics.failedNetworkRecoveries == 0);

    const auto externalRecovered = runSession(
        baseUrl + "/stable.mp4",
        3,
        10,
        std::chrono::seconds(25),
        baseUrl + "/external-read-retry.m4a");
    assert(externalRecovered.ended);
    assert(!externalRecovered.invalid);
    assert(externalRecovered.externalTrackSwitched);
    assert(externalRecovered.lastVideoTimestamp >= 6'500);
    assert(std::any_of(
        externalRecovered.recovery.begin(),
        externalRecovered.recovery.end(),
        [](const qtav::NetworkRecoveryStatus& status) {
            return status.state == qtav::NetworkRecoveryState::Recovered
                && status.operation
                    == qtav::NetworkRecoveryOperation::Read
                && status.input
                    == qtav::NetworkRecoveryInput::ExternalAudio;
        }));
    assert(externalRecovered.statistics.networkRecoveryAttempts >= 1);
    assert(externalRecovered.statistics.successfulNetworkRecoveries == 1);
    assert(externalRecovered.statistics.failedNetworkRecoveries == 0);

    const auto readExhausted = runSession(
        baseUrl + "/read-exhausted.mp4",
        2,
        5,
        std::chrono::seconds(15));
    assert(!readExhausted.ended);
    assert(readExhausted.invalid);
    assert(observed(
        readExhausted,
        qtav::NetworkRecoveryState::Failed,
        qtav::NetworkRecoveryOperation::Read));
    assert(readExhausted.statistics.networkRecoveryAttempts == 2);
    assert(
        readExhausted.statistics.successfulNetworkRecoveries <= 2);
    assert(readExhausted.statistics.failedNetworkRecoveries == 1);
    assert(std::any_of(
        readExhausted.recovery.begin(),
        readExhausted.recovery.end(),
        [](const qtav::NetworkRecoveryStatus& status) {
            return status.state == qtav::NetworkRecoveryState::Waiting
                && status.operation
                    == qtav::NetworkRecoveryOperation::Read
                && status.attempt == 2
                && status.retryDelayMilliseconds == 10;
        }));

    const auto exhausted = runSession(
        baseUrl + "/always-503.mp4",
        2,
        5,
        std::chrono::seconds(10));
    assert(!exhausted.ended);
    assert(exhausted.invalid);
    assert(observed(
        exhausted,
        qtav::NetworkRecoveryState::Failed,
        qtav::NetworkRecoveryOperation::Open));
    assert(exhausted.statistics.networkRecoveryAttempts == 2);
    assert(exhausted.statistics.successfulNetworkRecoveries == 0);
    assert(exhausted.statistics.failedNetworkRecoveries == 1);

    std::mutex cancelMutex;
    std::condition_variable cancelChanged;
    bool loading = false;
    bool stopped = false;
    bool waiting = false;
    qtav::Player cancelPlayer;
    configureNetwork(cancelPlayer, 3, 5'000);
    cancelPlayer
        .onNetworkRecoveryStatus(
            [&](const qtav::NetworkRecoveryStatus& status) {
                if (status.state == qtav::NetworkRecoveryState::Waiting) {
                    {
                        std::lock_guard<std::mutex> lock(cancelMutex);
                        waiting = true;
                    }
                    cancelPlayer.setState(qtav::State::Stopped);
                }
            })
        .onMediaStatus(
            [&](qtav::MediaStatus, qtav::MediaStatus status) {
                {
                    std::lock_guard<std::mutex> lock(cancelMutex);
                    loading = loading || status == qtav::MediaStatus::Loading;
                    stopped = loading && status == qtav::MediaStatus::NoMedia;
                }
                cancelChanged.notify_all();
                return false;
            });
    const auto cancelStart = std::chrono::steady_clock::now();
    cancelPlayer.setMedia(baseUrl + "/always-503.mp4");
    cancelPlayer.setState(qtav::State::Playing);
    {
        std::unique_lock<std::mutex> lock(cancelMutex);
        assert(cancelChanged.wait_for(
            lock,
            std::chrono::seconds(2),
            [&] { return stopped; }));
    }
    const auto cancelElapsed = std::chrono::steady_clock::now() - cancelStart;
    assert(waiting);
    assert(cancelElapsed < std::chrono::seconds(2));
    assert(
        cancelPlayer.networkRecoveryStatus().state
        == qtav::NetworkRecoveryState::Idle);
    const auto cancelStatistics = cancelPlayer.playbackStatistics();
    assert(cancelStatistics.networkRecoveryAttempts == 0);
    assert(cancelStatistics.failedNetworkRecoveries == 0);
    return 0;
}
