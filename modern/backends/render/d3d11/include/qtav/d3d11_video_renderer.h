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

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
