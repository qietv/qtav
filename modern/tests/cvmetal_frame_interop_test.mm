// SPDX-License-Identifier: LGPL-2.1-or-later

#import <CoreVideo/CVPixelBuffer.h>
#import <Metal/Metal.h>

#include <qtav/cvmetal_frame_interop.h>
#include <qtav/player.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace {

class PixelBufferFrameData final : public qtav::HardwareFrameData {
public:
    explicit PixelBufferFrameData(CVPixelBufferRef buffer)
        : buffer_(CVPixelBufferRetain(buffer))
    {
    }

    ~PixelBufferFrameData() override
    {
        CVPixelBufferRelease(buffer_);
    }

    qtav::HardwareDeviceType deviceType() const noexcept override
    {
        return qtav::HardwareDeviceType::VideoToolbox;
    }

    int width() const noexcept override
    {
        return static_cast<int>(CVPixelBufferGetWidth(buffer_));
    }

    int height() const noexcept override
    {
        return static_cast<int>(CVPixelBufferGetHeight(buffer_));
    }

    qtav::PixelFormat softwareFormat() const noexcept override
    {
        const OSType format = CVPixelBufferGetPixelFormatType(buffer_);
        return format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
                || format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
            ? qtav::PixelFormat::P010
            : qtav::PixelFormat::NV12;
    }

    qtav::NativeHandle nativeHandle(
        qtav::HardwareHandleType type) const noexcept override
    {
        if (type != qtav::HardwareHandleType::Frame
            && type != qtav::HardwareHandleType::Surface) {
            return { type, 0 };
        }
        return {
            type,
            reinterpret_cast<std::uintptr_t>(buffer_),
        };
    }

    bool isMappable(qtav::HardwareMapMode) const noexcept override
    {
        return false;
    }

    std::shared_ptr<qtav::HardwareFrameMapping> map(
        qtav::HardwareMapMode) const override
    {
        ++mapCalls_;
        return {};
    }

    int mapCalls() const noexcept
    {
        return mapCalls_.load();
    }

private:
    CVPixelBufferRef buffer_ = nullptr;
    mutable std::atomic<int> mapCalls_ { 0 };
};

struct Pixel {
    std::uint8_t blue = 0;
    std::uint8_t green = 0;
    std::uint8_t red = 0;
    std::uint8_t alpha = 0;
};

bool isRed(Pixel pixel)
{
    return pixel.red > 160 && pixel.red > pixel.green * 2
        && pixel.red > pixel.blue * 2 && pixel.alpha > 247;
}

bool isBlue(Pixel pixel)
{
    return pixel.blue > 160 && pixel.blue > pixel.green * 2
        && pixel.blue > pixel.red * 2 && pixel.alpha > 247;
}

id<MTLTexture> makeTarget(
    id<MTLDevice> device,
    int width,
    int height)
{
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:static_cast<NSUInteger>(width)
                                        height:static_cast<NSUInteger>(height)
                                     mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageRenderTarget;
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

void testDirectImport(
    id<MTLDevice> device,
    const std::shared_ptr<qtav::CVMetalFrameInterop>& interop)
{
    NSDictionary* attributes = @{
        (id)kCVPixelBufferMetalCompatibilityKey : @YES,
        (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    };
    CVPixelBufferRef buffer = nullptr;
    assert(
        CVPixelBufferCreate(
            kCFAllocatorDefault,
            4,
            2,
            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            (__bridge CFDictionaryRef)attributes,
            &buffer)
        == kCVReturnSuccess);
    assert(buffer);

    auto data = std::make_shared<PixelBufferFrameData>(buffer);
    qtav::HardwareFrame frame(data);
    CVPixelBufferRelease(buffer);

    assert(interop->device().get() == device);
    const auto capabilities = interop->capabilities();
    assert(capabilities.zeroCopy);
    assert(!capabilities.cpuFallback);
    assert(
        capabilities.targetDevice
        == qtav::HardwareDeviceType::Metal);
    assert(
        std::find(
            capabilities.sourceDevices.begin(),
            capabilities.sourceDevices.end(),
            qtav::HardwareDeviceType::VideoToolbox)
        != capabilities.sourceDevices.end());
    assert(interop->supports(frame));

    auto imported = interop->importFrame(frame);
    assert(imported);
    assert(imported->width() == 4);
    assert(imported->height() == 2);
    assert(imported->format() == qtav::PixelFormat::NV12);
    assert(imported->colorRange() == qtav::ColorRange::Limited);
    assert(imported->planeCount() == 2);
    assert(imported->texture(0));
    assert(imported->texture(1));
    assert(imported->texture(0).pixelFormat == MTLPixelFormatR8Unorm);
    assert(imported->texture(1).pixelFormat == MTLPixelFormatRG8Unorm);
    assert(imported->texture(0).width == 4);
    assert(imported->texture(1).width == 2);
    assert(data->mapCalls() == 0);

    frame = {};
    assert(imported->texture(0));
    imported.reset();
    interop->flush();
    assert(data->mapCalls() == 0);

    CVPixelBufferRef p010Buffer = nullptr;
    assert(
        CVPixelBufferCreate(
            kCFAllocatorDefault,
            4,
            2,
            kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
            (__bridge CFDictionaryRef)attributes,
            &p010Buffer)
        == kCVReturnSuccess);
    assert(p010Buffer);
    auto p010Data =
        std::make_shared<PixelBufferFrameData>(p010Buffer);
    qtav::HardwareFrame p010Frame(p010Data);
    CVPixelBufferRelease(p010Buffer);

    assert(interop->supports(p010Frame));
    auto p010Import = interop->importFrame(p010Frame);
    assert(p010Import);
    assert(p010Import->format() == qtav::PixelFormat::P010);
    assert(p010Import->colorRange() == qtav::ColorRange::Limited);
    assert(p010Import->texture(0).pixelFormat == MTLPixelFormatR16Unorm);
    assert(p010Import->texture(1).pixelFormat == MTLPixelFormatRG16Unorm);
    assert(p010Data->mapCalls() == 0);

    CVPixelBufferRef fullRangeBuffer = nullptr;
    assert(
        CVPixelBufferCreate(
            kCFAllocatorDefault,
            4,
            2,
            kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
            (__bridge CFDictionaryRef)attributes,
            &fullRangeBuffer)
        == kCVReturnSuccess);
    assert(fullRangeBuffer);
    auto fullRangeData =
        std::make_shared<PixelBufferFrameData>(fullRangeBuffer);
    qtav::HardwareFrame fullRangeFrame(fullRangeData);
    CVPixelBufferRelease(fullRangeBuffer);

    assert(interop->supports(fullRangeFrame));
    auto fullRangeImport = interop->importFrame(fullRangeFrame);
    assert(fullRangeImport);
    assert(fullRangeImport->format() == qtav::PixelFormat::NV12);
    assert(fullRangeImport->colorRange() == qtav::ColorRange::Full);
    assert(fullRangeData->mapCalls() == 0);
}

void testPlayerRendering(
    const char* media,
    id<MTLDevice> device,
    id<MTLCommandQueue> queue,
    const std::shared_ptr<qtav::CVMetalFrameInterop>& interop)
{
    id<MTLTexture> target = makeTarget(device, 8, 8);
    assert(target);
    auto renderer = std::make_shared<qtav::MetalVideoRenderer>(
        qtav::BorrowedMetalDevice(device),
        qtav::BorrowedMetalCommandQueue(queue),
        [&] {
            qtav::MetalRenderTarget result;
            result.texture = target;
            result.waitUntilCompleted = true;
            return result;
        },
        interop);

    qtav::VideoRenderConfig config;
    config.surfaceSize = { 8, 8 };
    config.aspectRatio = qtav::VideoAspectRatioMode::Stretch;
    assert(renderer->open(config));
    assert(renderer->hardwareFrameInterop() == interop);
    const auto renderCapabilities = renderer->capabilities();
    assert(
        std::find(
            renderCapabilities.hardwareDevices.begin(),
            renderCapabilities.hardwareDevices.end(),
            qtav::HardwareDeviceType::VideoToolbox)
        != renderCapabilities.hardwareDevices.end());

    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> fallback { false };
    std::atomic<bool> rendered { false };
    std::atomic<bool> hardwareRendered { false };
    bool finished = false;
    qtav::HardwareFrame retainedHardwareFrame;

    {
        qtav::Player player;
        player
            .setHardwareDecodeConfig({
                qtav::HardwareDeviceType::VideoToolbox,
                true,
            })
            .setVideoRenderAPI(renderer)
            .onEvent([&](const qtav::MediaEvent& event) {
                if (event.category == "decoder.hardware.fallback") {
                    fallback.store(true);
                }
                return false;
            })
            .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
                if (frame.hasHardwareFrame()) {
                    hardwareRendered.store(true);
                    std::lock_guard<std::mutex> lock(mutex);
                    retainedHardwareFrame = frame.hardwareFrame();
                }
            })
            .setRenderCallback([&](void*) {
                rendered.store(player.renderVideo() >= 0.0);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    finished = true;
                }
                changed.notify_one();
                player.setState(qtav::State::Stopped);
            });

        player.setMedia(media);
        player.setState(qtav::State::Playing);

        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            std::chrono::seconds(10),
            [&] { return finished; }));
    }

    assert(rendered.load());
    if (!fallback.load()) {
        assert(hardwareRendered.load());
    }
    if (retainedHardwareFrame) {
        auto retainedImport = interop->importFrame(retainedHardwareFrame);
        assert(retainedImport);
        assert(retainedImport->texture(0));
        retainedHardwareFrame = {};
        assert(retainedImport->texture(0));
    }

    const auto pixels = readTarget(target);
    assert(isRed(pixels[static_cast<std::size_t>(3 * 8 + 1)]));
    assert(isBlue(pixels[static_cast<std::size_t>(3 * 8 + 6)]));

    renderer->setHardwareFrameInterop({});
    assert(!renderer->hardwareFrameInterop());
    renderer->close();
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);

    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        assert(device);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        assert(queue);

        auto interop = std::make_shared<qtav::CVMetalFrameInterop>(
            qtav::BorrowedMetalDevice(device));
        testDirectImport(device, interop);
        testPlayerRendering(argv[1], device, queue, interop);
    }
    return 0;
}
