// SPDX-License-Identifier: LGPL-2.1-or-later

#import <qtav/cvmetal_frame_interop.h>

#import <CoreVideo/CVMetalTextureCache.h>
#import <CoreVideo/CVPixelBuffer.h>

#include <array>
#include <mutex>
#include <utility>

namespace qtav {
namespace {

struct CVMetalFormat {
    PixelFormat format = PixelFormat::Unknown;
    ColorRange range = ColorRange::Unknown;
    MTLPixelFormat luma = MTLPixelFormatInvalid;
    MTLPixelFormat chroma = MTLPixelFormatInvalid;
};

bool cvMetalFormat(
    OSType source,
    CVMetalFormat& result) noexcept
{
    switch (source) {
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        result = {
            PixelFormat::NV12,
            ColorRange::Limited,
            MTLPixelFormatR8Unorm,
            MTLPixelFormatRG8Unorm,
        };
        return true;
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
        result = {
            PixelFormat::NV12,
            ColorRange::Full,
            MTLPixelFormatR8Unorm,
            MTLPixelFormatRG8Unorm,
        };
        return true;
    case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
        result = {
            PixelFormat::P010,
            ColorRange::Limited,
            MTLPixelFormatR16Unorm,
            MTLPixelFormatRG16Unorm,
        };
        return true;
    case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
        result = {
            PixelFormat::P010,
            ColorRange::Full,
            MTLPixelFormatR16Unorm,
            MTLPixelFormatRG16Unorm,
        };
        return true;
    default:
        return false;
    }
}

CVPixelBufferRef pixelBuffer(
    const HardwareFrame& frame) noexcept
{
    if (frame.deviceType() != HardwareDeviceType::VideoToolbox) {
        return nullptr;
    }
    return reinterpret_cast<CVPixelBufferRef>(
        frame.nativeHandle(HardwareHandleType::Frame).value);
}

bool supportedPixelBuffer(
    CVPixelBufferRef buffer,
    CVMetalFormat* format = nullptr) noexcept
{
    CVMetalFormat result;
    if (!buffer
        || !CVPixelBufferIsPlanar(buffer)
        || CVPixelBufferGetPlaneCount(buffer) != 2
        || !cvMetalFormat(CVPixelBufferGetPixelFormatType(buffer), result)
        || CVPixelBufferGetWidthOfPlane(buffer, 0) == 0
        || CVPixelBufferGetHeightOfPlane(buffer, 0) == 0
        || CVPixelBufferGetWidthOfPlane(buffer, 1) == 0
        || CVPixelBufferGetHeightOfPlane(buffer, 1) == 0) {
        return false;
    }
    if (format) {
        *format = result;
    }
    return true;
}

class CVMetalTextureFrame final : public MetalTextureFrame {
public:
    CVMetalTextureFrame(
        HardwareFrame source,
        PixelFormat format,
        ColorRange range,
        CVMetalTextureRef luma,
        CVMetalTextureRef chroma)
        : source_(std::move(source))
        , format_(format)
        , range_(range)
        , textures_ { luma, chroma }
    {
    }

    ~CVMetalTextureFrame() override
    {
        for (auto& texture : textures_) {
            if (texture) {
                CFRelease(texture);
                texture = nullptr;
            }
        }
    }

    int width() const noexcept override
    {
        return source_.width();
    }

    int height() const noexcept override
    {
        return source_.height();
    }

    PixelFormat format() const noexcept override
    {
        return format_;
    }

    ColorRange colorRange() const noexcept override
    {
        return range_;
    }

    int planeCount() const noexcept override
    {
        return 2;
    }

    id<MTLTexture> texture(int plane) const noexcept override
    {
        return plane >= 0 && plane < planeCount()
            ? CVMetalTextureGetTexture(textures_[plane])
            : nil;
    }

private:
    HardwareFrame source_;
    PixelFormat format_ = PixelFormat::Unknown;
    ColorRange range_ = ColorRange::Unknown;
    std::array<CVMetalTextureRef, 2> textures_ {};
};

} // namespace

class CVMetalFrameInterop::Impl {
public:
    explicit Impl(BorrowedMetalDevice device)
        : device_(device)
    {
        if (device_) {
            CVMetalTextureCacheCreate(
                kCFAllocatorDefault,
                nullptr,
                device_.get(),
                nullptr,
                &cache_);
        }
    }

    ~Impl()
    {
        if (cache_) {
            CFRelease(cache_);
        }
    }

    mutable std::mutex mutex_;
    BorrowedMetalDevice device_;
    CVMetalTextureCacheRef cache_ = nullptr;
};

CVMetalFrameInterop::CVMetalFrameInterop(BorrowedMetalDevice device)
    : impl_(std::make_unique<Impl>(device))
{
}

CVMetalFrameInterop::~CVMetalFrameInterop() = default;
CVMetalFrameInterop::CVMetalFrameInterop(
    CVMetalFrameInterop&&) noexcept = default;
CVMetalFrameInterop& CVMetalFrameInterop::operator=(
    CVMetalFrameInterop&&) noexcept = default;

HardwareInteropCapabilities CVMetalFrameInterop::capabilities() const
{
    HardwareInteropCapabilities result;
    if (impl_ && impl_->cache_) {
        result.sourceDevices = { HardwareDeviceType::VideoToolbox };
        result.targetDevice = HardwareDeviceType::Metal;
        result.zeroCopy = true;
    }
    return result;
}

bool CVMetalFrameInterop::supports(
    const HardwareFrame& frame) const noexcept
{
    return impl_ && impl_->cache_
        && supportedPixelBuffer(pixelBuffer(frame));
}

std::shared_ptr<MetalTextureFrame> CVMetalFrameInterop::importFrame(
    const HardwareFrame& frame)
{
    if (!impl_ || !impl_->cache_) {
        return {};
    }

    CVPixelBufferRef buffer = pixelBuffer(frame);
    CVMetalFormat format;
    if (!supportedPixelBuffer(buffer, &format)) {
        return {};
    }

    std::array<CVMetalTextureRef, 2> textures {};
    const std::array<MTLPixelFormat, 2> pixelFormats {
        format.luma,
        format.chroma,
    };
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        for (std::size_t plane = 0; plane < textures.size(); ++plane) {
            const CVReturn status = CVMetalTextureCacheCreateTextureFromImage(
                kCFAllocatorDefault,
                impl_->cache_,
                buffer,
                nullptr,
                pixelFormats[plane],
                CVPixelBufferGetWidthOfPlane(buffer, plane),
                CVPixelBufferGetHeightOfPlane(buffer, plane),
                plane,
                &textures[plane]);
            if (status != kCVReturnSuccess || !textures[plane]
                || !CVMetalTextureGetTexture(textures[plane])) {
                for (auto texture : textures) {
                    if (texture) {
                        CFRelease(texture);
                    }
                }
                return {};
            }
        }
    }

    try {
        return std::make_shared<CVMetalTextureFrame>(
            frame,
            format.format,
            format.range,
            textures[0],
            textures[1]);
    } catch (...) {
        for (auto texture : textures) {
            CFRelease(texture);
        }
        throw;
    }
}

BorrowedMetalDevice CVMetalFrameInterop::device() const noexcept
{
    return impl_ ? impl_->device_ : BorrowedMetalDevice {};
}

void CVMetalFrameInterop::flush() noexcept
{
    if (!impl_ || !impl_->cache_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    CVMetalTextureCacheFlush(impl_->cache_, 0);
}

} // namespace qtav
