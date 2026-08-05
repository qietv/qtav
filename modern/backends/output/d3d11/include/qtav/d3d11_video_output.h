// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(_WIN32)
#  error "qtav/d3d11_video_output.h is available only on Windows"
#endif

#if !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <dxgi1_4.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <qtav/output_d3d11_export.h>
#include <qtav/video_render_api.h>

namespace qtav {

class D3D11DeviceAccess;
class Player;

using D3D11SwapChainBinding =
    std::function<HRESULT(IDXGISwapChain1*)>;
using D3D11CurrentMonitorCallback = std::function<HMONITOR()>;

struct QTAV_OUTPUT_D3D11_EXPORT D3D11CompositionSurface {
    VideoSize size;
    float compositionScaleX = 1.0F;
    float compositionScaleY = 1.0F;
    D3D11SwapChainBinding bindSwapChain;
    // Set the desktop window hosting this composition surface to enable
    // automatic native HDR/display tracking. A composition swap chain has no
    // HWND and IDXGISwapChain::GetContainingOutput() is unsupported.
    HWND window = nullptr;
    // Optional override for hosts without a desktop HWND. Return the current
    // monitor on every call so display moves and Windows HDR-setting changes
    // are observed without reopening.
    D3D11CurrentMonitorCallback currentMonitor;

    bool isValid() const noexcept;
};

enum class D3D11OutputPreference {
    PreferHdr,
    RequireHdr,
    SdrOnly,
};

enum class D3D11HdrPresentationMode {
    ScRGB,
    HDR10,
};

struct QTAV_OUTPUT_D3D11_EXPORT D3D11VideoOutputOptions {
    VideoAspectRatioMode aspectRatio = VideoAspectRatioMode::Fit;
    D3D11OutputPreference outputPreference =
        D3D11OutputPreference::PreferHdr;
    // FP16 scRGB is the general-purpose Advanced Color path. Opaque video
    // surfaces may select RGB10/PQ to avoid the DWM scRGB conversion and
    // present HDR10-compatible content in the display's native transfer.
    D3D11HdrPresentationMode hdrPresentationMode =
        D3D11HdrPresentationMode::ScRGB;
    DXGI_ALPHA_MODE alphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    UINT bufferCount = 2;
    bool forceWarp = false;
    bool allowWarpFallback = true;
    bool allowSoftwareMappingFallback = true;
    bool configureHardwareDecoding = true;
};

enum class D3D11PresentationColorSpace {
    SDR,
    ScRGB,
    HDR10,
};

struct QTAV_OUTPUT_D3D11_EXPORT D3D11VideoOutputColorInfo {
    D3D11OutputPreference preference =
        D3D11OutputPreference::PreferHdr;
    D3D11PresentationColorSpace colorSpace =
        D3D11PresentationColorSpace::SDR;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
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

enum class D3D11VideoOutputEventType {
    SurfaceLost,
    Error,
};

struct QTAV_OUTPUT_D3D11_EXPORT D3D11VideoOutputEvent {
    D3D11VideoOutputEventType type = D3D11VideoOutputEventType::Error;
    std::string detail;
    HRESULT code = S_OK;
};

struct QTAV_OUTPUT_D3D11_EXPORT D3D11VideoOutputStatistics {
    // Counters and maxima accumulated since the previous takeStatistics().
    std::uint64_t renderRequests = 0;
    std::uint64_t coalescedRenderRequests = 0;
    std::uint64_t renderPasses = 0;
    std::uint64_t presentedFrames = 0;
    std::uint64_t longRenderGaps = 0;
    std::int64_t maximumRenderGapMicroseconds = 0;
    std::int64_t maximumRenderMicroseconds = 0;
    std::int64_t maximumPresentMicroseconds = 0;
    std::uint64_t busyPresents = 0;
    // Compatibility mirror of terminalRenderDrops. Recovered retry attempts
    // are not skipped renders.
    std::uint64_t skippedRenders = 0;
    // Detailed outcomes accumulated across render-thread attempts.
    std::uint64_t noFrameRenderAttempts = 0;
    std::uint64_t playerBusyRenderAttempts = 0;
    std::uint64_t rendererBusyRenderAttempts = 0;
    std::uint64_t retryWakeups = 0;
    // A superseded frame is also one terminal drop. terminalRenderDrops may
    // additionally contain non-retryable backend failures.
    std::uint64_t supersededRenderFrames = 0;
    std::uint64_t terminalRenderDrops = 0;
    std::uint64_t rendererStateBusyRenderAttempts = 0;
    std::uint64_t rendererSerializationBusyRenderAttempts = 0;
    std::uint64_t rendererDeviceContextBusyRenderAttempts = 0;
    std::uint64_t rendererReservationAwareContextBusyRenderAttempts = 0;
    std::uint64_t rendererUnreservedContextBusyRenderAttempts = 0;
    // Contention intercepted by the proactive bounded acquisition, and the
    // subset that exhausted its handoff interval before renderer entry.
    std::uint64_t contextHandoffWaits = 0;
    std::uint64_t contextHandoffTimeouts = 0;
    std::uint64_t rendererInFlightBusyRenderAttempts = 0;
    std::uint64_t decoderSurfaceCopies = 0;
    std::int64_t maximumColorSetupMicroseconds = 0;
    std::int64_t maximumInteropMicroseconds = 0;
    std::int64_t maximumBufferUpdateMicroseconds = 0;
    std::int64_t maximumDrawMicroseconds = 0;
};

// High-level composition-surface output for ordinary Windows playback.
//
// The output owns the D3D11 device, composition swap chain, render target,
// D3D11VA/libplacebo raw-plane path, redraw-coalescing render thread, Present(),
// resize, HDR/SDR presentation, and teardown. The hosting HWND and surface
// binding callback are the only ordinary UI-toolkit bridge; for a WinUI
// SwapChainPanel the callback normally calls
// ISwapChainPanelNative::SetSwapChain().
//
// attach() exclusively owns the Player render callback/default render slot and
// optionally its hardware decode configuration until detach(). The connection
// must be detached before the Player or native surface is destroyed.
// open(), resize(), close(), attach(), and detach() must be serialized by the
// caller. Event and frame-presented callbacks may run on the internal render
// thread; they must not destroy or detach the output from inside the callback.
class QTAV_OUTPUT_D3D11_EXPORT D3D11VideoOutput final {
public:
    using EventCallback =
        std::function<void(const D3D11VideoOutputEvent&)>;
    using FramePresentedCallback = std::function<void(double)>;

    D3D11VideoOutput();
    ~D3D11VideoOutput();

    D3D11VideoOutput(D3D11VideoOutput&&) noexcept;
    D3D11VideoOutput& operator=(D3D11VideoOutput&&) noexcept;
    D3D11VideoOutput(const D3D11VideoOutput&) = delete;
    D3D11VideoOutput& operator=(const D3D11VideoOutput&) = delete;

    D3D11VideoOutput& setEventCallback(EventCallback callback);
    D3D11VideoOutput& setFramePresentedCallback(
        FramePresentedCallback callback);

    bool open(
        D3D11CompositionSurface surface,
        D3D11VideoOutputOptions options = {});
    void close() noexcept;

    bool attach(Player& player);
    void detach() noexcept;
    bool resize(
        VideoSize size,
        float compositionScaleX = 1.0F,
        float compositionScaleY = 1.0F);
    void requestRender() noexcept;

    bool isOpen() const noexcept;
    bool isAttached() const noexcept;
    std::string lastError() const;
    std::string deviceDescription() const;
    D3D11VideoOutputColorInfo colorInfo() const noexcept;
    std::shared_ptr<D3D11DeviceAccess> deviceAccess() const noexcept;
    // Atomically returns and resets the accumulated render/present statistics.
    D3D11VideoOutputStatistics takeStatistics() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
