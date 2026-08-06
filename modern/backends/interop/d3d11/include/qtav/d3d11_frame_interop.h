// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(_WIN32)
#  error "qtav/d3d11_frame_interop.h is available only on Windows"
#endif

#include <memory>

#include <qtav/d3d11_interop_export.h>
#include <qtav/d3d11_video_renderer.h>

namespace qtav {

// Retains same-device D3D11VA NV12/P010 decoder slices in raw form. The D3D11
// renderer copies each slice into its bounded same-format GPU texture ring
// before libplacebo sampling; no Video Processor RGB conversion or
// decoded-pixel CPU mapping occurs before Dolby Vision/HDR processing.
class QTAV_INTEROP_D3D11_EXPORT D3D11FrameInterop final
    : public D3D11HardwareFrameInterop {
public:
    explicit D3D11FrameInterop(
        std::shared_ptr<D3D11DeviceAccess> deviceAccess);
    ~D3D11FrameInterop() override;

    D3D11FrameInterop(D3D11FrameInterop&&) noexcept;
    D3D11FrameInterop& operator=(D3D11FrameInterop&&) noexcept;
    D3D11FrameInterop(const D3D11FrameInterop&) = delete;
    D3D11FrameInterop& operator=(const D3D11FrameInterop&) = delete;

    std::shared_ptr<D3D11DeviceAccess>
    deviceAccess() const noexcept override;
    HardwareInteropCapabilities capabilities() const override;
    bool supports(const HardwareFrame& frame) const noexcept override;
    std::shared_ptr<D3D11TextureFrame> importFrame(
        const HardwareFrame& frame) override;
    std::shared_ptr<D3D11TextureFrame> importFrame(
        const HardwareFrame& frame,
        const VideoColorSpace& color) override;

    void flush() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
