// SPDX-License-Identifier: LGPL-2.1-or-later

#if defined(NDEBUG)
#  undef NDEBUG
#endif

#include <qtav/player.h>

#include "simulated_audio_sink.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iterator>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::vector<qtav::TrackInfo> tracksOfType(
    const qtav::MediaInfo& info,
    qtav::MediaType type)
{
    std::vector<qtav::TrackInfo> result;
    std::copy_if(
        info.tracks.begin(),
        info.tracks.end(),
        std::back_inserter(result),
        [type](const qtav::TrackInfo& track) {
            return track.type == type;
        });
    return result;
}

const qtav::TrackInfo& alternateTrack(
    const std::vector<qtav::TrackInfo>& tracks,
    int active)
{
    const auto found = std::find_if(
        tracks.begin(),
        tracks.end(),
        [active](const qtav::TrackInfo& track) {
            return track.index != active;
        });
    assert(found != tracks.end());
    return *found;
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2 || argc == 3);
    const bool suppliedAudioSubtitleFixture = argc == 3
        && std::string(argv[2]) == "--audio-subtitle";

    qtav::Player player;
    qtav::test::SimulatedAudioSinkConfig sinkConfig;
    sinkConfig.hasDeviceClock = false;
    auto audioSink =
        std::make_shared<qtav::test::SimulatedAudioSink>(sinkConfig);
    player.setAudioSink(audioSink);
    assert(!player.setActiveTrack(qtav::MediaType::Audio, 0));
    assert(!player.setActiveTrack(qtav::MediaType::Subtitle, -1));

    std::mutex mutex;
    std::condition_variable changed;
    bool prepared = false;
    bool failed = false;
    bool switchBuffering = false;
    bool switchLoaded = false;
    int trackChanges = 0;
    std::atomic<bool> playingSwitchRequested { false };
    std::atomic<int> expectedAudioTrack { -1 };
    std::atomic<int> expectedVideoTrack { -1 };
    std::atomic<int> expectedSubtitleTrack { -1 };
    std::atomic<int> expectedAudioFrames { 0 };
    std::atomic<int> expectedVideoFrames { 0 };
    std::atomic<int> expectedSubtitleFrames { 0 };
    std::atomic<int> unexpectedAudioFrames { 0 };
    std::atomic<int> unexpectedVideoFrames { 0 };
    std::atomic<int> unexpectedSubtitleFrames { 0 };
    std::atomic<bool> countFrames { false };
    std::string lastSubtitleText;

    player
        .onEvent([&](const qtav::MediaEvent& event) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (event.category == "track.changed") {
                    ++trackChanges;
                } else if (event.category == "track.error"
                           || event.category == "media.error"
                           || event.category == "decode.error") {
                    failed = true;
                }
            }
            changed.notify_all();
            return false;
        })
        .onMediaStatus(
            [&](qtav::MediaStatus, qtav::MediaStatus status) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (status == qtav::MediaStatus::Invalid) {
                        failed = true;
                    } else if (playingSwitchRequested.load()
                               && status == qtav::MediaStatus::Buffering) {
                        switchBuffering = true;
                    } else if (playingSwitchRequested.load()
                               && status == qtav::MediaStatus::Loaded
                               && switchBuffering) {
                        switchLoaded = true;
                    }
                }
                changed.notify_all();
                return false;
            })
        .onAudioFrame([&](const qtav::AudioFrame& frame, int track) {
            assert(frame);
            if (!countFrames.load()) {
                return;
            }
            if (track == expectedAudioTrack.load()) {
                ++expectedAudioFrames;
            } else {
                ++unexpectedAudioFrames;
            }
            changed.notify_all();
        })
        .onVideoFrame([&](const qtav::VideoFrame& frame, int track) {
            assert(frame);
            if (!countFrames.load()) {
                return;
            }
            if (track == expectedVideoTrack.load()) {
                ++expectedVideoFrames;
            } else {
                ++unexpectedVideoFrames;
            }
            changed.notify_all();
        })
        .onSubtitleFrame(
            [&](const qtav::SubtitleFrame& frame, int track) {
                assert(frame);
                assert(!frame.text().empty());
                if (!countFrames.load()) {
                    return;
                }
                if (track == expectedSubtitleTrack.load()) {
                    ++expectedSubtitleFrames;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        lastSubtitleText = frame.text();
                    }
                } else {
                    ++unexpectedSubtitleFrames;
                }
                changed.notify_all();
            });

    player.setMedia(argv[1]);
    player.prepare(0, [&](std::int64_t position, bool* boost) {
        assert(position == 0);
        assert(boost && *boost);
        {
            std::lock_guard<std::mutex> lock(mutex);
            prepared = true;
        }
        changed.notify_all();
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, 10s, [&] { return prepared || failed; }));
    }
    assert(!failed);
    assert(player.state() == qtav::State::Paused);

    const auto initialInfo = player.mediaInfo();
    const auto audioTracks = tracksOfType(initialInfo, qtav::MediaType::Audio);
    const auto videoTracks = tracksOfType(initialInfo, qtav::MediaType::Video);
    const auto subtitleTracks =
        tracksOfType(initialInfo, qtav::MediaType::Subtitle);
    assert(audioTracks.size() >= 2);
    assert(!videoTracks.empty());
    assert(subtitleTracks.size() >= 2);
    if (suppliedAudioSubtitleFixture) {
        assert(!subtitleTracks.empty());
        std::cout << "supplied fixture: video=" << videoTracks.size()
                  << " audio=" << audioTracks.size()
                  << " subtitle=" << subtitleTracks.size()
                  << " activeVideo=" << initialInfo.activeVideoTrack
                  << " activeAudio=" << initialInfo.activeAudioTrack
                  << '\n';
    } else {
        assert(videoTracks.size() >= 2);
    }

    assert(initialInfo.activeAudioTrack >= 0);
    assert(initialInfo.activeVideoTrack >= 0);
    assert(initialInfo.activeSubtitleTrack >= 0);
    assert(!player.setActiveTrack(
        qtav::MediaType::Audio,
        initialInfo.activeVideoTrack));
    assert(!player.setActiveTrack(qtav::MediaType::Audio, 100'000));
    assert(!player.setActiveTrack(qtav::MediaType::Audio, -2));

    const auto& alternateAudio =
        alternateTrack(audioTracks, initialInfo.activeAudioTrack);
    const qtav::TrackInfo* alternateVideo = nullptr;
    if (!suppliedAudioSubtitleFixture) {
        alternateVideo = &alternateTrack(
            videoTracks,
            initialInfo.activeVideoTrack);
    }
    const auto& alternateSubtitle =
        alternateTrack(subtitleTracks, initialInfo.activeSubtitleTrack);

    const int pausedSwitches = alternateVideo ? 3 : 2;
    assert(player.setActiveTrack(
        qtav::MediaType::Audio,
        alternateAudio.index));
    if (alternateVideo) {
        assert(player.setActiveTrack(
            qtav::MediaType::Video,
            alternateVideo->index));
    }
    assert(player.setActiveTrack(
        qtav::MediaType::Subtitle,
        alternateSubtitle.index));
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, 10s, [&] {
            return trackChanges >= pausedSwitches || failed;
        }));
    }
    assert(!failed);
    assert(player.state() == qtav::State::Paused);
    auto switchedInfo = player.mediaInfo();
    assert(switchedInfo.activeAudioTrack == alternateAudio.index);
    if (alternateVideo) {
        assert(switchedInfo.activeVideoTrack == alternateVideo->index);
    }
    assert(switchedInfo.activeSubtitleTrack == alternateSubtitle.index);

    expectedAudioTrack.store(alternateAudio.index);
    expectedVideoTrack.store(
        alternateVideo ? alternateVideo->index
                       : initialInfo.activeVideoTrack);
    expectedSubtitleTrack.store(alternateSubtitle.index);
    countFrames.store(true);
    player.setPlaybackRate(2.0F);
    player.setState(qtav::State::Playing);
    {
        std::unique_lock<std::mutex> lock(mutex);
        const bool received = changed.wait_for(lock, 10s, [&] {
            return (expectedAudioFrames.load() >= 1
                    && expectedVideoFrames.load() >= 1
                    && expectedSubtitleFrames.load() >= 1)
                || failed;
        });
        if (!received) {
            const auto current = player.mediaInfo();
            std::cerr
                << "initial switched playback timeout: state="
                << static_cast<int>(player.state())
                << " status=" << static_cast<int>(player.mediaStatus())
                << " activeAudio=" << current.activeAudioTrack
                << " activeVideo=" << current.activeVideoTrack
                << " expectedAudioFrames=" << expectedAudioFrames.load()
                << " expectedVideoFrames=" << expectedVideoFrames.load()
                << " expectedSubtitleFrames="
                << expectedSubtitleFrames.load()
                << " unexpectedAudioFrames=" << unexpectedAudioFrames.load()
                << " unexpectedVideoFrames=" << unexpectedVideoFrames.load()
                << " unexpectedSubtitleFrames="
                << unexpectedSubtitleFrames.load()
                << '\n';
        }
        assert(received);
    }
    assert(!failed);
    assert(unexpectedAudioFrames.load() == 0);
    assert(unexpectedVideoFrames.load() == 0);
    assert(unexpectedSubtitleFrames.load() == 0);
    if (!suppliedAudioSubtitleFixture) {
        std::lock_guard<std::mutex> lock(mutex);
        assert(lastSubtitleText.find("Alternate") != std::string::npos);
        assert(lastSubtitleText.find('{') == std::string::npos);
    }
    auto sinkSnapshot = audioSink->snapshot();
    assert(sinkSnapshot.openCount >= 1);
    assert(sinkSnapshot.decodedFormat.sampleRate == alternateAudio.sampleRate);

    const auto positionBeforePlayingSwitch = player.position();
    countFrames.store(false);
    playingSwitchRequested.store(true);
    expectedAudioFrames.store(0);
    expectedVideoFrames.store(0);
    expectedSubtitleFrames.store(0);
    unexpectedAudioFrames.store(0);
    unexpectedVideoFrames.store(0);
    unexpectedSubtitleFrames.store(0);
    expectedAudioTrack.store(initialInfo.activeAudioTrack);
    expectedVideoTrack.store(initialInfo.activeVideoTrack);
    expectedSubtitleTrack.store(initialInfo.activeSubtitleTrack);
    assert(player.setActiveTrack(
        qtav::MediaType::Audio,
        initialInfo.activeAudioTrack));
    if (alternateVideo) {
        assert(player.setActiveTrack(
            qtav::MediaType::Video,
            initialInfo.activeVideoTrack));
    }
    assert(player.setActiveTrack(
        qtav::MediaType::Subtitle,
        initialInfo.activeSubtitleTrack));
    countFrames.store(true);

    const int totalSwitches = pausedSwitches * 2;
    {
        std::unique_lock<std::mutex> lock(mutex);
        const bool received = changed.wait_for(lock, 10s, [&] {
            return (trackChanges >= totalSwitches
                    && expectedAudioFrames.load() >= 1
                    && expectedVideoFrames.load() >= 1
                    && expectedSubtitleFrames.load() >= 1
                    && switchLoaded)
                || failed;
        });
        if (!received) {
            const auto current = player.mediaInfo();
            std::cerr
                << "playing switch timeout: state="
                << static_cast<int>(player.state())
                << " status=" << static_cast<int>(player.mediaStatus())
                << " activeAudio=" << current.activeAudioTrack
                << " activeVideo=" << current.activeVideoTrack
                << " trackChanges=" << trackChanges
                << " buffering=" << switchBuffering
                << " loaded=" << switchLoaded
                << " expectedAudioFrames=" << expectedAudioFrames.load()
                << " expectedVideoFrames=" << expectedVideoFrames.load()
                << " expectedSubtitleFrames="
                << expectedSubtitleFrames.load()
                << " unexpectedAudioFrames=" << unexpectedAudioFrames.load()
                << " unexpectedVideoFrames=" << unexpectedVideoFrames.load()
                << " unexpectedSubtitleFrames="
                << unexpectedSubtitleFrames.load()
                << '\n';
        }
        assert(received);
    }
    assert(!failed);
    assert(switchBuffering);
    assert(switchLoaded);
    assert(unexpectedAudioFrames.load() == 0);
    assert(unexpectedVideoFrames.load() == 0);
    assert(unexpectedSubtitleFrames.load() == 0);
    if (!suppliedAudioSubtitleFixture) {
        std::lock_guard<std::mutex> lock(mutex);
        assert(lastSubtitleText.find("Primary") != std::string::npos);
    }
    sinkSnapshot = audioSink->snapshot();
    assert(sinkSnapshot.openCount >= 2);
    assert(sinkSnapshot.closeCount >= 1);
    const auto primaryAudio = std::find_if(
        audioTracks.begin(),
        audioTracks.end(),
        [&](const qtav::TrackInfo& track) {
            return track.index == initialInfo.activeAudioTrack;
        });
    assert(primaryAudio != audioTracks.end());
    assert(sinkSnapshot.decodedFormat.sampleRate == primaryAudio->sampleRate);
    switchedInfo = player.mediaInfo();
    assert(switchedInfo.activeAudioTrack == initialInfo.activeAudioTrack);
    assert(switchedInfo.activeVideoTrack == initialInfo.activeVideoTrack);
    assert(
        switchedInfo.activeSubtitleTrack
        == initialInfo.activeSubtitleTrack);
    assert(player.position() + 250 >= positionBeforePlayingSwitch);

    assert(player.setActiveTrack(qtav::MediaType::Audio, -1));
    assert(player.setActiveTrack(qtav::MediaType::Subtitle, -1));
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, 10s, [&] {
            return trackChanges >= totalSwitches + 2 || failed;
        }));
    }
    assert(!failed);
    assert(player.mediaInfo().activeAudioTrack == -1);
    assert(player.mediaInfo().activeSubtitleTrack == -1);

    player.setState(qtav::State::Stopped);
    assert(player.waitFor(qtav::State::Stopped, 10'000));
    return 0;
}
