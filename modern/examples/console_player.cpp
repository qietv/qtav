// SPDX-License-Identifier: GPL-3.0-or-later

#include <qtav/player.h>

#if defined(QTAV_CORE_CONSOLE_HAS_COREAUDIO)
#  include <qtav/coreaudio_audio_sink.h>
#  include <qtav/swresample_audio_converter.h>
#endif

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>

namespace {

const char* stateName(qtav::State state)
{
    switch (state) {
    case qtav::State::Stopped:
        return "stopped";
    case qtav::State::Playing:
        return "playing";
    case qtav::State::Paused:
        return "paused";
    }
    return "unknown";
}

const char* statusName(qtav::MediaStatus status)
{
    switch (status) {
    case qtav::MediaStatus::NoMedia:
        return "no-media";
    case qtav::MediaStatus::Loading:
        return "loading";
    case qtav::MediaStatus::Loaded:
        return "loaded";
    case qtav::MediaStatus::Buffering:
        return "buffering";
    case qtav::MediaStatus::EndOfMedia:
        return "end";
    case qtav::MediaStatus::Invalid:
        return "invalid";
    }
    return "unknown";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: qtav_core_console <media-url>\n";
        return 2;
    }

    qtav::Player player;
#if defined(QTAV_CORE_CONSOLE_HAS_COREAUDIO)
    player
        .setAudioFrameConverter(
            std::make_shared<qtav::SwresampleAudioConverter>())
        .setAudioSink(
            std::make_shared<qtav::CoreAudioAudioSink>());
#endif

    std::atomic<std::uint64_t> decodedVideoFrames { 0 };
    std::atomic<std::uint64_t> renderedVideoFrames { 0 };
    std::atomic<std::uint64_t> decodedAudioFrames { 0 };

    player
        .onStateChanged([](qtav::State state) {
            std::cout << "state: " << stateName(state) << '\n';
        })
        .onMediaStatus([](qtav::MediaStatus oldStatus,
                           qtav::MediaStatus newStatus) {
            std::cout << "status: " << statusName(oldStatus) << " -> "
                      << statusName(newStatus) << '\n';
            return false;
        })
        .onEvent([](const qtav::MediaEvent& event) {
            std::cerr << event.category << ": " << event.detail << '\n';
            return false;
        })
        .onVideoFrame([&](const qtav::VideoFrame&, int) {
            ++decodedVideoFrames;
        })
        .onAudioFrame([&](const qtav::AudioFrame&, int) {
            ++decodedAudioFrames;
        })
        .setVideoRenderer(
            [&](const qtav::VideoFrame& frame, void*) {
                ++renderedVideoFrames;
                if (renderedVideoFrames == 1) {
                    std::cout << "video: " << frame.width() << 'x'
                              << frame.height() << ' ' << frame.formatName()
                              << " colorspace=" << frame.colorSpace() << '\n';
                }
            })
        .setRenderCallback([&](void*) {
            // A GUI integration schedules renderVideo() on its render thread.
            // This headless example can render immediately on the decode thread.
            player.renderVideo();
        });

    player.setMedia(argv[1]);
    player.setState(qtav::State::Playing);

    if (!player.waitFor(qtav::State::Playing, 10'000)) {
        if (player.mediaStatus() == qtav::MediaStatus::Invalid) {
            return 1;
        }
    }
    if (!player.waitFor(qtav::State::Stopped, 60'000)) {
        std::cerr << "playback timed out\n";
        player.setState(qtav::State::Stopped);
        return 1;
    }

    const auto info = player.mediaInfo();
    std::cout << "duration-ms: " << info.duration << '\n'
              << "video-frames: " << decodedVideoFrames.load() << '\n'
              << "rendered-frames: " << renderedVideoFrames.load() << '\n'
              << "audio-frames: " << decodedAudioFrames.load() << '\n';
    return player.mediaStatus() == qtav::MediaStatus::Invalid ? 1 : 0;
}
