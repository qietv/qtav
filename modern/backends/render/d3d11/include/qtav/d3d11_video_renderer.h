// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(_WIN32)
#  error "qtav/d3d11_video_renderer.h is available only on Windows"
#endif

#include <functional>
#include <memory>

#include <qtav/d3d11_export.h>
#include <qtav/d3d11_device_access.h>
#include <qtav/video_render_api.h>

namespace qtav {

// The view remains application-owned and must stay valid for the render()
// call. The renderer obtains the current view on every call, so swap-chain
// resize and other surface recreation do not require rebuilding the renderer.
struct QTAV_RENDER_D3D11_EXPORT D3D11RenderTarget {
    ID3D11RenderTargetView* view = nullptr;

    bool isValid() const noexcept;
};

using D3D11CurrentTargetCallback = std::function<D3D11RenderTarget()>;

// A backend-specific, reference-counted view of a shader-readable D3D11
// texture imported from a hardware video frame. The returned native objects
// are borrowed and remain valid while this object is alive.
class QTAV_RENDER_D3D11_EXPORT D3D11TextureFrame {
public:
    virtual ~D3D11TextureFrame();

    virtual int width() const noexcept = 0;
    virtual int height() const noexcept = 0;
    virtual PixelFormat format() const noexcept = 0;
    virtual ID3D11Texture2D* texture() const noexcept = 0;
    virtual ID3D11ShaderResourceView*
    shaderResourceView() const noexcept = 0;
};

// Implemented by an optional platform interop target. Import must not map or
// copy the frame through CPU memory. The interop must use the returned device
// access and its context guard for immediate/video-context operations.
class QTAV_RENDER_D3D11_EXPORT D3D11HardwareFrameInterop {
public:
    virtual ~D3D11HardwareFrameInterop();

    virtual std::shared_ptr<D3D11DeviceAccess>
    deviceAccess() const noexcept = 0;
    virtual HardwareInteropCapabilities capabilities() const = 0;
    virtual bool supports(const HardwareFrame& frame) const noexcept = 0;
    virtual std::shared_ptr<D3D11TextureFrame> importFrame(
        const HardwareFrame& frame) = 0;
};

class QTAV_RENDER_D3D11_EXPORT D3D11VideoRenderer final
    : public VideoRenderAPI {
public:
    D3D11VideoRenderer(
        BorrowedD3D11Device device,
        BorrowedD3D11DeviceContext context,
        D3D11CurrentTargetCallback currentTarget);
    D3D11VideoRenderer(
        std::shared_ptr<D3D11DeviceAccess> deviceAccess,
        D3D11CurrentTargetCallback currentTarget);
    ~D3D11VideoRenderer() override;

    D3D11VideoRenderer(D3D11VideoRenderer&&) noexcept;
    D3D11VideoRenderer& operator=(D3D11VideoRenderer&&) noexcept;
    D3D11VideoRenderer(const D3D11VideoRenderer&) = delete;
    D3D11VideoRenderer& operator=(const D3D11VideoRenderer&) = delete;

    VideoRenderCapabilities capabilities() const override;
    void setEventCallback(EventCallback callback) override;
    bool open(const VideoRenderConfig& config) override;
    bool configure(const VideoRenderConfig& config) override;
    bool render(const VideoFrame& frame) override;
    void close() noexcept override;

    BorrowedD3D11Device device() const noexcept;
    BorrowedD3D11DeviceContext context() const noexcept;
    std::shared_ptr<D3D11DeviceAccess> deviceAccess() const noexcept;
    void setCurrentTargetCallback(D3D11CurrentTargetCallback callback);
    void setHardwareFrameInterop(
        std::shared_ptr<D3D11HardwareFrameInterop> hardwareInterop);
    std::shared_ptr<D3D11HardwareFrameInterop>
    hardwareFrameInterop() const noexcept;
    void setAllowSoftwareMappingFallback(bool allow) noexcept;
    bool allowSoftwareMappingFallback() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
