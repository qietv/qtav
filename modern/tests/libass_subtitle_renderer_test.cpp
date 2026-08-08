// SPDX-License-Identifier: LGPL-2.1-or-later

#if defined(NDEBUG)
#  undef NDEBUG
#endif

#include <qtav/libass_subtitle_renderer.h>
#include <qtav/player.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>

namespace {

using namespace std::chrono_literals;

bool validImages(
    const qtav::LibassSubtitleRenderResult& result,
    int frameWidth,
    int frameHeight)
{
    if (result.images.empty()) {
        return false;
    }
    std::uint64_t coverage = 0;
    for (const auto& image : result.images) {
        if (!image.isValid() || image.x < 0 || image.y < 0
            || image.x + image.width > frameWidth
            || image.y + image.height > frameHeight
            || image.opacity == 0) {
            return false;
        }
        coverage += static_cast<std::uint64_t>(std::count_if(
            image.bitmap.begin(),
            image.bitmap.end(),
            [](std::uint8_t value) { return value != 0; }));
    }
    return coverage > 0;
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2 || argc == 3);
    const bool suppliedAudioSubtitleFixture = argc == 3
        && std::string(argv[2]) == "--audio-subtitle";

    qtav::LibassSubtitleRenderer renderer;
    qtav::LibassSubtitleRendererConfig invalidConfig;
    assert(!renderer.configure(invalidConfig));
    assert(!renderer.lastError().empty());

    qtav::LibassSubtitleRendererConfig config;
    config.frameWidth = 640;
    config.frameHeight = 360;
    config.storageWidth = 160;
    config.storageHeight = 90;
    config.defaultFamily = "Arial";
    assert(config.isValid());
    assert(renderer.configure(config));
    assert(renderer.isConfigured());
    assert(renderer.config().frameWidth == config.frameWidth);
    assert(renderer.render(0).images.empty());

    qtav::Player player;
    std::mutex mutex;
    std::condition_variable changed;
    bool prepared = false;
    bool failed = false;
    bool assSwitchComplete = false;
    bool primaryRendered = false;
    bool alternateRendered = false;
    int primaryTrack = -1;
    int alternateTrack = -1;
    std::uint64_t primaryGeneration = 0;
    std::uint64_t alternateGeneration = 0;

    player
        .onEvent([&](const qtav::MediaEvent& event) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (event.category == "track.changed"
                    && player.mediaInfo().activeSubtitleTrack
                        == alternateTrack) {
                    assSwitchComplete = true;
                } else if (event.category == "media.error"
                           || event.category == "decode.error"
                           || event.category == "subtitle.decode"
                           || event.category == "track.error") {
                    std::cerr << event.category << ": " << event.detail
                              << '\n';
                    failed = true;
                }
            }
            changed.notify_all();
            return false;
        })
        .onMediaStatus(
            [&](qtav::MediaStatus, qtav::MediaStatus status) {
                if (status == qtav::MediaStatus::Invalid) {
                    std::lock_guard<std::mutex> lock(mutex);
                    failed = true;
                    changed.notify_all();
                }
                return false;
            })
        .onSubtitleFrame(
            [&](const qtav::SubtitleFrame& frame, int track) {
                assert(frame);
                assert(frame.track() == track);
                assert(frame.presentationGeneration() != 0);
                assert(!frame.text().empty());
                const auto assEvents = frame.assEvents();
                assert(!assEvents.empty());
                assert(!frame.assHeader().empty());
                assert(renderer.add(frame));
                const auto result = renderer.render(
                    frame.timestamp()
                    + std::max<std::int64_t>(
                        1,
                        std::min<std::int64_t>(50, frame.duration() / 2)));
                assert(validImages(result, config.frameWidth, config.frameHeight));

                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (track == primaryTrack && !primaryRendered) {
                        primaryGeneration = frame.presentationGeneration();
                        primaryRendered = true;
                    } else if (track == alternateTrack
                               && !alternateRendered) {
                        if (!suppliedAudioSubtitleFixture) {
                            assert(frame.text().find("Alternate")
                                != std::string::npos);
                            assert(frame.text().find('{') == std::string::npos);
                            assert(std::any_of(
                                assEvents.begin(),
                                assEvents.end(),
                                [](const std::string& event) {
                                    return event.find('\\')
                                        != std::string::npos;
                                }));
                        }
                        alternateGeneration =
                            frame.presentationGeneration();
                        alternateRendered = true;
                    }
                }
                changed.notify_all();
            });

    player.setMedia(argv[1]);
    player.prepare(0, [&](std::int64_t, bool*) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            prepared = true;
        }
        changed.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, 10s, [&] {
            return prepared || failed;
        }));
    }
    assert(!failed);

    const auto info = player.mediaInfo();
    for (const auto& track : info.tracks) {
        if (track.type != qtav::MediaType::Subtitle) {
            continue;
        }
        if (track.codec == "ass") {
            alternateTrack = track.index;
        } else if (primaryTrack < 0) {
            primaryTrack = track.index;
        }
    }
    assert(primaryTrack >= 0);
    if (alternateTrack < 0) {
        const auto alternate = std::find_if(
            info.tracks.begin(),
            info.tracks.end(),
            [&](const qtav::TrackInfo& track) {
                return track.type == qtav::MediaType::Subtitle
                    && track.index != primaryTrack;
            });
        assert(alternate != info.tracks.end());
        alternateTrack = alternate->index;
    }
    assert(alternateTrack >= 0);
    assert(info.activeSubtitleTrack == primaryTrack);

    player.setPlaybackRate(4.0F);
    player.setState(qtav::State::Playing);
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, 10s, [&] {
            return primaryRendered || failed;
        }));
    }
    assert(!failed);

    assert(player.setActiveTrack(
        qtav::MediaType::Subtitle, alternateTrack));
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, 10s, [&] {
            return (assSwitchComplete && alternateRendered) || failed;
        }));
    }
    assert(!failed);
    assert(primaryGeneration != alternateGeneration);
    assert(renderer.lastError().empty());

    renderer.flush();
    assert(renderer.render(0).images.empty());
    player.setState(qtav::State::Stopped);
    assert(player.waitFor(qtav::State::Stopped, 10'000));
    return 0;
}
