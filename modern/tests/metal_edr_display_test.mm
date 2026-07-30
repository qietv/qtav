// SPDX-License-Identifier: LGPL-2.1-or-later

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>

#include "frame_internal.h"

#include <qtav/metal_video_renderer.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixfmt.h>
}

namespace {

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

void pumpEvents()
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline) {
        @autoreleasepool {
            NSEvent* event = [NSApp
                nextEventMatchingMask:NSEventMaskAny
                             untilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]
                                inMode:NSDefaultRunLoopMode
                               dequeue:YES];
            if (event) {
                [NSApp sendEvent:event];
            }
            [NSApp updateWindows];
        }
    }
}

} // namespace

int main()
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSScreen* screen = nil;
        float potentialHeadroom = 1.0F;
        for (NSScreen* candidate in NSScreen.screens) {
            const float candidateHeadroom =
                qtav::metalPotentialEDRHeadroom(candidate);
            if (candidateHeadroom > potentialHeadroom) {
                screen = candidate;
                potentialHeadroom = candidateHeadroom;
            }
        }
        if (!screen || potentialHeadroom <= 1.0F) {
            return 77;
        }

        constexpr int Width = 64;
        constexpr int Height = 64;
        const NSRect visible = screen.visibleFrame;
        const NSRect windowFrame = NSMakeRect(
            NSMinX(visible) + 8.0,
            NSMinY(visible) + 8.0,
            Width,
            Height);
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:windowFrame
                      styleMask:NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered
                          defer:NO
                         screen:screen];
        assert(window);
        NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(
            0.0,
            0.0,
            Width,
            Height)];
        CAMetalLayer* layer = [CAMetalLayer layer];
        assert(view && layer);
        view.wantsLayer = YES;
        view.layer = layer;
        window.contentView = view;
        window.backgroundColor = NSColor.blackColor;
        window.opaque = YES;
        [window orderFront:nil];
        pumpEvents();

        id<MTLDevice> device = layer.preferredDevice;
        if (!device) {
            device = MTLCreateSystemDefaultDevice();
        }
        assert(device);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        assert(queue);

        qtav::MetalEDRToneMapping toneMapping =
            qtav::MetalEDRToneMapping::System;
        auto renderer = std::make_shared<qtav::MetalVideoRenderer>(
            qtav::BorrowedMetalDevice(device),
            qtav::BorrowedMetalCommandQueue(queue),
            [&] {
                qtav::MetalRenderTarget target;
                target.layer = layer;
                target.display = screen;
                target.outputColorSpace =
                    qtav::MetalOutputColorSpace::ExtendedLinearBT2020;
                target.edrToneMapping = toneMapping;
                target.waitUntilCompleted = true;
                return target;
            });
        qtav::VideoRenderConfig config;
        config.surfaceSize = { Width, Height };
        config.aspectRatio = qtav::VideoAspectRatioMode::Stretch;
        assert(renderer->open(config));

        const qtav::VideoFrame frame = makeHDRFrame(Width, Height);
        assert(renderer->render(frame));
        assert(layer.pixelFormat == MTLPixelFormatRGBA16Float);
        assert(layer.wantsExtendedDynamicRangeContent);
        assert(layer.EDRMetadata);
        assert(CFEqual(
            CGColorSpaceGetName(layer.colorspace),
            kCGColorSpaceExtendedLinearITUR_2020));
        pumpEvents();

        const float currentHeadroom =
            qtav::metalCurrentEDRHeadroom(screen);
        if (currentHeadroom <= 1.0F) {
            renderer->close();
            [window orderOut:nil];
            [window close];
            return 77;
        }

        toneMapping = qtav::MetalEDRToneMapping::DisplayAdaptive;
        assert(renderer->render(frame));
        assert(layer.EDRMetadata == nil);

        renderer->close();
        [window orderOut:nil];
        [window close];
    }
    return 0;
}
