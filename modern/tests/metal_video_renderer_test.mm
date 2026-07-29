// SPDX-License-Identifier: LGPL-2.1-or-later

#import <Metal/Metal.h>

#include <qtav/metal_video_renderer.h>
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
#include <vector>

namespace {

struct Pixel {
    std::uint8_t blue = 0;
    std::uint8_t green = 0;
    std::uint8_t red = 0;
    std::uint8_t alpha = 0;
};

bool isBlack(Pixel pixel)
{
    return pixel.red < 8 && pixel.green < 8 && pixel.blue < 8
        && pixel.alpha > 247;
}

bool isRed(Pixel pixel)
{
    return pixel.red > 180 && pixel.red > pixel.green * 2
        && pixel.red > pixel.blue * 2 && pixel.alpha > 247;
}

bool isBlue(Pixel pixel)
{
    return pixel.blue > 180 && pixel.blue > pixel.green * 2
        && pixel.blue > pixel.red * 2 && pixel.alpha > 247;
}

id<MTLTexture> makeTarget(
    id<MTLDevice> device,
    int width,
    int height,
    MTLPixelFormat format = MTLPixelFormatBGRA8Unorm)
{
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:format
                                         width:static_cast<NSUInteger>(width)
                                        height:static_cast<NSUInteger>(height)
                                     mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [device newTextureWithDescriptor:descriptor];
}

std::vector<Pixel> readTarget(id<MTLTexture> texture)
{
    std::vector<Pixel> result(texture.width * texture.height);
    [texture getBytes:result.data()
          bytesPerRow:texture.width * sizeof(Pixel)
           fromRegion:MTLRegionMake2D(0, 0, texture.width, texture.height)
          mipmapLevel:0];
    return result;
}

Pixel pixel(
    const std::vector<Pixel>& pixels,
    int width,
    int x,
    int y)
{
    return pixels[static_cast<std::size_t>(y * width + x)];
}

qtav::VideoFrame renderFile(
    const char* path,
    qtav::PixelFormat expectedFormat,
    const std::shared_ptr<qtav::MetalVideoRenderer>& renderer)
{
    std::mutex mutex;
    std::condition_variable condition;
    bool finished = false;
    bool rendered = false;
    qtav::VideoFrame captured;

    qtav::Player player;
    player
        .setVideoRenderAPI(renderer)
        .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
            captured = frame;
        })
        .setRenderCallback([&](void*) {
            rendered = player.renderVideo() >= 0.0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                finished = true;
            }
            condition.notify_one();
            player.setState(qtav::State::Stopped);
        });
    player.setMedia(path);
    player.setState(qtav::State::Playing);

    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(condition.wait_for(
            lock,
            std::chrono::seconds(10),
            [&] { return finished; }));
    }
    assert(rendered);
    assert(captured);
    assert(captured.format() == expectedFormat);
    return captured;
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 4);

    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        assert(device);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        assert(queue);

        qtav::BorrowedMetalDevice borrowedDevice(device);
        qtav::BorrowedMetalCommandQueue borrowedQueue(queue);
        assert(borrowedDevice);
        assert(borrowedQueue);
        assert(borrowedDevice.get() == device);
        assert(borrowedQueue.get() == queue);

        id<MTLTexture> target = makeTarget(device, 8, 8);
        assert(target);
        bool exposeTarget = true;
        qtav::MetalOutputColorSpace outputColorSpace =
            qtav::MetalOutputColorSpace::SDR;
        auto renderer = std::make_shared<qtav::MetalVideoRenderer>(
            borrowedDevice,
            borrowedQueue,
            [&] {
                qtav::MetalRenderTarget result;
                result.texture = exposeTarget ? target : nil;
                result.outputColorSpace = outputColorSpace;
                result.waitUntilCompleted = true;
                return result;
            });

        const auto capabilities = renderer->capabilities();
        assert(capabilities.customViewport);
        assert(capabilities.rotation);
        assert(std::find(
                   capabilities.softwareFormats.begin(),
                   capabilities.softwareFormats.end(),
                   qtav::PixelFormat::NV12)
            != capabilities.softwareFormats.end());
        assert(std::find(
                   capabilities.softwareFormats.begin(),
                   capabilities.softwareFormats.end(),
                   qtav::PixelFormat::P010)
            != capabilities.softwareFormats.end());

        int errors = 0;
        int surfacesLost = 0;
        int redraws = 0;
        renderer->setEventCallback(
            [&](const qtav::VideoRenderEvent& event) {
                if (event.type == qtav::VideoRenderEventType::Error) {
                    ++errors;
                } else if (
                    event.type == qtav::VideoRenderEventType::SurfaceLost) {
                    ++surfacesLost;
                } else if (
                    event.type == qtav::VideoRenderEventType::RedrawRequested) {
                    ++redraws;
                }
            });

        qtav::VideoRenderConfig invalid;
        invalid.surfaceSize = { 8, 8 };
        invalid.viewport = { 7, 0, 2, 2 };
        assert(!renderer->open(invalid));
        assert(errors == 1);

        qtav::VideoRenderConfig config;
        config.surfaceSize = { 8, 8 };
        assert(renderer->open(config));
        assert(renderer->device().get() == device);
        assert(renderer->commandQueue().get() == queue);

        const qtav::VideoFrame rgb =
            renderFile(argv[1], qtav::PixelFormat::RGB24, renderer);
        auto pixels = readTarget(target);
        assert(isBlack(pixel(pixels, 8, 3, 0)));
        assert(isRed(pixel(pixels, 8, 1, 3)));
        assert(isBlue(pixel(pixels, 8, 6, 3)));
        assert(isBlack(pixel(pixels, 8, 3, 7)));

        exposeTarget = false;
        assert(!renderer->render(rgb));
        assert(surfacesLost == 1);
        exposeTarget = true;

        config.viewport = { 2, 1, 4, 6 };
        config.aspectRatio = qtav::VideoAspectRatioMode::Stretch;
        assert(renderer->configure(config));
        assert(redraws == 1);
        assert(renderer->render(rgb));
        pixels = readTarget(target);
        assert(isBlack(pixel(pixels, 8, 1, 3)));
        assert(isRed(pixel(pixels, 8, 2, 3)));
        assert(isBlue(pixel(pixels, 8, 5, 3)));
        assert(isBlack(pixel(pixels, 8, 6, 3)));

        config.viewport = {};
        config.rotation = qtav::VideoRotation::Rotate180;
        assert(renderer->configure(config));
        assert(renderer->render(rgb));
        pixels = readTarget(target);
        assert(isBlue(pixel(pixels, 8, 1, 3)));
        assert(isRed(pixel(pixels, 8, 6, 3)));

        config.rotation = qtav::VideoRotation::Rotate90;
        config.aspectRatio = qtav::VideoAspectRatioMode::Fit;
        assert(renderer->configure(config));
        assert(renderer->render(rgb));
        pixels = readTarget(target);
        assert(isBlack(pixel(pixels, 8, 0, 3)));
        assert(isRed(pixel(pixels, 8, 3, 1)));
        assert(isBlue(pixel(pixels, 8, 3, 6)));

        config.rotation = qtav::VideoRotation::Rotate0;
        config.aspectRatio = qtav::VideoAspectRatioMode::Fill;
        assert(renderer->configure(config));
        assert(renderer->render(rgb));
        pixels = readTarget(target);
        assert(!isBlack(pixel(pixels, 8, 0, 0)));
        assert(!isBlack(pixel(pixels, 8, 7, 7)));

        config.aspectRatio = qtav::VideoAspectRatioMode::Stretch;
        assert(renderer->configure(config));
        renderFile(argv[2], qtav::PixelFormat::YUV420P, renderer);
        pixels = readTarget(target);
        assert(isRed(pixel(pixels, 8, 1, 3)));
        assert(isBlue(pixel(pixels, 8, 6, 3)));

        renderFile(argv[3], qtav::PixelFormat::NV12, renderer);
        pixels = readTarget(target);
        assert(isRed(pixel(pixels, 8, 1, 3)));
        assert(isBlue(pixel(pixels, 8, 6, 3)));

        target = makeTarget(device, 6, 4);
        config.surfaceSize = { 6, 4 };
        assert(renderer->configure(config));
        assert(redraws == 6);
        assert(renderer->render(rgb));
        pixels = readTarget(target);
        assert(isRed(pixel(pixels, 6, 1, 2)));
        assert(isBlue(pixel(pixels, 6, 4, 2)));

        outputColorSpace =
            qtav::MetalOutputColorSpace::ExtendedLinearSRGB;
        assert(!renderer->render(rgb));
        assert(errors == 2);

        target = makeTarget(
            device,
            6,
            4,
            MTLPixelFormatRGBA16Float);
        assert(renderer->render(rgb));

        renderer->close();
        assert(!renderer->render(rgb));
        assert(errors == 3);
    }
    return 0;
}
