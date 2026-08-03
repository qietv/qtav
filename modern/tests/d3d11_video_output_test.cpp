// SPDX-License-Identifier: LGPL-2.1-or-later

#if defined(NDEBUG)
#  undef NDEBUG
#endif

#if !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <wrl/client.h>

#include <qtav/d3d11_video_output.h>
#include <qtav/player.h>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>

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
    assert(statistics.renderRequests >= statistics.presentedFrames);
    assert(
        statistics.renderPasses
        >= statistics.presentedFrames + statistics.busyPresents);
    assert(statistics.presentedFrames >= 3);
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
