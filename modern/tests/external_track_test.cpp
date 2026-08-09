// SPDX-License-Identifier: LGPL-2.1-or-later

#if defined(NDEBUG)
#  undef NDEBUG
#endif

#include <qtav/player.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::vector<qtav::TrackInfo> tracks(
    const qtav::MediaInfo& info,
    qtav::MediaType type,
    bool external)
{
    std::vector<qtav::TrackInfo> result;
    std::copy_if(
        info.tracks.begin(),
        info.tracks.end(),
        std::back_inserter(result),
        [type, external](const qtav::TrackInfo& track) {
            return track.type == type && track.external == external;
        });
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 4 || argc == 5);
    const std::string mainUrl = argv[1];
    const std::string audioUrl = argv[2];
    const std::string subtitleUrl = argv[3];
    const bool sidecarOnly = argc == 5
        && std::string(argv[4]) == "--sidecar-only";
    const bool disablePrimaryVideo = argc == 5
        && std::string(argv[4]) == "--disable-primary-video";
    assert(argc == 4 || sidecarOnly || disablePrimaryVideo);

    qtav::Player player;
    assert(!player.setExternalMedia(qtav::MediaType::Unknown, audioUrl));
    assert(!player.setExternalMedia(qtav::MediaType::Video, audioUrl));
    assert(player.externalMedia(qtav::MediaType::Unknown).empty());
    assert(player.setExternalMedia(qtav::MediaType::Audio, audioUrl));
    assert(player.setExternalMedia(qtav::MediaType::Subtitle, subtitleUrl));
    assert(player.externalMedia(qtav::MediaType::Audio) == audioUrl);
    assert(player.externalMedia(qtav::MediaType::Subtitle) == subtitleUrl);

    std::mutex mutex;
    std::condition_variable changed;
    bool prepared = false;
    bool failed = false;
    int trackChanges = 0;
    int loadedStatuses = 0;
    std::atomic<bool> countFrames { false };
    std::atomic<int> expectedAudioTrack { -1 };
    std::atomic<int> expectedSubtitleTrack { -1 };
    std::atomic<int> audioFrames { 0 };
    std::atomic<int> videoFrames { 0 };
    std::atomic<int> subtitleFrames { 0 };
    std::atomic<int> unexpectedAudioFrames { 0 };
    std::atomic<int> unexpectedSubtitleFrames { 0 };

    player
        .onEvent([&](const qtav::MediaEvent& event) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (event.category == "track.changed") {
                    ++trackChanges;
                } else if (event.category == "media.error"
                           || event.category == "external.error"
                           || event.category == "track.error"
                           || event.category == "reader.error"
                           || event.category == "decode.error"
                           || event.category == "subtitle.decode") {
                    std::cerr << event.category << ": " << event.detail
                              << " (" << event.error << ")\n";
                    failed = true;
                }
            }
            changed.notify_all();
            return false;
        })
        .onMediaStatus([&](qtav::MediaStatus, qtav::MediaStatus status) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (status == qtav::MediaStatus::Loaded) {
                    ++loadedStatuses;
                } else if (status == qtav::MediaStatus::Invalid) {
                    failed = true;
                }
            }
            changed.notify_all();
            return false;
        })
        .onAudioFrame([&](const qtav::AudioFrame& frame, int track) {
            assert(frame);
            if (countFrames.load()) {
                if (track == expectedAudioTrack.load()) {
                    ++audioFrames;
                } else {
                    ++unexpectedAudioFrames;
                }
                changed.notify_all();
            }
        })
        .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
            assert(frame);
            if (countFrames.load()) {
                ++videoFrames;
                changed.notify_all();
            }
        })
        .onSubtitleFrame(
            [&](const qtav::SubtitleFrame& frame, int track) {
                assert(frame);
                assert(!frame.text().empty());
                if (countFrames.load()) {
                    if (track == expectedSubtitleTrack.load()
                        && frame.track() == track) {
                        ++subtitleFrames;
                    } else {
                        ++unexpectedSubtitleFrames;
                    }
                    changed.notify_all();
                }
            });

    player.setMedia(mainUrl);
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
        assert(changed.wait_for(lock, 15s, [&] {
            return prepared || failed;
        }));
    }
    assert(!failed);
    assert(player.state() == qtav::State::Paused);

    const auto info = player.mediaInfo();
    const auto internalAudio = tracks(info, qtav::MediaType::Audio, false);
    const auto externalAudio = tracks(info, qtav::MediaType::Audio, true);
    const auto internalSubtitle =
        tracks(info, qtav::MediaType::Subtitle, false);
    const auto externalSubtitle =
        tracks(info, qtav::MediaType::Subtitle, true);
    const auto internalVideo = tracks(info, qtav::MediaType::Video, false);
    assert(!internalVideo.empty());
    assert(!externalAudio.empty());
    assert(!externalSubtitle.empty());
    if (sidecarOnly) {
        assert(internalAudio.empty());
        assert(internalSubtitle.empty());
    }

    std::set<int> identities;
    for (const auto& track : info.tracks) {
        assert(identities.insert(track.index).second);
        assert(track.streamIndex >= 0);
        if (track.external) {
            assert(track.sourceUrl == audioUrl
                || track.sourceUrl == subtitleUrl);
        } else {
            assert(track.sourceUrl == mainUrl);
            assert(track.index == track.streamIndex);
        }
    }

    const auto audioTrack = externalAudio.front().index;
    const auto subtitleTrack = externalSubtitle.front().index;
    int expectedChanges = 0;
    if (info.activeAudioTrack != audioTrack) {
        assert(player.setActiveTrack(qtav::MediaType::Audio, audioTrack));
        ++expectedChanges;
    }
    if (info.activeSubtitleTrack != subtitleTrack) {
        assert(player.setActiveTrack(
            qtav::MediaType::Subtitle,
            subtitleTrack));
        ++expectedChanges;
    }
    if (disablePrimaryVideo && info.activeVideoTrack >= 0) {
        assert(player.setActiveTrack(qtav::MediaType::Video, -1));
        ++expectedChanges;
    }
    if (expectedChanges > 0) {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, 15s, [&] {
            return trackChanges >= expectedChanges || failed;
        }));
    }
    assert(!failed);
    assert(player.mediaInfo().activeAudioTrack == audioTrack);
    assert(player.mediaInfo().activeSubtitleTrack == subtitleTrack);
    if (disablePrimaryVideo) {
        assert(player.mediaInfo().activeVideoTrack == -1);
    }

    expectedAudioTrack.store(audioTrack);
    expectedSubtitleTrack.store(subtitleTrack);
    countFrames.store(true);
    player.setPlaybackRate(4.0F);
    player.setState(qtav::State::Playing);
    {
        std::unique_lock<std::mutex> lock(mutex);
        const bool received = changed.wait_for(lock, 15s, [&] {
            return (audioFrames.load() >= 1
                    && (disablePrimaryVideo || videoFrames.load() >= 1)
                    && subtitleFrames.load() >= 1)
                || failed;
        });
        if (!received) {
            std::cerr << "external playback timeout: audio="
                      << audioFrames.load() << " video="
                      << videoFrames.load() << " subtitle="
                      << subtitleFrames.load() << " unexpectedAudio="
                      << unexpectedAudioFrames.load()
                      << " unexpectedSubtitle="
                      << unexpectedSubtitleFrames.load() << '\n';
        }
        assert(received);
    }
    assert(!failed);
    assert(unexpectedAudioFrames.load() == 0);
    assert(unexpectedSubtitleFrames.load() == 0);

    assert(player.seek(0, qtav::SeekFlag::KeyFrame));
    const auto audioBeforeSeek = audioFrames.load();
    const auto subtitleBeforeSeek = subtitleFrames.load();
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, 15s, [&] {
            return (audioFrames.load() > audioBeforeSeek
                    && subtitleFrames.load() > subtitleBeforeSeek)
                || failed;
        }));
    }
    assert(!failed);
    assert(player.mediaInfo().activeAudioTrack == audioTrack);
    assert(player.mediaInfo().activeSubtitleTrack == subtitleTrack);

    if (!internalAudio.empty()) {
        player.setState(qtav::State::Paused);
        assert(player.waitFor(qtav::State::Paused, 10'000));
        int loadedBefore = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            loadedBefore = loadedStatuses;
        }
        assert(player.setExternalMedia(qtav::MediaType::Audio, {}));
        {
            std::unique_lock<std::mutex> lock(mutex);
            assert(changed.wait_for(lock, 15s, [&] {
                return loadedStatuses > loadedBefore || failed;
            }));
        }
        assert(!failed);
        assert(player.state() == qtav::State::Paused);
        assert(player.externalMedia(qtav::MediaType::Audio).empty());
        const auto reopened = player.mediaInfo();
        assert(tracks(reopened, qtav::MediaType::Audio, true).empty());
        assert(reopened.activeAudioTrack >= 0);
    }

    player.setState(qtav::State::Stopped);
    assert(player.waitFor(qtav::State::Stopped, 10'000));
    assert(player.externalMedia(qtav::MediaType::Audio)
        == (internalAudio.empty() ? audioUrl : std::string {}));
    assert(player.externalMedia(qtav::MediaType::Subtitle) == subtitleUrl);
    return 0;
}
