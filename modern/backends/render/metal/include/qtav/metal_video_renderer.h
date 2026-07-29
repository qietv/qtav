// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(__APPLE__) || !defined(__OBJC__)
#  error "qtav/metal_video_renderer.h must be compiled as Objective-C++ on Apple platforms"
#endif

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <functional>
#include <memory>

#include <qtav/metal_export.h>
#include <qtav/video_render_api.h>

namespace qtav {

class QTAV_RENDER_METAL_EXPORT BorrowedMetalDevice final {
public:
    explicit BorrowedMetalDevice(id<MTLDevice> value = nil) noexcept;

    id<MTLDevice> get() const noexcept;
    explicit operator bool() const noexcept;

private:
    __unsafe_unretained id<MTLDevice> value_ = nil;
};

class QTAV_RENDER_METAL_EXPORT BorrowedMetalCommandQueue final {
public:
    explicit BorrowedMetalCommandQueue(
        id<MTLCommandQueue> value = nil) noexcept;

    id<MTLCommandQueue> get() const noexcept;
    explicit operator bool() const noexcept;

private:
    __unsafe_unretained id<MTLCommandQueue> value_ = nil;
};

enum class MetalOutputColorSpace {
    // Preserve the source's SDR transfer for ordinary 8-bit targets. HDR
    // sources are mapped into the SDR target range.
    SDR,
    // Linear BT.709/sRGB primaries in an RGBA16Float target. A value of 1.0
    // represents referenceWhiteNits and HDR highlights may exceed 1.0.
    ExtendedLinearSRGB,
};

// The callback and all native objects returned by it remain application-owned.
// They must stay valid for the render() call. A committed Metal command buffer
// retains the resources it uses until GPU execution finishes.
struct QTAV_RENDER_METAL_EXPORT MetalRenderTarget {
    __unsafe_unretained id<MTLTexture> texture = nil;
    __unsafe_unretained id<CAMetalDrawable> drawable = nil;
    MetalOutputColorSpace outputColorSpace = MetalOutputColorSpace::SDR;
    float referenceWhiteNits = 100.0F;
    // Intended for deterministic offscreen readback and diagnostics.
    bool waitUntilCompleted = false;

    bool isValid() const noexcept;
};

using MetalCurrentTargetCallback = std::function<MetalRenderTarget()>;

// A backend-specific, reference-counted view of native Metal textures imported
// from a hardware video frame. Texture objects remain valid while this object
// is alive.
class QTAV_RENDER_METAL_EXPORT MetalTextureFrame {
public:
    virtual ~MetalTextureFrame();

    virtual int width() const noexcept = 0;
    virtual int height() const noexcept = 0;
    virtual PixelFormat format() const noexcept = 0;
    virtual ColorRange colorRange() const noexcept = 0;
    virtual int planeCount() const noexcept = 0;
    virtual id<MTLTexture> texture(int plane) const noexcept = 0;
};

// Implemented by an optional platform interop target. Import must not map or
// copy the frame through CPU memory.
class QTAV_RENDER_METAL_EXPORT MetalHardwareFrameInterop {
public:
    virtual ~MetalHardwareFrameInterop();

    virtual BorrowedMetalDevice device() const noexcept = 0;
    virtual HardwareInteropCapabilities capabilities() const = 0;
    virtual bool supports(const HardwareFrame& frame) const noexcept = 0;
    virtual std::shared_ptr<MetalTextureFrame> importFrame(
        const HardwareFrame& frame) = 0;
};

class QTAV_RENDER_METAL_EXPORT MetalVideoRenderer final
    : public VideoRenderAPI {
public:
    MetalVideoRenderer(
        BorrowedMetalDevice device,
        BorrowedMetalCommandQueue commandQueue,
        MetalCurrentTargetCallback currentTarget,
        std::shared_ptr<MetalHardwareFrameInterop> hardwareInterop = {});
    ~MetalVideoRenderer() override;

    MetalVideoRenderer(MetalVideoRenderer&&) noexcept;
    MetalVideoRenderer& operator=(MetalVideoRenderer&&) noexcept;
    MetalVideoRenderer(const MetalVideoRenderer&) = delete;
    MetalVideoRenderer& operator=(const MetalVideoRenderer&) = delete;

    VideoRenderCapabilities capabilities() const override;
    void setEventCallback(EventCallback callback) override;
    bool open(const VideoRenderConfig& config) override;
    bool configure(const VideoRenderConfig& config) override;
    bool render(const VideoFrame& frame) override;
    void close() noexcept override;

    BorrowedMetalDevice device() const noexcept;
    BorrowedMetalCommandQueue commandQueue() const noexcept;
    void setCurrentTargetCallback(MetalCurrentTargetCallback callback);
    void setHardwareFrameInterop(
        std::shared_ptr<MetalHardwareFrameInterop> hardwareInterop);
    std::shared_ptr<MetalHardwareFrameInterop>
    hardwareFrameInterop() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
