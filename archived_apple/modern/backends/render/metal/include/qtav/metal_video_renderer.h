// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(__APPLE__) || !defined(__OBJC__)
#  error "qtav/metal_video_renderer.h must be compiled as Objective-C++ on Apple platforms"
#endif

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <TargetConditionals.h>

#if TARGET_OS_OSX
@class NSScreen;
#else
@class UIScreen;
#endif

#include <functional>
#include <memory>

#include <qtav/metal_export.h>
#include <qtav/video_render_api.h>

namespace qtav {

#if TARGET_OS_OSX
using MetalEDRDisplay = NSScreen*;
#else
using MetalEDRDisplay = UIScreen*;
#endif

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
    // Linear BT.2020 primaries in an RGBA16Float target. A value of 1.0
    // represents referenceWhiteNits and HDR highlights may exceed 1.0.
    // This is the preferred output for HDR10/HLG Apple EDR presentation.
    ExtendedLinearBT2020,
};

enum class MetalEDRToneMapping {
    // Attach CAEDRMetadata to a CAMetalLayer and let the display system adapt
    // the extended-linear values to the active display.
    System,
    // Tone-map extended-linear values in the shader to the target display's
    // current EDR headroom. The headroom is sampled for every render call.
    DisplayAdaptive,
    // Preserve extended-linear values without tone mapping. Intended for
    // reference displays, offline processing, and deterministic readback.
    None,
};

// The callback and all native objects returned by it remain application-owned.
// They must stay valid for the render() call. A committed Metal command buffer
// retains the resources it uses until GPU execution finishes.
struct QTAV_RENDER_METAL_EXPORT MetalRenderTarget {
    __unsafe_unretained id<MTLTexture> texture = nil;
    __unsafe_unretained id<CAMetalDrawable> drawable = nil;
    // Prefer returning a layer for onscreen EDR. The renderer configures the
    // layer, including EDR metadata, before obtaining its next drawable.
    __unsafe_unretained CAMetalLayer* layer = nil;
    // NSScreen on macOS or UIScreen on iOS. Used only for DisplayAdaptive
    // tone mapping and sampled on every render call.
    __unsafe_unretained MetalEDRDisplay display = nil;
    MetalOutputColorSpace outputColorSpace = MetalOutputColorSpace::SDR;
    MetalEDRToneMapping edrToneMapping = MetalEDRToneMapping::System;
    float referenceWhiteNits = 100.0F;
    // A positive value overrides display-derived headroom. Zero requests a
    // real-time NSScreen/UIScreen query and falls back to 1.0 when unavailable.
    float currentEDRHeadroom = 0.0F;
    // Intended for deterministic offscreen readback and diagnostics.
    bool waitUntilCompleted = false;

    bool isValid() const noexcept;
};

using MetalCurrentTargetCallback = std::function<MetalRenderTarget()>;

// These helpers accept NSScreen on macOS and UIScreen on iOS. They return 1.0
// for nil, an unsupported OS, or a non-EDR display.
QTAV_RENDER_METAL_EXPORT float
metalCurrentEDRHeadroom(MetalEDRDisplay display) noexcept;
QTAV_RENDER_METAL_EXPORT float
metalPotentialEDRHeadroom(MetalEDRDisplay display) noexcept;

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
