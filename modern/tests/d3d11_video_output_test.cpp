// SPDX-License-Identifier: LGPL-2.1-or-later

#if defined(NDEBUG)
#  undef NDEBUG
#endif

#if !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <wrl/client.h>

#include <qtav/d3d11_device_access.h>
#include <qtav/d3d11_video_output.h>
#include <qtav/player.h>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
    assert(argc == 2);

    using Microsoft::WRL::ComPtr;

    std::mutex mutex;
    std::condition_variable changed;
    ComPtr<IDXGISwapChain1> boundSwapChain;
    int bindings = 0;
    int unbindings = 0;
    int presentedFrames = 0;
    int outputErrors = 0;

    qtav::D3D11CompositionSurface surface;
    surface.size = { 320, 180 };
    surface.compositionScaleX = 1.25F;
    surface.compositionScaleY = 1.25F;
    surface.bindSwapChain =
        [&](IDXGISwapChain1* swapChain) -> HRESULT {
            std::lock_guard<std::mutex> lock(mutex);
            boundSwapChain = swapChain;
            swapChain ? ++bindings : ++unbindings;
            changed.notify_all();
            return S_OK;
        };

    qtav::D3D11VideoOutput invalidOutput;
    qtav::D3D11VideoOutputOptions invalidOptions;
    invalidOptions.hdrPresentationMode =
        qtav::D3D11HdrPresentationMode::HDR10;
    assert(!invalidOutput.open(surface, invalidOptions));
    assert(
        invalidOutput.lastError().find("opaque")
        != std::string::npos);

    qtav::D3D11VideoOutputOptions options;
    options.outputPreference =
        qtav::D3D11OutputPreference::SdrOnly;
    options.forceWarp = true;
    options.allowWarpFallback = false;

    qtav::D3D11VideoOutput output;
    output
        .setEventCallback(
            [&](const qtav::D3D11VideoOutputEvent&) {
                std::lock_guard<std::mutex> lock(mutex);
                ++outputErrors;
                changed.notify_all();
            })
        .setFramePresentedCallback(
        [&](double timestamp) {
            assert(timestamp >= 0.0);
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++presentedFrames;
            }
            changed.notify_all();
        });
    const bool opened = output.open(surface, options);
    if (!opened) {
        std::fprintf(
            stderr,
            "D3D11 output open failed: %s\n",
            output.lastError().c_str());
    }
    assert(opened);
    assert(output.isOpen());
    assert(output.deviceAccess());
    const auto initialColorInfo = output.colorInfo();
    assert(
        initialColorInfo.preference
        == qtav::D3D11OutputPreference::SdrOnly);
    assert(
        initialColorInfo.format
        == DXGI_FORMAT_B8G8R8A8_UNORM);
    assert(!initialColorInfo.isHdrOutput());
    {
        std::lock_guard<std::mutex> lock(mutex);
        assert(boundSwapChain);
        assert(bindings == 1);
    }

    qtav::Player player;
    const auto previousHardwareConfig = player.hardwareDecodeConfig();
    assert(output.attach(player));
    assert(output.isAttached());
    assert(
        player.hardwareDecodeConfig().deviceType
        == qtav::HardwareDeviceType::D3D11);

    player.setMedia(argv[1]);
    player.setState(qtav::State::Playing);
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return presentedFrames >= 2; }));
    }

    player.setState(qtav::State::Paused);
    assert(player.waitFor(qtav::State::Paused, 5'000));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    static_cast<void>(output.takeStatistics());

    const auto deviceAccess = output.deviceAccess();
    assert(deviceAccess);
    int presentedBeforeRetry = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        presentedBeforeRetry = presentedFrames;
    }
    qtav::D3D11VideoOutputStatistics busyStatistics;
    {
        auto contextGuard = deviceAccess->contextGuard();
        output.requestRender();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        busyStatistics = output.takeStatistics();
        std::lock_guard<std::mutex> lock(mutex);
        assert(presentedFrames == presentedBeforeRetry);
    }
    assert(busyStatistics.rendererBusyRenderAttempts > 0);
    assert(
        busyStatistics.rendererDeviceContextBusyRenderAttempts
        == busyStatistics.rendererBusyRenderAttempts);
    assert(busyStatistics.retryWakeups > 0);
    assert(busyStatistics.contextHandoffWaits > 0);
    assert(busyStatistics.contextHandoffTimeouts > 0);
    assert(busyStatistics.skippedRenders == 0);
    assert(busyStatistics.terminalRenderDrops == 0);

    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return presentedFrames > presentedBeforeRetry; }));
    }
    const auto recoveredStatistics = output.takeStatistics();
    assert(recoveredStatistics.presentedFrames > 0);
    assert(recoveredStatistics.skippedRenders == 0);
    assert(recoveredStatistics.terminalRenderDrops == 0);

    int presentedBeforeResume = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        presentedBeforeResume = presentedFrames;
    }
    player.setState(qtav::State::Playing);
    assert(player.waitFor(qtav::State::Playing, 5'000));
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return presentedFrames > presentedBeforeResume; }));
    }
    int presentedBeforeRewind = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        presentedBeforeRewind = presentedFrames;
    }
    assert(player.seek(0));
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return presentedFrames > presentedBeforeRewind; }));
    }
    static_cast<void>(output.takeStatistics());

    int presentedBeforeSupersession = 0;
    qtav::D3D11VideoOutputStatistics supersededStatistics;
    const auto accumulateSupersessionStatistics =
        [&](const qtav::D3D11VideoOutputStatistics& statistics) {
            supersededStatistics.renderRequests +=
                statistics.renderRequests;
            supersededStatistics.renderPasses +=
                statistics.renderPasses;
            supersededStatistics.presentedFrames +=
                statistics.presentedFrames;
            supersededStatistics.busyPresents +=
                statistics.busyPresents;
            supersededStatistics.skippedRenders +=
                statistics.skippedRenders;
            supersededStatistics.rendererBusyRenderAttempts +=
                statistics.rendererBusyRenderAttempts;
            supersededStatistics.retryWakeups +=
                statistics.retryWakeups;
            supersededStatistics.supersededRenderFrames +=
                statistics.supersededRenderFrames;
            supersededStatistics.terminalRenderDrops +=
                statistics.terminalRenderDrops;
            supersededStatistics.rendererDeviceContextBusyRenderAttempts +=
                statistics.rendererDeviceContextBusyRenderAttempts;
        };
    {
        auto contextGuard = deviceAccess->contextGuard();
        output.requestRender();

        const auto busyDeadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(5);
        while (
            supersededStatistics
                    .rendererDeviceContextBusyRenderAttempts
                == 0
            && std::chrono::steady_clock::now() < busyDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            accumulateSupersessionStatistics(
                output.takeStatistics());
        }
        assert(
            supersededStatistics
                    .rendererDeviceContextBusyRenderAttempts
                > 0);

        // Observing the failed context attempt is the barrier: the serial
        // render worker has completed any prior Present/callback before it can
        // start this blocked attempt. A baseline sampled before that attempt
        // can race an already-rendered frame finishing presentation after the
        // test acquires the immediate-context guard.
        {
            std::lock_guard<std::mutex> lock(mutex);
            presentedBeforeSupersession = presentedFrames;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        accumulateSupersessionStatistics(output.takeStatistics());
        std::lock_guard<std::mutex> lock(mutex);
        assert(presentedFrames == presentedBeforeSupersession);
    }
    assert(supersededStatistics.rendererBusyRenderAttempts > 0);
    assert(
        supersededStatistics.rendererDeviceContextBusyRenderAttempts > 0);
    assert(supersededStatistics.retryWakeups > 0);
    assert(supersededStatistics.supersededRenderFrames > 0);
    assert(
        supersededStatistics.terminalRenderDrops
        >= supersededStatistics.supersededRenderFrames);
    assert(
        supersededStatistics.skippedRenders
        == supersededStatistics.terminalRenderDrops);
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return presentedFrames > presentedBeforeSupersession; }));
    }
    const auto supersessionRecoveryStatistics =
        output.takeStatistics();
    assert(supersessionRecoveryStatistics.presentedFrames > 0);

    assert(output.resize({ 640, 360 }, 1.5F, 1.5F));
    output.requestRender();
    {
        std::unique_lock<std::mutex> lock(mutex);
        const auto before = presentedFrames;
        assert(changed.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return presentedFrames > before; }));
    }

    player.setState(qtav::State::Stopped);
    assert(player.waitFor(qtav::State::Stopped, 5'000));
    output.detach();
    assert(!output.isAttached());
    assert(
        player.hardwareDecodeConfig().deviceType
        == previousHardwareConfig.deviceType);

    const auto statistics = output.takeStatistics();
    const auto totalRenderRequests =
        busyStatistics.renderRequests
        + recoveredStatistics.renderRequests
        + supersededStatistics.renderRequests
        + supersessionRecoveryStatistics.renderRequests
        + statistics.renderRequests;
    const auto totalRenderPasses =
        busyStatistics.renderPasses
        + recoveredStatistics.renderPasses
        + supersededStatistics.renderPasses
        + supersessionRecoveryStatistics.renderPasses
        + statistics.renderPasses;
    const auto totalPresentedFrames =
        busyStatistics.presentedFrames
        + recoveredStatistics.presentedFrames
        + supersededStatistics.presentedFrames
        + supersessionRecoveryStatistics.presentedFrames
        + statistics.presentedFrames;
    const auto totalBusyPresents =
        busyStatistics.busyPresents
        + recoveredStatistics.busyPresents
        + supersededStatistics.busyPresents
        + supersessionRecoveryStatistics.busyPresents
        + statistics.busyPresents;
    assert(totalRenderRequests >= totalPresentedFrames);
    assert(
        totalRenderPasses
        >= totalPresentedFrames + totalBusyPresents);
    assert(totalPresentedFrames >= 2);
    {
        std::lock_guard<std::mutex> lock(mutex);
        assert(outputErrors == 0);
    }

    output.close();
    assert(!output.isOpen());
    {
        std::lock_guard<std::mutex> lock(mutex);
        assert(!boundSwapChain);
        assert(unbindings == 1);
    }
    return 0;
}
