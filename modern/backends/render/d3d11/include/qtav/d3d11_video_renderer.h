// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(_WIN32)
#  error "qtav/d3d11_video_renderer.h is available only on Windows"
#endif

#include <dxgi1_4.h>

#include <cstdint>
#include <functional>
#include <memory>

#include <qtav/d3d11_export.h>
#include <qtav/d3d11_device_access.h>
#include <qtav/d3d11_statistics.h>
#include <qtav/video_render_api.h>

namespace qtav {

enum class D3D11OutputColorSpace {
    SDR,
    ScRGB,
    HDR10,
};

// Snapshot of the output selected for the most recent render. The renderer
// refreshes this information on every frame when a swap chain is supplied, so
// moving a window between displays or changing the Windows HDR setting is
// observed without rebuilding the renderer.
struct QTAV_RENDER_D3D11_EXPORT D3D11AdvancedColorInfo {
    D3D11OutputColorSpace outputColorSpace =
        D3D11OutputColorSpace::SDR;
    DXGI_COLOR_SPACE_TYPE swapChainColorSpace =
        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    DXGI_COLOR_SPACE_TYPE displayColorSpace =
        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    HMONITOR monitor = nullptr;
    int bitsPerColor = 8;
    float sdrWhiteLevelNits = 80.0F;
    float minimumLuminanceNits = 0.0F;
    float maximumLuminanceNits = 80.0F;
    float maximumFullFrameLuminanceNits = 80.0F;
    bool displayDetected = false;
    bool advancedColorActive = false;
    bool swapChainColorSpaceConfigured = false;
    bool sdrWhiteLevelFromSystem = false;

    bool isHdrOutput() const noexcept;
};

struct QTAV_RENDER_D3D11_EXPORT D3D11VideoRendererStatistics {
    // Counters and coarse per-stage maxima accumulated since the previous
    // takeStatistics() call.
    std::uint64_t decoderSurfaceCopies = 0;
    // Mutually exclusive reasons for render() returning false because a
    // transient renderer resource was busy.
    std::uint64_t stateBusyRenderAttempts = 0;
    std::uint64_t serializationBusyRenderAttempts = 0;
    std::uint64_t deviceContextBusyRenderAttempts = 0;
    // These two counters partition deviceContextBusyRenderAttempts by the
    // current immediate-context owner's acquisition policy.
    std::uint64_t reservationAwareContextBusyRenderAttempts = 0;
    std::uint64_t unreservedContextBusyRenderAttempts = 0;
    std::uint64_t inFlightBusyRenderAttempts = 0;
    std::int64_t maximumColorSetupMicroseconds = 0;
    std::int64_t maximumInteropMicroseconds = 0;
    std::int64_t maximumBufferUpdateMicroseconds = 0;
    std::int64_t maximumDrawMicroseconds = 0;
};

// The view and optional swap chain remain application-owned and must stay
// valid for the render() call. Supplying the swap chain enables automatic
// IDXGIOutput6 capability discovery, SDR-white lookup, SetColorSpace1(), and
// display-switch handling. Composition swap chains do not implement
// GetContainingOutput(); set monitor to the native window's current monitor
// for that presentation path. The renderer obtains all objects on every call,
// so resize, display moves, and other surface recreation do not require
// rebuilding it.
struct QTAV_RENDER_D3D11_EXPORT D3D11RenderTarget {
    ID3D11RenderTargetView* view = nullptr;
    IDXGISwapChain3* swapChain = nullptr;
    HMONITOR monitor = nullptr;

    bool isValid() const noexcept;
};

using D3D11CurrentTargetCallback = std::function<D3D11RenderTarget()>;

// A backend-specific, reference-counted view of a D3D11 texture imported from
// a hardware video frame. NV12/P010 decoder surfaces are exposed in their raw
// form so libplacebo can sample their planes before Dolby Vision reshaping and
// color conversion. The returned native objects are borrowed and remain valid
// while this object is alive.
class QTAV_RENDER_D3D11_EXPORT D3D11TextureFrame {
public:
    virtual ~D3D11TextureFrame();

    virtual int width() const noexcept = 0;
    virtual int height() const noexcept = 0;
    virtual PixelFormat format() const noexcept = 0;
    virtual ID3D11Texture2D* texture() const noexcept = 0;
    virtual ID3D11ShaderResourceView*
    shaderResourceView() const noexcept = 0;
    // Array slice used when texture() is a decoder texture array.
    virtual UINT arraySlice() const noexcept;
    // Existing implementations default to an SDR BGRA/RGBA interpretation.
    // Raw decoder interop overrides these with NV12/P010 and source identity.
    virtual DXGI_FORMAT dxgiFormat() const noexcept;
    virtual DXGI_COLOR_SPACE_TYPE colorSpace() const noexcept;
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
    // Color-aware import used by the renderer. Existing interop
    // implementations remain source-compatible through the default
    // forwarding implementation.
    virtual std::shared_ptr<D3D11TextureFrame> importFrame(
        const HardwareFrame& frame,
        const VideoColorSpace& color);
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
    VideoRenderAttemptResult renderDetailed(
        const VideoFrame& frame) override;
    void close() noexcept override;

    // Completes submitted rendering and drains the background release of
    // borrowed targets and decoder slices. Call before resizing or replacing
    // a target.
    void flush() noexcept;

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
    // Bypasses the default decoder-to-shader GPU copy and samples compatible
    // shader-bound NV12/P010 decoder texture-array slices directly. This is
    // an explicitly unsafe compatibility/performance option: D3D11 does not
    // guarantee decoder-surface sampling, and drivers may expose padding,
    // throughput, seek, or resource-lifetime failures. Keep disabled unless
    // the exact adapter, driver, codec, seek, and shutdown matrix is accepted.
    void setDirectDecoderTextureSamplingEnabled(bool enabled) noexcept;
    bool directDecoderTextureSamplingEnabled() const noexcept;
    // Counters is the low-cost default. Timing additionally enables per-frame
    // steady-clock sampling for the coarse render stages.
    void setStatisticsMode(D3D11StatisticsMode mode) noexcept;
    D3D11StatisticsMode statisticsMode() const noexcept;
    D3D11AdvancedColorInfo advancedColorInfo() const noexcept;
    // Atomically returns and resets the accumulated render-stage maxima.
    D3D11VideoRendererStatistics takeStatistics() noexcept;

private:
    bool renderFrame(
        const VideoFrame& frame,
        VideoRenderRetryReason* retryReason);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
