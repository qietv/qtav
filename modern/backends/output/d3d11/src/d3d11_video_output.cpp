// SPDX-License-Identifier: LGPL-2.1-or-later
#include <qtav/d3d11_video_output.h>

#include <qtav/d3d11_device_access.h>
#include <qtav/d3d11_frame_interop.h>
#include <qtav/d3d11_video_renderer.h>
#include <qtav/d3d11va_hardware_decoder.h>
#include <qtav/player.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace qtav {

namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

std::int64_t steadyMicroseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch())
        .count();
}

constexpr DWORD frameLatencyWaitMilliseconds = 20;

void updateMaximum(
    std::atomic<std::int64_t>& destination,
    std::int64_t value) noexcept
{
    auto current = destination.load(std::memory_order_relaxed);
    while (value > current
           && !destination.compare_exchange_weak(
               current,
               value,
               std::memory_order_relaxed)) {
    }
}

std::string hresultDetail(const char* detail, HRESULT code)
{
    std::ostringstream message;
    message << detail << " (HRESULT 0x" << std::hex << std::uppercase
            << static_cast<unsigned long>(code) << ')';
    return message.str();
}

HRESULT createDevice(
    D3D_DRIVER_TYPE driverType,
    UINT flags,
    ComPtr<ID3D11Device>& device,
    ComPtr<ID3D11DeviceContext>& context)
{
    const D3D_FEATURE_LEVEL levels[] {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL selected {};
    HRESULT status = D3D11CreateDevice(
        nullptr,
        driverType,
        nullptr,
        flags,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &device,
        &selected,
        &context);
    if (status == E_INVALIDARG) {
        device.Reset();
        context.Reset();
        status = D3D11CreateDevice(
            nullptr,
            driverType,
            nullptr,
            flags,
            levels + 1,
            1,
            D3D11_SDK_VERSION,
            &device,
            &selected,
            &context);
    }
    return status;
}

D3D11PresentationColorSpace presentationColorSpace(
    D3D11OutputColorSpace colorSpace) noexcept
{
    switch (colorSpace) {
    case D3D11OutputColorSpace::ScRGB:
        return D3D11PresentationColorSpace::ScRGB;
    case D3D11OutputColorSpace::HDR10:
        return D3D11PresentationColorSpace::HDR10;
    case D3D11OutputColorSpace::SDR:
        return D3D11PresentationColorSpace::SDR;
    }
    return D3D11PresentationColorSpace::SDR;
}

} // namespace

bool D3D11CompositionSurface::isValid() const noexcept
{
    return size.isValid() && compositionScaleX > 0.0F
        && compositionScaleY > 0.0F
        && static_cast<bool>(bindSwapChain);
}

bool D3D11VideoOutputColorInfo::isHdrOutput() const noexcept
{
    return advancedColorActive
        && swapChainColorSpaceConfigured
        && colorSpace != D3D11PresentationColorSpace::SDR;
}

class D3D11VideoOutput::Impl {
public:
    struct RenderState {
        std::mutex mutex;
        std::condition_variable changed;
        bool requested = false;
        bool stopping = false;
        Player* player = nullptr;
        std::uint64_t generation = 0;
        std::atomic<std::uint64_t> renderRequests { 0 };
        std::atomic<std::uint64_t> coalescedRenderRequests { 0 };
        std::atomic<std::uint64_t> renderPasses { 0 };
        std::atomic<std::uint64_t> presentedFrames { 0 };
        std::atomic<std::uint64_t> busyPresents { 0 };
        std::atomic<std::uint64_t> skippedRenders { 0 };
        std::atomic<std::uint64_t> longRenderGaps { 0 };
        std::atomic<std::int64_t> previousRenderMicroseconds { 0 };
        std::atomic<std::int64_t> maximumRenderGapMicroseconds { 0 };
        std::atomic<std::int64_t> maximumRenderMicroseconds { 0 };
        std::atomic<std::int64_t> maximumPresentMicroseconds { 0 };
        std::atomic<std::int64_t> maximumColorSetupMicroseconds { 0 };
        std::atomic<std::int64_t> maximumInteropMicroseconds { 0 };
        std::atomic<std::int64_t> maximumBufferUpdateMicroseconds { 0 };
        std::atomic<std::int64_t> maximumDrawMicroseconds { 0 };
    };

    ~Impl()
    {
        close();
    }

    void setEventCallback(EventCallback callback)
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        eventCallback_ = std::move(callback);
    }

    void setFramePresentedCallback(FramePresentedCallback callback)
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        framePresentedCallback_ = std::move(callback);
    }

    bool open(
        D3D11CompositionSurface surface,
        D3D11VideoOutputOptions options)
    {
        close();
        if (!surface.isValid()) {
            setError(
                "The D3D11 composition surface is invalid",
                E_INVALIDARG);
            return false;
        }
        if (options.bufferCount < 2) {
            setError(
                "A flip-model composition swap chain needs at least "
                "two buffers",
                E_INVALIDARG);
            return false;
        }
        if (options.outputPreference
                == D3D11OutputPreference::RequireHdr
            && !surface.window
            && !surface.currentMonitor) {
            setError(
                "RequireHdr needs the composition surface's HWND "
                "or current-monitor callback",
                E_INVALIDARG);
            return false;
        }

        surface_ = std::move(surface);
        options_ = options;
        advancedColorEnabled_ =
            options_.outputPreference
                != D3D11OutputPreference::SdrOnly
            && (surface_.window
                || static_cast<bool>(surface_.currentMonitor));
        selectedFormat_ = advancedColorEnabled_
            ? DXGI_FORMAT_R16G16B16A16_FLOAT
            : DXGI_FORMAT_B8G8R8A8_UNORM;
        hdrRequirementFailed_.store(
            false,
            std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            lastError_.clear();
        }

        HRESULT status = initializeDevice();
        if (FAILED(status)) {
            setError("D3D11 device creation failed", status);
            releaseGraphics(false);
            return false;
        }

        status = initializeSwapChain();
        if (FAILED(status)) {
            setError("Composition swap-chain creation failed", status);
            releaseGraphics(false);
            return false;
        }

        status = bindSurface(swapChain_.Get());
        if (FAILED(status)) {
            setError("The native surface rejected the swap chain", status);
            releaseGraphics(false);
            return false;
        }
        surfaceBound_ = true;

        status = createRenderTarget();
        if (FAILED(status)) {
            setError("D3D11 render-target creation failed", status);
            releaseGraphics(true);
            return false;
        }
        status = updateSwapChainTransform();
        if (FAILED(status)) {
            setError("Composition transform update failed", status);
            releaseGraphics(true);
            return false;
        }

        renderer_ = std::make_shared<D3D11VideoRenderer>(
            deviceAccess_,
            [this] {
                const HMONITOR monitor = currentMonitor();
                return D3D11RenderTarget {
                    renderTargetView_.Get(),
                    advancedColorEnabled_
                        ? swapChain_.Get()
                        : nullptr,
                    monitor,
                };
            });
        renderer_->setEventCallback(
            [this](const VideoRenderEvent& event) {
                handleRendererEvent(event);
            });
        interop_ = std::make_shared<D3D11FrameInterop>(deviceAccess_);
        renderer_->setHardwareFrameInterop(interop_);
        renderer_->setAllowSoftwareMappingFallback(
            options_.allowSoftwareMappingFallback);

        VideoRenderConfig renderConfig;
        renderConfig.surfaceSize = surface_.size;
        renderConfig.aspectRatio = options_.aspectRatio;
        if (!renderer_->open(renderConfig)) {
            setError("The D3D11 renderer could not be opened", E_FAIL);
            releaseGraphics(true);
            return false;
        }

        renderState_ = std::make_shared<RenderState>();
        open_.store(true, std::memory_order_release);
        renderThread_ = std::thread([this, state = renderState_] {
            runRenderThread(std::move(state));
        });
        return true;
    }

    void close() noexcept
    {
        detach();
        stopRenderThread();
        releaseGraphics(true);
    }

    bool attach(Player& player)
    {
        if (!open_.load(std::memory_order_acquire)) {
            setError("The D3D11 video output is not open", E_UNEXPECTED);
            return false;
        }

        detach();

        std::shared_ptr<D3D11VideoRenderer> renderer;
        std::shared_ptr<D3D11DeviceAccess> access;
        std::shared_ptr<RenderState> state;
        {
            std::lock_guard<std::mutex> lock(graphicsMutex_);
            renderer = renderer_;
            access = deviceAccess_;
            state = renderState_;
        }
        if (!renderer || !access || !state) {
            setError(
                "The D3D11 video output has incomplete resources",
                E_UNEXPECTED);
            return false;
        }

        // attach() takes exclusive ownership of the Player render slot. Clear
        // a previous application's scheduling callback before changing the
        // render API so it cannot race this output's renderer.
        player.setRenderCallback({});

        HardwareDecodeConfig previousConfig;
        if (options_.configureHardwareDecoding) {
            previousConfig = player.hardwareDecodeConfig();
            player.setHardwareDecodeConfig(
                d3d11vaHardwareDecodeConfig(access));
        }
        player.setVideoRenderAPI(renderer);

        {
            std::lock_guard<std::mutex> lock(connectionMutex_);
            attachedPlayer_ = &player;
            previousHardwareConfig_ = previousConfig;
            restoreHardwareConfig_ =
                options_.configureHardwareDecoding;
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->player = &player;
            ++state->generation;
        }

        const std::weak_ptr<RenderState> weakState = state;
        player.setRenderCallback(
            [weakState](void*) {
                if (const auto locked = weakState.lock()) {
                    requestRender(locked);
                }
            });
        requestRender(state);
        return true;
    }

    void detach() noexcept
    {
        Player* player = nullptr;
        HardwareDecodeConfig previousConfig;
        bool restoreHardwareConfig = false;
        {
            std::lock_guard<std::mutex> lock(connectionMutex_);
            player = attachedPlayer_;
            previousConfig = previousHardwareConfig_;
            restoreHardwareConfig = restoreHardwareConfig_;
        }
        if (!player) {
            return;
        }

        player->setRenderCallback({});
        {
            std::lock_guard<std::mutex> graphicsLock(graphicsMutex_);
            if (renderState_) {
                std::lock_guard<std::mutex> stateLock(
                    renderState_->mutex);
                if (renderState_->player == player) {
                    renderState_->player = nullptr;
                    renderState_->requested = false;
                    ++renderState_->generation;
                }
            }
        }
        player->setVideoRenderAPI({});
        if (restoreHardwareConfig) {
            player->setHardwareDecodeConfig(previousConfig);
        }

        {
            std::lock_guard<std::mutex> lock(connectionMutex_);
            if (attachedPlayer_ == player) {
                attachedPlayer_ = nullptr;
                previousHardwareConfig_ = {};
                restoreHardwareConfig_ = false;
            }
        }
    }

    bool resize(
        VideoSize size,
        float compositionScaleX,
        float compositionScaleY)
    {
        if (!size.isValid() || !(compositionScaleX > 0.0F)
            || !(compositionScaleY > 0.0F)) {
            setError("The requested output size is invalid", E_INVALIDARG);
            return false;
        }

        HRESULT status = S_OK;
        {
            std::lock_guard<std::mutex> lock(graphicsMutex_);
            if (!open_.load(std::memory_order_acquire) || !swapChain_
                || !renderer_) {
                status = E_UNEXPECTED;
            } else if (
                surface_.size.width == size.width
                && surface_.size.height == size.height) {
                surface_.compositionScaleX = compositionScaleX;
                surface_.compositionScaleY = compositionScaleY;
                status = updateSwapChainTransform();
            } else {
                auto guard = deviceAccess_->contextGuard();
                ID3D11RenderTargetView* noTarget = nullptr;
                context_->OMSetRenderTargets(1, &noTarget, nullptr);
                context_->Flush();
                renderTargetView_.Reset();

                status = swapChain_->ResizeBuffers(
                    options_.bufferCount,
                    static_cast<UINT>(size.width),
                    static_cast<UINT>(size.height),
                    selectedFormat_,
                    frameLatencyWaitableObject_
                        ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
                        : 0);
                if (SUCCEEDED(status)) {
                    surface_.size = size;
                    surface_.compositionScaleX = compositionScaleX;
                    surface_.compositionScaleY = compositionScaleY;
                    status = createRenderTarget();
                }
                if (SUCCEEDED(status)) {
                    status = updateSwapChainTransform();
                }
                if (SUCCEEDED(status)) {
                    VideoRenderConfig config;
                    config.surfaceSize = size;
                    config.aspectRatio = options_.aspectRatio;
                    if (!renderer_->configure(config)) {
                        status = E_FAIL;
                    }
                }
            }
        }

        if (FAILED(status)) {
            setError("The D3D11 output could not be resized", status);
            return false;
        }
        requestRender();
        return true;
    }

    void requestRender() noexcept
    {
        requestRender(renderState_);
    }

    bool isOpen() const noexcept
    {
        return open_.load(std::memory_order_acquire);
    }

    bool isAttached() const noexcept
    {
        std::lock_guard<std::mutex> lock(connectionMutex_);
        return attachedPlayer_ != nullptr;
    }

    std::string lastError() const
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        return lastError_;
    }

    std::string deviceDescription() const
    {
        std::lock_guard<std::mutex> lock(graphicsMutex_);
        return deviceDescription_;
    }

    D3D11VideoOutputColorInfo colorInfo() const noexcept
    {
        D3D11VideoOutputColorInfo result;
        std::shared_ptr<D3D11VideoRenderer> renderer;
        {
            std::lock_guard<std::mutex> lock(graphicsMutex_);
            result.preference = options_.outputPreference;
            result.format = selectedFormat_;
            renderer = renderer_;
        }
        if (!renderer) {
            return result;
        }

        const auto rendererInfo = renderer->advancedColorInfo();
        result.colorSpace =
            presentationColorSpace(rendererInfo.outputColorSpace);
        result.swapChainColorSpace =
            rendererInfo.swapChainColorSpace;
        result.displayColorSpace =
            rendererInfo.displayColorSpace;
        result.monitor = rendererInfo.monitor;
        result.bitsPerColor = rendererInfo.bitsPerColor;
        result.sdrWhiteLevelNits =
            rendererInfo.sdrWhiteLevelNits;
        result.minimumLuminanceNits =
            rendererInfo.minimumLuminanceNits;
        result.maximumLuminanceNits =
            rendererInfo.maximumLuminanceNits;
        result.maximumFullFrameLuminanceNits =
            rendererInfo.maximumFullFrameLuminanceNits;
        result.displayDetected = rendererInfo.displayDetected;
        result.advancedColorActive =
            rendererInfo.advancedColorActive;
        result.swapChainColorSpaceConfigured =
            rendererInfo.swapChainColorSpaceConfigured;
        result.sdrWhiteLevelFromSystem =
            rendererInfo.sdrWhiteLevelFromSystem;
        return result;
    }

    std::shared_ptr<D3D11DeviceAccess> deviceAccess() const noexcept
    {
        std::lock_guard<std::mutex> lock(graphicsMutex_);
        return deviceAccess_;
    }

    D3D11VideoOutputStatistics takeStatistics() noexcept
    {
        D3D11VideoOutputStatistics result;
        const auto state = renderState_;
        if (!state) {
            return result;
        }
        result.renderRequests =
            state->renderRequests.exchange(0);
        result.coalescedRenderRequests =
            state->coalescedRenderRequests.exchange(0);
        result.renderPasses = state->renderPasses.exchange(0);
        result.presentedFrames = state->presentedFrames.exchange(0);
        result.busyPresents = state->busyPresents.exchange(0);
        result.skippedRenders =
            state->skippedRenders.exchange(0);
        result.longRenderGaps = state->longRenderGaps.exchange(0);
        result.maximumRenderGapMicroseconds =
            state->maximumRenderGapMicroseconds.exchange(0);
        result.maximumRenderMicroseconds =
            state->maximumRenderMicroseconds.exchange(0);
        result.maximumPresentMicroseconds =
            state->maximumPresentMicroseconds.exchange(0);
        result.maximumColorSetupMicroseconds =
            state->maximumColorSetupMicroseconds.exchange(0);
        result.maximumInteropMicroseconds =
            state->maximumInteropMicroseconds.exchange(0);
        result.maximumBufferUpdateMicroseconds =
            state->maximumBufferUpdateMicroseconds.exchange(0);
        result.maximumDrawMicroseconds =
            state->maximumDrawMicroseconds.exchange(0);
        return result;
    }

private:
    static void requestRender(
        const std::shared_ptr<RenderState>& state) noexcept
    {
        if (!state) {
            return;
        }
        state->renderRequests.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->stopping || !state->player) {
                return;
            }
            if (state->requested) {
                state->coalescedRenderRequests.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
            state->requested = true;
        }
        state->changed.notify_one();
    }

    HRESULT initializeDevice()
    {
        const UINT videoFlags =
            D3D11_CREATE_DEVICE_BGRA_SUPPORT
            | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        HRESULT status = E_FAIL;
        if (!options_.forceWarp) {
            status = createDevice(
                D3D_DRIVER_TYPE_HARDWARE,
                videoFlags,
                device_,
                context_);
            if (SUCCEEDED(status)) {
                deviceDescription_ =
                    "hardware D3D11 with video support";
            }
            if (FAILED(status)) {
                status = createDevice(
                    D3D_DRIVER_TYPE_HARDWARE,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                    device_,
                    context_);
                if (SUCCEEDED(status)) {
                    deviceDescription_ = "hardware D3D11";
                }
            }
        }
        if (FAILED(status)
            && (options_.forceWarp
                || options_.allowWarpFallback)) {
            status = createDevice(
                D3D_DRIVER_TYPE_WARP,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                device_,
                context_);
            if (SUCCEEDED(status)) {
                deviceDescription_ = "WARP D3D11";
            }
        }
        if (FAILED(status)) {
            return status;
        }

        deviceAccess_ = D3D11DeviceAccess::create(
            BorrowedD3D11Device(device_.Get()),
            BorrowedD3D11DeviceContext(context_.Get()));
        if (!deviceAccess_) {
            return E_FAIL;
        }
        return S_OK;
    }

    HRESULT initializeSwapChain()
    {
        ComPtr<IDXGIDevice1> dxgiDevice;
        HRESULT status = device_.As(&dxgiDevice);
        if (FAILED(status)) {
            return status;
        }
        const bool useFrameLatencyWaitableObject =
            !options_.forceWarp;
        if (!useFrameLatencyWaitableObject) {
            status = dxgiDevice->SetMaximumFrameLatency(1);
            if (FAILED(status)) {
                return status;
            }
        }

        ComPtr<IDXGIAdapter> adapter;
        status = dxgiDevice->GetAdapter(&adapter);
        if (FAILED(status)) {
            return status;
        }
        ComPtr<IDXGIFactory2> factory;
        status = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(status)) {
            return status;
        }

        DXGI_SWAP_CHAIN_DESC1 description {};
        description.Width = static_cast<UINT>(surface_.size.width);
        description.Height = static_cast<UINT>(surface_.size.height);
        description.Format = selectedFormat_;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = options_.bufferCount;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        description.AlphaMode = options_.alphaMode;
        description.Flags = useFrameLatencyWaitableObject
            ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
            : 0;

        ComPtr<IDXGISwapChain1> swapChain;
        status = factory->CreateSwapChainForComposition(
            device_.Get(),
            &description,
            nullptr,
            &swapChain);
        if (FAILED(status)) {
            return status;
        }
        status = swapChain.As(&swapChain_);
        if (FAILED(status)) {
            return status;
        }
        if (!useFrameLatencyWaitableObject) {
            return S_OK;
        }
        status = swapChain_->SetMaximumFrameLatency(1);
        if (FAILED(status)) {
            return status;
        }
        frameLatencyWaitableObject_ =
            swapChain_->GetFrameLatencyWaitableObject();
        return frameLatencyWaitableObject_ ? S_OK : E_FAIL;
    }

    HRESULT createRenderTarget()
    {
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT status = swapChain_->GetBuffer(
            0,
            IID_PPV_ARGS(&backBuffer));
        if (FAILED(status)) {
            return status;
        }
        return device_->CreateRenderTargetView(
            backBuffer.Get(),
            nullptr,
            &renderTargetView_);
    }

    HRESULT updateSwapChainTransform()
    {
        const DXGI_MATRIX_3X2_F transform {
            1.0F / surface_.compositionScaleX,
            0.0F,
            0.0F,
            1.0F / surface_.compositionScaleY,
            0.0F,
            0.0F,
        };
        return swapChain_->SetMatrixTransform(&transform);
    }

    HMONITOR currentMonitor() const noexcept
    {
        if (surface_.currentMonitor) {
            try {
                return surface_.currentMonitor();
            } catch (...) {
                return nullptr;
            }
        }
        if (surface_.window) {
            return MonitorFromWindow(
                surface_.window,
                MONITOR_DEFAULTTONEAREST);
        }
        return nullptr;
    }

    void handleRendererEvent(const VideoRenderEvent& event)
    {
        if (event.type == VideoRenderEventType::RedrawRequested) {
            requestRender();
            return;
        }
        const auto type =
            event.type == VideoRenderEventType::SurfaceLost
            ? D3D11VideoOutputEventType::SurfaceLost
            : D3D11VideoOutputEventType::Error;
        reportEvent({ type, event.detail, S_OK });
    }

    void runRenderThread(std::shared_ptr<RenderState> state)
    {
        bool waitForPresentationCapacity = false;
        while (true) {
            Player* player = nullptr;
            std::uint64_t generation = 0;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->changed.wait(
                    lock,
                    [&] { return state->requested || state->stopping; });
                if (state->stopping) {
                    return;
                }
                state->requested = false;
                player = state->player;
                generation = state->generation;
            }
            if (!player) {
                continue;
            }

            if (frameLatencyWaitableObject_
                && waitForPresentationCapacity) {
                const DWORD waitStatus = WaitForSingleObjectEx(
                    frameLatencyWaitableObject_,
                    frameLatencyWaitMilliseconds,
                    FALSE);
                if (waitStatus == WAIT_TIMEOUT) {
                    state->busyPresents.fetch_add(
                        1,
                        std::memory_order_relaxed);
                    requestRender(state);
                    continue;
                }
                if (waitStatus == WAIT_FAILED) {
                    setError(
                        "Waiting for D3D11 presentation capacity failed",
                        HRESULT_FROM_WIN32(GetLastError()));
                    continue;
                }
                waitForPresentationCapacity = false;
            }
            state->renderPasses.fetch_add(
                1,
                std::memory_order_relaxed);

            const auto renderStarted = steadyMicroseconds();
            double timestamp = -1.0;
            HRESULT presentStatus = S_OK;
            bool requiredHdrUnavailable = false;
            std::int64_t renderCompleted = renderStarted;
            {
                std::lock_guard<std::mutex> graphicsLock(graphicsMutex_);
                {
                    std::lock_guard<std::mutex> stateLock(state->mutex);
                    if (state->stopping || state->player != player
                        || state->generation != generation) {
                        continue;
                    }
                }
                if (!open_.load(std::memory_order_acquire)
                    || !renderTargetView_ || !swapChain_
                    || !deviceAccess_) {
                    continue;
                }

                timestamp = player->renderVideo();
                if (renderer_) {
                    const auto rendererStatistics =
                        renderer_->takeStatistics();
                    updateMaximum(
                        state->maximumColorSetupMicroseconds,
                        rendererStatistics
                            .maximumColorSetupMicroseconds);
                    updateMaximum(
                        state->maximumInteropMicroseconds,
                        rendererStatistics
                            .maximumInteropMicroseconds);
                    updateMaximum(
                        state->maximumBufferUpdateMicroseconds,
                        rendererStatistics
                            .maximumBufferUpdateMicroseconds);
                    updateMaximum(
                        state->maximumDrawMicroseconds,
                        rendererStatistics
                            .maximumDrawMicroseconds);
                }
                if (timestamp < 0.0) {
                    state->skippedRenders.fetch_add(
                        1,
                        std::memory_order_relaxed);
                    continue;
                }
                renderCompleted = steadyMicroseconds();
                if (options_.outputPreference
                        == D3D11OutputPreference::RequireHdr
                    && (!renderer_
                        || !renderer_->advancedColorInfo()
                                .isHdrOutput())) {
                    requiredHdrUnavailable = true;
                } else {
                    presentStatus = swapChain_->Present(
                        1,
                        DXGI_PRESENT_DO_NOT_WAIT);
                }
            }

            const auto presentCompleted = steadyMicroseconds();
            if (requiredHdrUnavailable) {
                if (!hdrRequirementFailed_.exchange(
                        true,
                        std::memory_order_relaxed)) {
                    setError(
                        "HDR output is required, but Windows Advanced "
                        "Color is not active on the current display",
                        DXGI_ERROR_NOT_CURRENTLY_AVAILABLE);
                }
                continue;
            }
            hdrRequirementFailed_.store(
                false,
                std::memory_order_relaxed);
            if (presentStatus == DXGI_ERROR_WAS_STILL_DRAWING) {
                waitForPresentationCapacity =
                    frameLatencyWaitableObject_ != nullptr;
                state->busyPresents.fetch_add(
                    1,
                    std::memory_order_relaxed);
                updateMaximum(
                    state->maximumRenderMicroseconds,
                    renderCompleted - renderStarted);
                updateMaximum(
                    state->maximumPresentMicroseconds,
                    presentCompleted - renderCompleted);
                requestRender(state);
                continue;
            }
            if (FAILED(presentStatus)) {
                const auto type =
                    presentStatus == DXGI_ERROR_DEVICE_REMOVED
                        || presentStatus == DXGI_ERROR_DEVICE_RESET
                    ? D3D11VideoOutputEventType::SurfaceLost
                    : D3D11VideoOutputEventType::Error;
                setError("D3D11 Present failed", presentStatus, type);
                continue;
            }
            waitForPresentationCapacity =
                frameLatencyWaitableObject_ != nullptr;

            const auto previous =
                state->previousRenderMicroseconds.exchange(
                    presentCompleted,
                    std::memory_order_relaxed);
            if (previous > 0) {
                const auto gap = presentCompleted - previous;
                updateMaximum(
                    state->maximumRenderGapMicroseconds,
                    gap);
                if (gap > 80'000) {
                    state->longRenderGaps.fetch_add(
                        1,
                        std::memory_order_relaxed);
                }
            }
            updateMaximum(
                state->maximumRenderMicroseconds,
                renderCompleted - renderStarted);
            updateMaximum(
                state->maximumPresentMicroseconds,
                presentCompleted - renderCompleted);
            state->presentedFrames.fetch_add(
                1,
                std::memory_order_relaxed);

            FramePresentedCallback callback;
            {
                std::lock_guard<std::mutex> lock(callbackMutex_);
                callback = framePresentedCallback_;
            }
            if (callback) {
                try {
                    callback(timestamp);
                } catch (...) {
                    // User callbacks must not terminate the render worker.
                }
            }
        }
    }

    void stopRenderThread() noexcept
    {
        const auto state = renderState_;
        if (state) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->stopping = true;
                state->requested = false;
                state->player = nullptr;
                ++state->generation;
            }
            state->changed.notify_all();
        }
        if (renderThread_.joinable()) {
            renderThread_.join();
        }
        renderState_.reset();
    }

    void releaseGraphics(bool unbindSurface) noexcept
    {
        if (unbindSurface && surfaceBound_ && surface_.bindSwapChain) {
            static_cast<void>(bindSurface(nullptr));
        }
        surfaceBound_ = false;

        std::lock_guard<std::mutex> lock(graphicsMutex_);
        open_.store(false, std::memory_order_release);
        if (renderer_) {
            renderer_->close();
        }
        if (interop_) {
            interop_->flush();
        }
        interop_.reset();
        renderer_.reset();
        renderTargetView_.Reset();
        if (frameLatencyWaitableObject_) {
            CloseHandle(frameLatencyWaitableObject_);
            frameLatencyWaitableObject_ = nullptr;
        }
        swapChain_.Reset();
        deviceAccess_.reset();
        context_.Reset();
        device_.Reset();
        surface_ = {};
        options_ = {};
        selectedFormat_ = DXGI_FORMAT_UNKNOWN;
        advancedColorEnabled_ = false;
        hdrRequirementFailed_.store(
            false,
            std::memory_order_relaxed);
        deviceDescription_.clear();
    }

    HRESULT bindSurface(IDXGISwapChain1* swapChain) noexcept
    {
        if (!surface_.bindSwapChain) {
            return E_INVALIDARG;
        }
        try {
            return surface_.bindSwapChain(swapChain);
        } catch (...) {
            return E_FAIL;
        }
    }

    void setError(
        const char* detail,
        HRESULT code,
        D3D11VideoOutputEventType type =
            D3D11VideoOutputEventType::Error)
    {
        const auto message = hresultDetail(detail, code);
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            lastError_ = message;
        }
        reportEvent({ type, message, code });
    }

    void reportEvent(D3D11VideoOutputEvent event)
    {
        EventCallback callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            callback = eventCallback_;
        }
        if (callback) {
            try {
                callback(event);
            } catch (...) {
                // User callbacks must not unwind through backend code.
            }
        }
    }

    mutable std::mutex graphicsMutex_;
    mutable std::mutex connectionMutex_;
    mutable std::mutex callbackMutex_;
    D3D11CompositionSurface surface_;
    D3D11VideoOutputOptions options_;
    EventCallback eventCallback_;
    FramePresentedCallback framePresentedCallback_;
    std::string lastError_;
    std::string deviceDescription_;
    std::atomic<bool> open_ { false };
    std::atomic<bool> hdrRequirementFailed_ { false };
    bool surfaceBound_ = false;
    bool advancedColorEnabled_ = false;
    DXGI_FORMAT selectedFormat_ = DXGI_FORMAT_UNKNOWN;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain3> swapChain_;
    HANDLE frameLatencyWaitableObject_ = nullptr;
    ComPtr<ID3D11RenderTargetView> renderTargetView_;
    std::shared_ptr<D3D11DeviceAccess> deviceAccess_;
    std::shared_ptr<D3D11VideoRenderer> renderer_;
    std::shared_ptr<D3D11FrameInterop> interop_;

    std::shared_ptr<RenderState> renderState_;
    std::thread renderThread_;
    Player* attachedPlayer_ = nullptr;
    HardwareDecodeConfig previousHardwareConfig_;
    bool restoreHardwareConfig_ = false;
};

D3D11VideoOutput::D3D11VideoOutput()
    : impl_(std::make_unique<Impl>())
{
}

D3D11VideoOutput::~D3D11VideoOutput() = default;

D3D11VideoOutput::D3D11VideoOutput(D3D11VideoOutput&&) noexcept = default;

D3D11VideoOutput&
D3D11VideoOutput::operator=(D3D11VideoOutput&&) noexcept = default;

D3D11VideoOutput&
D3D11VideoOutput::setEventCallback(EventCallback callback)
{
    impl_->setEventCallback(std::move(callback));
    return *this;
}

D3D11VideoOutput&
D3D11VideoOutput::setFramePresentedCallback(
    FramePresentedCallback callback)
{
    impl_->setFramePresentedCallback(std::move(callback));
    return *this;
}

bool D3D11VideoOutput::open(
    D3D11CompositionSurface surface,
    D3D11VideoOutputOptions options)
{
    return impl_->open(std::move(surface), options);
}

void D3D11VideoOutput::close() noexcept
{
    impl_->close();
}

bool D3D11VideoOutput::attach(Player& player)
{
    return impl_->attach(player);
}

void D3D11VideoOutput::detach() noexcept
{
    impl_->detach();
}

bool D3D11VideoOutput::resize(
    VideoSize size,
    float compositionScaleX,
    float compositionScaleY)
{
    return impl_->resize(
        size,
        compositionScaleX,
        compositionScaleY);
}

void D3D11VideoOutput::requestRender() noexcept
{
    impl_->requestRender();
}

bool D3D11VideoOutput::isOpen() const noexcept
{
    return impl_->isOpen();
}

bool D3D11VideoOutput::isAttached() const noexcept
{
    return impl_->isAttached();
}

std::string D3D11VideoOutput::lastError() const
{
    return impl_->lastError();
}

std::string D3D11VideoOutput::deviceDescription() const
{
    return impl_->deviceDescription();
}

D3D11VideoOutputColorInfo
D3D11VideoOutput::colorInfo() const noexcept
{
    return impl_->colorInfo();
}

std::shared_ptr<D3D11DeviceAccess>
D3D11VideoOutput::deviceAccess() const noexcept
{
    return impl_->deviceAccess();
}

D3D11VideoOutputStatistics
D3D11VideoOutput::takeStatistics() noexcept
{
    return impl_->takeStatistics();
}

} // namespace qtav
