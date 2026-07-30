// SPDX-License-Identifier: GPL-3.0-or-later

#include <qtav/player.h>

#if defined(QTAV_CORE_CONSOLE_HAS_D3D11_VIDEO)
#  if !defined(NOMINMAX)
#    define NOMINMAX
#  endif
#  include <d3d11.h>
#  include <wrl/client.h>
#  include <qtav/d3d11_frame_interop.h>
#  include <qtav/d3d11va_hardware_decoder.h>
#endif

#if defined(QTAV_CORE_CONSOLE_HAS_COREAUDIO)
#  include <qtav/coreaudio_audio_sink.h>
#  include <qtav/swresample_audio_converter.h>
#elif defined(QTAV_CORE_CONSOLE_HAS_WASAPI)
#  include <qtav/swresample_audio_converter.h>
#  include <qtav/wasapi_audio_sink.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>

namespace {

#if defined(QTAV_CORE_CONSOLE_HAS_D3D11_VIDEO)

bool configureD3D11Video(qtav::Player& player)
{
    using Microsoft::WRL::ComPtr;

    const D3D_FEATURE_LEVEL levels[] {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL selected {};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT status = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &device,
        &selected,
        &context);
    if (status == E_INVALIDARG) {
        status = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            levels + 1,
            1,
            D3D11_SDK_VERSION,
            &device,
            &selected,
            &context);
    }
    if (FAILED(status)) {
        return false;
    }

    auto deviceAccess = qtav::D3D11DeviceAccess::create(
        qtav::BorrowedD3D11Device(device.Get()),
        qtav::BorrowedD3D11DeviceContext(context.Get()));
    if (!deviceAccess) {
        return false;
    }

    constexpr int width = 640;
    constexpr int height = 360;
    D3D11_TEXTURE2D_DESC targetDescription {};
    targetDescription.Width = width;
    targetDescription.Height = height;
    targetDescription.MipLevels = 1;
    targetDescription.ArraySize = 1;
    targetDescription.Format =
        DXGI_FORMAT_B8G8R8A8_UNORM;
    targetDescription.SampleDesc.Count = 1;
    targetDescription.Usage = D3D11_USAGE_DEFAULT;
    targetDescription.BindFlags =
        D3D11_BIND_RENDER_TARGET
        | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> targetTexture;
    ComPtr<ID3D11RenderTargetView> targetView;
    if (FAILED(device->CreateTexture2D(
            &targetDescription,
            nullptr,
            &targetTexture))
        || FAILED(device->CreateRenderTargetView(
            targetTexture.Get(),
            nullptr,
            &targetView))) {
        return false;
    }

    auto renderer =
        std::make_shared<qtav::D3D11VideoRenderer>(
            deviceAccess,
            [retainedView = std::move(targetView)] {
                return qtav::D3D11RenderTarget {
                    retainedView.Get(),
                };
            });
    renderer->setHardwareFrameInterop(
        std::make_shared<qtav::D3D11FrameInterop>(
            deviceAccess));
    qtav::VideoRenderConfig renderConfig;
    renderConfig.surfaceSize = { width, height };
    if (!renderer->open(renderConfig)) {
        return false;
    }

    const auto decodeConfig =
        qtav::d3d11vaHardwareDecodeConfig(deviceAccess);
    if (!decodeConfig.device) {
        return false;
    }
    player
        .setHardwareDecodeConfig(decodeConfig)
        .setVideoRenderAPI(renderer);
    return true;
}

#endif

const char* stateName(qtav::State state)
{
    switch (state) {
    case qtav::State::Stopped:
        return "stopped";
    case qtav::State::Playing:
        return "playing";
    case qtav::State::Paused:
        return "paused";
    }
    return "unknown";
}

const char* statusName(qtav::MediaStatus status)
{
    switch (status) {
    case qtav::MediaStatus::NoMedia:
        return "no-media";
    case qtav::MediaStatus::Loading:
        return "loading";
    case qtav::MediaStatus::Loaded:
        return "loaded";
    case qtav::MediaStatus::Buffering:
        return "buffering";
    case qtav::MediaStatus::EndOfMedia:
        return "end";
    case qtav::MediaStatus::Invalid:
        return "invalid";
    }
    return "unknown";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: qtav_core_console <media-url>\n";
        return 2;
    }

    qtav::Player player;
    const bool requireNativeWindowsAV =
#if defined(QTAV_CORE_CONSOLE_HAS_D3D11_VIDEO) \
    && defined(QTAV_CORE_CONSOLE_HAS_WASAPI)
        std::getenv("QTAV_CORE_REQUIRE_NATIVE_WINDOWS_AV") != nullptr;
#else
        false;
#endif
    bool d3d11VideoConfigured = false;
#if defined(QTAV_CORE_CONSOLE_HAS_COREAUDIO)
    player
        .setAudioFrameConverter(
            std::make_shared<qtav::SwresampleAudioConverter>())
        .setAudioSink(
            std::make_shared<qtav::CoreAudioAudioSink>());
#elif defined(QTAV_CORE_CONSOLE_HAS_WASAPI)
    player
        .setAudioFrameConverter(
            std::make_shared<qtav::SwresampleAudioConverter>())
        .setAudioSink(
            std::make_shared<qtav::WasapiAudioSink>());
#endif

#if defined(QTAV_CORE_CONSOLE_HAS_D3D11_VIDEO)
    if (configureD3D11Video(player)) {
        d3d11VideoConfigured = true;
        std::cout
            << "video-backend: D3D11VA/D3D11 Video Processor\n";
    } else {
        std::cout
            << "video-backend: callback/software fallback\n";
    }
#endif

    std::atomic<std::uint64_t> decodedVideoFrames { 0 };
    std::atomic<std::uint64_t> renderedVideoFrames { 0 };
    std::atomic<std::uint64_t> decodedAudioFrames { 0 };
    std::atomic<std::uint64_t> hardwareVideoFrames { 0 };
    std::atomic<bool> hardwareDecodeFallback { false };
    std::atomic<bool> audioSinkOpenFailed { false };

    player
        .onStateChanged([](qtav::State state) {
            std::cout << "state: " << stateName(state) << '\n';
        })
        .onMediaStatus([](qtav::MediaStatus oldStatus,
                           qtav::MediaStatus newStatus) {
            std::cout << "status: " << statusName(oldStatus) << " -> "
                      << statusName(newStatus) << '\n';
            return false;
        })
        .onEvent([&](const qtav::MediaEvent& event) {
            if (event.category == "decoder.hardware.fallback") {
                hardwareDecodeFallback.store(true);
            } else if (event.category == "audio.sink.open") {
                audioSinkOpenFailed.store(true);
            }
            std::cerr << event.category << ": " << event.detail << '\n';
            return false;
        })
        .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
            if (frame.hasHardwareFrame()) {
                ++hardwareVideoFrames;
            }
            if (++decodedVideoFrames == 1) {
                std::cout << "video: " << frame.width() << 'x'
                          << frame.height() << ' ' << frame.formatName()
                          << " colorspace=" << frame.colorSpace() << '\n';
            }
        })
        .onAudioFrame([&](const qtav::AudioFrame&, int) {
            ++decodedAudioFrames;
        })
        .setVideoRenderer(
            [](const qtav::VideoFrame&, void*) {})
        .setRenderCallback([&](void*) {
            // A GUI integration schedules renderVideo() on its render thread.
            // This headless example can render immediately on the decode thread.
            if (player.renderVideo() >= 0.0) {
                ++renderedVideoFrames;
            }
        });

    player.setMedia(argv[1]);
    player.setState(qtav::State::Playing);

    if (!player.waitFor(qtav::State::Playing, 10'000)) {
        if (player.mediaStatus() == qtav::MediaStatus::Invalid) {
            return 1;
        }
    }
    if (!player.waitFor(qtav::State::Stopped, 60'000)) {
        std::cerr << "playback timed out\n";
        player.setState(qtav::State::Stopped);
        return 1;
    }

    const auto info = player.mediaInfo();
    std::cout << "duration-ms: " << info.duration << '\n'
              << "video-frames: " << decodedVideoFrames.load() << '\n'
              << "rendered-frames: " << renderedVideoFrames.load() << '\n'
              << "audio-frames: " << decodedAudioFrames.load() << '\n';
    if (player.mediaStatus() == qtav::MediaStatus::Invalid) {
        return 1;
    }
    if (requireNativeWindowsAV) {
        if (!d3d11VideoConfigured
            || hardwareDecodeFallback.load()
            || hardwareVideoFrames.load() == 0) {
            std::cout
                << "native Windows A/V integration skipped: "
                   "D3D11VA hardware presentation is unavailable\n";
            return 77;
        }
        if (audioSinkOpenFailed.load()) {
            std::cout
                << "native Windows A/V integration skipped: "
                   "no usable WASAPI render endpoint\n";
            return 77;
        }
        if (decodedAudioFrames.load() == 0
            || renderedVideoFrames.load() == 0) {
            std::cerr
                << "native Windows A/V integration failed to "
                   "render both streams\n";
            return 1;
        }
    }
    return 0;
}
