// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(__APPLE__) || !defined(__OBJC__)
#  error "qtav/cvmetal_frame_interop.h must be compiled as Objective-C++ on Apple platforms"
#endif

#include <memory>

#include <qtav/cvmetal_export.h>
#include <qtav/metal_video_renderer.h>

namespace qtav {

// Imports VideoToolbox CVPixelBuffer planes through CVMetalTextureCache.
// The borrowed Metal device must outlive this object.
class QTAV_INTEROP_CVMETAL_EXPORT CVMetalFrameInterop final
    : public MetalHardwareFrameInterop {
public:
    explicit CVMetalFrameInterop(BorrowedMetalDevice device);
    ~CVMetalFrameInterop() override;

    CVMetalFrameInterop(CVMetalFrameInterop&&) noexcept;
    CVMetalFrameInterop& operator=(CVMetalFrameInterop&&) noexcept;
    CVMetalFrameInterop(const CVMetalFrameInterop&) = delete;
    CVMetalFrameInterop& operator=(const CVMetalFrameInterop&) = delete;

    BorrowedMetalDevice device() const noexcept override;
    HardwareInteropCapabilities capabilities() const override;
    bool supports(const HardwareFrame& frame) const noexcept override;
    std::shared_ptr<MetalTextureFrame> importFrame(
        const HardwareFrame& frame) override;

    void flush() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
