// SPDX-License-Identifier: LGPL-2.1-or-later

#import <Metal/Metal.h>
#import <CoreGraphics/CoreGraphics.h>

#include "frame_internal.h"

#include <qtav/metal_video_renderer.h>
#include <qtav/player.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixfmt.h>
}

namespace {

struct Pixel {
    std::uint8_t blue = 0;
    std::uint8_t green = 0;
    std::uint8_t red = 0;
    std::uint8_t alpha = 0;
};

struct FloatPixel {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    float alpha = 0.0F;
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

float halfToFloat(std::uint16_t value)
{
    const std::uint32_t sign =
        static_cast<std::uint32_t>(value & 0x8000U) << 16;
    std::uint32_t exponent = (value >> 10) & 0x1FU;
    std::uint32_t mantissa = value & 0x03FFU;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            int shift = 0;
            while ((mantissa & 0x0400U) == 0) {
                mantissa <<= 1;
                ++shift;
            }
            mantissa &= 0x03FFU;
            bits = sign
                | static_cast<std::uint32_t>(127 - 15 - shift) << 23
                | mantissa << 13;
        }
    } else if (exponent == 0x1FU) {
        bits = sign | 0x7F800000U | mantissa << 13;
    } else {
        exponent += 127 - 15;
        bits = sign | exponent << 23 | mantissa << 13;
    }
    float result = 0.0F;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

std::vector<FloatPixel> readFloatTarget(id<MTLTexture> texture)
{
    std::vector<std::uint16_t> storage(
        texture.width * texture.height * 4);
    [texture getBytes:storage.data()
          bytesPerRow:texture.width * 4 * sizeof(std::uint16_t)
           fromRegion:MTLRegionMake2D(0, 0, texture.width, texture.height)
          mipmapLevel:0];
    std::vector<FloatPixel> result(texture.width * texture.height);
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = {
            halfToFloat(storage[index * 4]),
            halfToFloat(storage[index * 4 + 1]),
            halfToFloat(storage[index * 4 + 2]),
            halfToFloat(storage[index * 4 + 3]),
        };
    }
    return result;
}

qtav::VideoFrame makeHDRFrame(int width, int height)
{
    AVFrame* native = av_frame_alloc();
    assert(native);
    native->width = width;
    native->height = height;
    native->format = AV_PIX_FMT_RGB24;
    native->color_range = AVCOL_RANGE_JPEG;
    native->color_primaries = AVCOL_PRI_BT2020;
    native->color_trc = AVCOL_TRC_SMPTE2084;
    native->colorspace = AVCOL_SPC_RGB;
    assert(av_frame_get_buffer(native, 32) == 0);
    assert(av_frame_make_writable(native) == 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::uint8_t* rgb =
                native->data[0] + y * native->linesize[0] + x * 3;
            rgb[0] = 191;
            rgb[1] = 0;
            rgb[2] = 0;
        }
    }

    AVMasteringDisplayMetadata* mastering =
        av_mastering_display_metadata_create_side_data(native);
    assert(mastering);
    mastering->has_primaries = 1;
    mastering->display_primaries[0][0] = { 34000, 50000 };
    mastering->display_primaries[0][1] = { 16000, 50000 };
    mastering->display_primaries[1][0] = { 13250, 50000 };
    mastering->display_primaries[1][1] = { 34500, 50000 };
    mastering->display_primaries[2][0] = { 7500, 50000 };
    mastering->display_primaries[2][1] = { 3000, 50000 };
    mastering->white_point[0] = { 15635, 50000 };
    mastering->white_point[1] = { 16450, 50000 };
    mastering->has_luminance = 1;
    mastering->min_luminance = { 1, 10000 };
    mastering->max_luminance = { 1000, 1 };

    AVContentLightMetadata* light =
        av_content_light_metadata_create_side_data(native);
    assert(light);
    light->MaxCLL = 1000;
    light->MaxFALL = 400;

    qtav::VideoFrame frame =
        qtav::detail::FrameFactory::video(native, 0, 40);
    av_frame_free(&native);
    assert(frame);
    return frame;
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
        CAMetalLayer* targetLayer = nil;
        qtav::MetalEDRDisplay targetDisplay = nil;
        qtav::MetalOutputColorSpace outputColorSpace =
            qtav::MetalOutputColorSpace::SDR;
        qtav::MetalEDRToneMapping toneMapping =
            qtav::MetalEDRToneMapping::System;
        float currentEDRHeadroom = 0.0F;
        auto renderer = std::make_shared<qtav::MetalVideoRenderer>(
            borrowedDevice,
            borrowedQueue,
            [&] {
                qtav::MetalRenderTarget result;
                result.texture =
                    exposeTarget && !targetLayer ? target : nil;
                result.layer = targetLayer;
                result.display = targetDisplay;
                result.outputColorSpace = outputColorSpace;
                result.edrToneMapping = toneMapping;
                result.currentEDRHeadroom = currentEDRHeadroom;
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

        const qtav::VideoFrame hdr = makeHDRFrame(6, 4);
        outputColorSpace =
            qtav::MetalOutputColorSpace::ExtendedLinearBT2020;
        toneMapping = qtav::MetalEDRToneMapping::None;
        assert(renderer->render(hdr));
        auto floatPixels = readFloatTarget(target);
        const FloatPixel bt2020Pixel = floatPixels[0];
        assert(bt2020Pixel.red > 5.0F);
        assert(bt2020Pixel.green < 0.01F);
        assert(bt2020Pixel.blue < 0.01F);
        assert(bt2020Pixel.alpha > 0.99F);

        outputColorSpace =
            qtav::MetalOutputColorSpace::ExtendedLinearSRGB;
        assert(renderer->render(hdr));
        floatPixels = readFloatTarget(target);
        assert(floatPixels[0].red > bt2020Pixel.red * 1.5F);

        outputColorSpace =
            qtav::MetalOutputColorSpace::ExtendedLinearBT2020;
        toneMapping = qtav::MetalEDRToneMapping::DisplayAdaptive;
        currentEDRHeadroom = 2.0F;
        assert(renderer->render(hdr));
        floatPixels = readFloatTarget(target);
        const float twoStopHeadroom = floatPixels[0].red;
        assert(twoStopHeadroom > 1.8F && twoStopHeadroom <= 2.01F);

        currentEDRHeadroom = 4.0F;
        assert(renderer->render(hdr));
        floatPixels = readFloatTarget(target);
        assert(floatPixels[0].red > twoStopHeadroom);
        assert(floatPixels[0].red > 3.5F && floatPixels[0].red <= 4.01F);

        currentEDRHeadroom = 1.0F;
        assert(renderer->render(hdr));
        floatPixels = readFloatTarget(target);
        assert(floatPixels[0].red > 0.9F && floatPixels[0].red <= 1.01F);

        assert(qtav::metalCurrentEDRHeadroom(nil) == 1.0F);
        assert(qtav::metalPotentialEDRHeadroom(nil) == 1.0F);

        targetLayer = [CAMetalLayer layer];
        assert(targetLayer);
        target = nil;
        toneMapping = qtav::MetalEDRToneMapping::System;
        currentEDRHeadroom = 0.0F;
        targetLayer.bounds = CGRectMake(0, 0, 6, 4);
        assert(renderer->render(hdr));
        assert(targetLayer.device == device);
        assert(targetLayer.pixelFormat == MTLPixelFormatRGBA16Float);
        assert(targetLayer.wantsExtendedDynamicRangeContent);
        assert(targetLayer.EDRMetadata);
        assert(targetLayer.colorspace);
        assert(CFEqual(
            CGColorSpaceGetName(targetLayer.colorspace),
            kCGColorSpaceExtendedLinearITUR_2020));

        renderer->close();
        assert(!renderer->render(rgb));
        assert(errors == 3);
    }
    return 0;
}
