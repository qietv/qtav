// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/player.h>
#include <qtav/videotoolbox_hardware_decoder.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace {

void testExplicitSoftwareFallback(const char* media)
{
    qtav::Player player;
    std::mutex mutex;
    std::condition_variable changed;
    bool fallbackReported = false;
    bool frameReceived = false;
    bool stopped = false;

    player
        .setHardwareDecodeConfig({
            qtav::HardwareDeviceType::Metal,
            true,
        })
        .onEvent([&](const qtav::MediaEvent& event) {
            if (event.category == "decoder.hardware.fallback") {
                std::lock_guard<std::mutex> lock(mutex);
                fallbackReported = true;
            }
            return false;
        })
        .onStateChanged([&](qtav::State state) {
            if (state == qtav::State::Stopped) {
                std::lock_guard<std::mutex> lock(mutex);
                stopped = true;
                changed.notify_all();
            }
        })
        .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
            assert(frame);
            assert(!frame.hasHardwareFrame());
            {
                std::lock_guard<std::mutex> lock(mutex);
                frameReceived = true;
            }
            player.setState(qtav::State::Stopped);
        });

    player.setMedia(media);
    player.setState(qtav::State::Playing);

    std::unique_lock<std::mutex> lock(mutex);
    assert(changed.wait_for(
        lock,
        std::chrono::seconds(5),
        [&] { return stopped; }));
    assert(fallbackReported);
    assert(frameReceived);
}

void testDisabledSoftwareFallback(const char* media)
{
    qtav::Player player;
    std::mutex mutex;
    std::condition_variable changed;
    bool hardwareErrorReported = false;
    bool invalid = false;
    std::atomic<int> frames { 0 };

    player
        .setHardwareDecodeConfig({
            qtav::HardwareDeviceType::Metal,
            false,
        })
        .onEvent([&](const qtav::MediaEvent& event) {
            if (event.category == "decoder.hardware.error") {
                std::lock_guard<std::mutex> lock(mutex);
                hardwareErrorReported = true;
            }
            return false;
        })
        .onMediaStatus(
            [&](qtav::MediaStatus, qtav::MediaStatus status) {
                if (status == qtav::MediaStatus::Invalid) {
                    std::lock_guard<std::mutex> lock(mutex);
                    invalid = true;
                    changed.notify_all();
                }
                return false;
            })
        .onVideoFrame([&](const qtav::VideoFrame&, int) { ++frames; });

    player.setMedia(media);
    player.setState(qtav::State::Playing);

    std::unique_lock<std::mutex> lock(mutex);
    assert(changed.wait_for(
        lock,
        std::chrono::seconds(5),
        [&] { return invalid; }));
    assert(hardwareErrorReported);
    assert(frames.load() == 0);
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    testExplicitSoftwareFallback(argv[1]);
    testDisabledSoftwareFallback(argv[1]);

    qtav::HardwareFrame retainedFrame;
    std::atomic<int> videoFrames { 0 };
    std::atomic<int> hardwareFrames { 0 };
    std::atomic<bool> softwareFallback { false };
    std::mutex mutex;
    std::condition_variable changed;
    int stage = 0;
    bool stopped = false;
    bool failed = false;

    {
        qtav::Player player;
        const auto decodeConfig =
            qtav::videoToolboxHardwareDecodeConfig();
        assert(
            decodeConfig.deviceType
            == qtav::HardwareDeviceType::VideoToolbox);
        assert(decodeConfig.allowSoftwareFallback);
        player.setHardwareDecodeConfig(decodeConfig);
        assert(
            player.hardwareDecodeConfig().deviceType
            == qtav::HardwareDeviceType::VideoToolbox);

        player
            .onEvent([&](const qtav::MediaEvent& event) {
                if (event.category == "decoder.hardware.fallback") {
                    softwareFallback.store(true);
                }
                return false;
            })
            .onStateChanged([&](qtav::State state) {
                std::lock_guard<std::mutex> lock(mutex);
                if (state == qtav::State::Stopped && stage == 4) {
                    stopped = true;
                    changed.notify_all();
                }
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
            .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
                assert(frame);
                ++videoFrames;

                if (frame.hasHardwareFrame()) {
                    const auto hardware = frame.hardwareFrame();
                    assert(hardware);
                    assert(
                        hardware.deviceType()
                        == qtav::HardwareDeviceType::VideoToolbox);
                    assert(hardware.width() == frame.width());
                    assert(hardware.height() == frame.height());
                    assert(frame.planeCount() == 0);
                    assert(frame.data() == nullptr);
                    assert(frame.lineSize() == 0);

                    const auto pixelBuffer =
                        qtav::videoToolboxPixelBuffer(hardware);
                    assert(pixelBuffer);
                    assert(
                        static_cast<int>(
                            CVPixelBufferGetWidth(pixelBuffer))
                        == frame.width());
                    assert(
                        static_cast<int>(
                            CVPixelBufferGetHeight(pixelBuffer))
                        == frame.height());

                    if (hardwareFrames.fetch_add(1) == 0) {
                        assert(hardware.softwareFormat() !=
                            qtav::PixelFormat::Unknown);
                        assert(frame.format() == hardware.softwareFormat());
                        assert(hardware.isMappable());
                        const auto mapping = hardware.map();
                        assert(mapping);
                        assert(mapping->width() == frame.width());
                        assert(mapping->height() == frame.height());
                        assert(mapping->planeCount() > 0);
                        assert(mapping->data(0));
                        std::lock_guard<std::mutex> lock(mutex);
                        retainedFrame = hardware;
                    }
                }

                int action = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stage == 0) {
                        stage = 1;
                        action = 1;
                    } else if (stage == 2) {
                        stage = 3;
                        action = 2;
                    } else if (stage == 3) {
                        stage = 4;
                        action = 3;
                    }
                }

                if (action == 1) {
                    assert(player.seek(
                        500,
                        qtav::SeekFlag::FromStart,
                        [&](std::int64_t position) {
                            assert(position == 500);
                            std::lock_guard<std::mutex> lock(mutex);
                            assert(stage == 1);
                            stage = 2;
                        }));
                } else if (action == 2) {
                    player.setMedia(argv[1]);
                } else if (action == 3) {
                    player.setState(qtav::State::Stopped);
                }
            });

        player.setPlaybackRate(8.0F);
        player.setMedia(argv[1]);
        player.setState(qtav::State::Playing);

        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            std::chrono::seconds(10),
            [&] { return stopped || failed; }));
        assert(!failed);
        assert(stopped);
    }

    assert(videoFrames.load() >= 3);
    if (!softwareFallback.load()) {
        assert(hardwareFrames.load() > 0);
    }
    if (retainedFrame) {
        const auto pixelBuffer =
            qtav::videoToolboxPixelBuffer(retainedFrame);
        assert(pixelBuffer);
        assert(CVPixelBufferGetWidth(pixelBuffer) == 160);
        assert(CVPixelBufferGetHeight(pixelBuffer) == 90);
    }
    return 0;
}
