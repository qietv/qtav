// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/cpu_video_renderer.h>
#include <qtav/player.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace {

bool allEqual(
    const std::uint8_t* first,
    const std::uint8_t* last,
    std::uint8_t value)
{
    return std::all_of(first, last, [value](std::uint8_t byte) {
        return byte == value;
    });
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);

    auto renderer = std::make_shared<qtav::CpuVideoRenderer>();
    const auto capabilities = renderer->capabilities();
    assert(std::find(
               capabilities.aspectRatioModes.begin(),
               capabilities.aspectRatioModes.end(),
               qtav::VideoAspectRatioMode::Stretch)
        != capabilities.aspectRatioModes.end());
    assert(qtav::cpuImageBytesPerPixel(qtav::PixelFormat::BGRA) == 4);
    assert(qtav::cpuImageBytesPerPixel(qtav::PixelFormat::Gray8) == 1);
    assert(qtav::cpuImageBytesPerPixel(qtav::PixelFormat::YUV420P) == 0);

    int errors = 0;
    renderer->setEventCallback([&](const qtav::VideoRenderEvent& event) {
        if (event.type == qtav::VideoRenderEventType::Error) {
            ++errors;
        }
    });

    qtav::VideoRenderConfig config;
    config.surfaceSize = { 2, 1 };
    assert(!renderer->open(config));
    assert(errors == 1);
    config.aspectRatio = qtav::VideoAspectRatioMode::Stretch;
    assert(renderer->open(config));

    std::array<std::uint8_t, 12> bgra;
    std::array<std::uint8_t, 10> rgba;
    std::array<std::uint8_t, 4> gray;
    bgra.fill(0xcd);
    rgba.fill(0xcd);
    gray.fill(0xcd);

    qtav::CpuImageBuffer invalid {
        bgra.data(),
        2,
        1,
        7,
        qtav::PixelFormat::BGRA,
    };
    assert(!invalid.isValid());
    assert(!renderer->setTarget(invalid));

    std::mutex mutex;
    std::condition_variable condition;
    bool converted = false;
    bool conversionSucceeded = false;

    qtav::Player player;
    player.setVideoRenderAPI(renderer).setRenderCallback([&](void*) {
        const qtav::CpuImageBuffer bgraTarget {
            bgra.data(),
            2,
            1,
            12,
            qtav::PixelFormat::BGRA,
        };
        const qtav::CpuImageBuffer rgbaTarget {
            rgba.data(),
            2,
            1,
            10,
            qtav::PixelFormat::RGBA,
        };
        const qtav::CpuImageBuffer grayTarget {
            gray.data(),
            2,
            1,
            4,
            qtav::PixelFormat::Gray8,
        };

        bool success = renderer->setTarget(bgraTarget);
        success = success && player.renderVideo() >= 0.0;
        success = success && renderer->setTarget(rgbaTarget);
        success = success && player.renderVideo() >= 0.0;
        success = success && renderer->setTarget(grayTarget);
        success = success && player.renderVideo() >= 0.0;

        {
            std::lock_guard<std::mutex> lock(mutex);
            conversionSucceeded = success;
            converted = true;
        }
        condition.notify_one();
        player.setState(qtav::State::Stopped);
    });

    player.setMedia(argv[1]);
    player.setState(qtav::State::Playing);
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(condition.wait_for(
            lock,
            std::chrono::seconds(10),
            [&] { return converted; }));
    }
    assert(conversionSucceeded);

    const std::array<std::uint8_t, 8> expectedBgra {
        51,
        34,
        17,
        255,
        51,
        34,
        17,
        255,
    };
    const std::array<std::uint8_t, 8> expectedRgba {
        17,
        34,
        51,
        255,
        17,
        34,
        51,
        255,
    };
    assert(std::equal(expectedBgra.begin(), expectedBgra.end(), bgra.begin()));
    assert(std::equal(expectedRgba.begin(), expectedRgba.end(), rgba.begin()));
    assert(gray[0] == 31);
    assert(gray[1] == 31);
    assert(allEqual(bgra.data() + 8, bgra.data() + bgra.size(), 0xcd));
    assert(allEqual(rgba.data() + 8, rgba.data() + rgba.size(), 0xcd));
    assert(allEqual(gray.data() + 2, gray.data() + gray.size(), 0xcd));

    renderer->close();
    assert(!renderer->target().isValid());
    assert(errors == 1);
    return 0;
}
