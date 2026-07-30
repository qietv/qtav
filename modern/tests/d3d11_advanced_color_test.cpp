// SPDX-License-Identifier: LGPL-2.1-or-later

#if defined(NDEBUG)
#  undef NDEBUG
#endif

#if !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <d3d11.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <qtav/d3d11_video_renderer.h>

#include "frame_internal.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t windowClassName[] =
    L"QtAVCoreAdvancedColorTestWindow";

LRESULT CALLBACK windowProcedure(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    return DefWindowProcW(window, message, wParam, lParam);
}

struct TestWindow {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND handle = nullptr;

    TestWindow() = default;
    TestWindow(const TestWindow&) = delete;
    TestWindow& operator=(const TestWindow&) = delete;
    TestWindow(TestWindow&& other) noexcept
        : instance(other.instance)
        , handle(other.handle)
    {
        other.handle = nullptr;
    }
    TestWindow& operator=(TestWindow&&) = delete;

    ~TestWindow()
    {
        if (handle) {
            DestroyWindow(handle);
        }
        UnregisterClassW(windowClassName, instance);
    }
};

TestWindow makeWindow(const RECT& bounds)
{
    TestWindow result;
    WNDCLASSEXW windowClass {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &windowProcedure;
    windowClass.hInstance = result.instance;
    windowClass.lpszClassName = windowClassName;
    if (!RegisterClassExW(&windowClass)
        && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return result;
    }

    result.handle = CreateWindowExW(
        0,
        windowClassName,
        L"QtAVCore Advanced Color test",
        WS_OVERLAPPEDWINDOW,
        bounds.left + 16,
        bounds.top + 16,
        64,
        64,
        nullptr,
        nullptr,
        result.instance,
        nullptr);
    return result;
}

qtav::VideoFrame makeSdrFrame()
{
    AVFrame* frame = av_frame_alloc();
    assert(frame);
    frame->format = AV_PIX_FMT_BGRA;
    frame->width = 2;
    frame->height = 2;
    frame->color_range = AVCOL_RANGE_JPEG;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    frame->colorspace = AVCOL_SPC_RGB;
    assert(av_frame_get_buffer(frame, 32) >= 0);
    for (int y = 0; y < frame->height; ++y) {
        auto* row = frame->data[0]
            + static_cast<std::ptrdiff_t>(y) * frame->linesize[0];
        for (int x = 0; x < frame->width; ++x) {
            row[x * 4 + 0] = 255;
            row[x * 4 + 1] = 255;
            row[x * 4 + 2] = 255;
            row[x * 4 + 3] = 255;
        }
    }
    qtav::VideoFrame result =
        qtav::detail::FrameFactory::video(frame, 0, 0);
    av_frame_free(&frame);
    assert(result);
    return result;
}

double pqFromNits(double nits)
{
    constexpr double m1 = 2610.0 / 16384.0;
    constexpr double m2 = 2523.0 / 32.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 2413.0 / 128.0;
    constexpr double c3 = 2392.0 / 128.0;
    const double power = std::pow(nits / 10000.0, m1);
    return std::pow(
        (c1 + c2 * power) / (1.0 + c3 * power),
        m2);
}

qtav::VideoFrame makePqFrame()
{
    AVFrame* frame = av_frame_alloc();
    assert(frame);
    frame->format = AV_PIX_FMT_P010LE;
    frame->width = 2;
    frame->height = 2;
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->color_primaries = AVCOL_PRI_BT2020;
    frame->color_trc = AVCOL_TRC_SMPTE2084;
    frame->colorspace = AVCOL_SPC_BT2020_NCL;
    frame->chroma_location = AVCHROMA_LOC_LEFT;
    assert(av_frame_get_buffer(frame, 32) >= 0);

    const auto limitedCode = [](double normalized) {
        return static_cast<std::uint16_t>(
            std::lround(64.0 + normalized * 876.0));
    };
    const std::uint16_t diffuse = static_cast<std::uint16_t>(
        limitedCode(pqFromNits(80.0)) << 6U);
    const std::uint16_t highlight = static_cast<std::uint16_t>(
        limitedCode(pqFromNits(1000.0)) << 6U);
    for (int y = 0; y < frame->height; ++y) {
        auto* luma = reinterpret_cast<std::uint16_t*>(
            frame->data[0]
            + static_cast<std::ptrdiff_t>(y) * frame->linesize[0]);
        luma[0] = diffuse;
        luma[1] = highlight;
    }
    auto* chroma = reinterpret_cast<std::uint16_t*>(frame->data[1]);
    chroma[0] = static_cast<std::uint16_t>(512U << 6U);
    chroma[1] = static_cast<std::uint16_t>(512U << 6U);

    qtav::VideoFrame result =
        qtav::detail::FrameFactory::video(frame, 0, 0);
    av_frame_free(&frame);
    assert(result);
    return result;
}

float halfToFloat(std::uint16_t value)
{
    const std::uint32_t sign =
        static_cast<std::uint32_t>(value & 0x8000U) << 16U;
    int exponent = static_cast<int>((value >> 10U) & 0x1fU);
    std::uint32_t mantissa = value & 0x03ffU;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 1;
            while ((mantissa & 0x0400U) == 0) {
                mantissa <<= 1U;
                --exponent;
            }
            mantissa &= 0x03ffU;
            bits = sign
                | (static_cast<std::uint32_t>(exponent + 112) << 23U)
                | (mantissa << 13U);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7f800000U | (mantissa << 13U);
    } else {
        bits = sign
            | (static_cast<std::uint32_t>(exponent + 112) << 23U)
            | (mantissa << 13U);
    }
    float result = 0.0F;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

std::vector<float> readRedChannel(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture)
{
    D3D11_TEXTURE2D_DESC source {};
    texture->GetDesc(&source);
    D3D11_TEXTURE2D_DESC staging = source;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> readback;
    assert(SUCCEEDED(device->CreateTexture2D(
        &staging,
        nullptr,
        &readback)));
    context->CopyResource(readback.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped {};
    assert(SUCCEEDED(context->Map(
        readback.Get(),
        0,
        D3D11_MAP_READ,
        0,
        &mapped)));
    std::vector<float> result(
        static_cast<std::size_t>(source.Width)
        * static_cast<std::size_t>(source.Height));
    for (UINT y = 0; y < source.Height; ++y) {
        const auto* row = reinterpret_cast<const std::uint16_t*>(
            static_cast<const std::uint8_t*>(mapped.pData)
            + static_cast<std::size_t>(y) * mapped.RowPitch);
        for (UINT x = 0; x < source.Width; ++x) {
            result[
                static_cast<std::size_t>(y) * source.Width + x] =
                halfToFloat(row[x * 4]);
        }
    }
    context->Unmap(readback.Get(), 0);
    return result;
}

struct DeviceResources {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
};

DeviceResources makeDevice()
{
    DeviceResources result;
    const D3D_FEATURE_LEVEL levels[] {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL selected {};
    HRESULT status = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &result.device,
        &selected,
        &result.context);
    if (status == E_INVALIDARG) {
        status = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels + 1,
            1,
            D3D11_SDK_VERSION,
            &result.device,
            &selected,
            &result.context);
    }
    if (FAILED(status)) {
        return {};
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(result.device.As(&dxgiDevice))
        || FAILED(dxgiDevice->GetAdapter(&result.adapter))
        || FAILED(result.adapter->GetParent(
            IID_PPV_ARGS(&result.factory)))) {
        return {};
    }
    return result;
}

std::vector<DXGI_OUTPUT_DESC> attachedOutputs(IDXGIAdapter* adapter)
{
    std::vector<DXGI_OUTPUT_DESC> result;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIOutput> output;
        if (adapter->EnumOutputs(index, &output)
            == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (!output) {
            continue;
        }
        DXGI_OUTPUT_DESC description {};
        if (SUCCEEDED(output->GetDesc(&description))
            && description.AttachedToDesktop) {
            result.push_back(description);
        }
    }
    return result;
}

struct SwapChainTarget {
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> view;
};

SwapChainTarget makeSwapChainTarget(
    const DeviceResources& resources,
    HWND window,
    DXGI_FORMAT format =
        DXGI_FORMAT_R16G16B16A16_FLOAT)
{
    DXGI_SWAP_CHAIN_DESC1 description {};
    description.Width = 64;
    description.Height = 64;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    SwapChainTarget result;
    ComPtr<IDXGISwapChain1> swapChain1;
    if (FAILED(resources.factory->CreateSwapChainForHwnd(
            resources.device.Get(),
            window,
            &description,
            nullptr,
            nullptr,
            &swapChain1))
        || FAILED(swapChain1.As(&result.swapChain))
        || FAILED(result.swapChain->GetBuffer(
            0,
            IID_PPV_ARGS(&result.texture)))
        || FAILED(resources.device->CreateRenderTargetView(
            result.texture.Get(),
            nullptr,
            &result.view))) {
        return {};
    }
    resources.factory->MakeWindowAssociation(
        window,
        DXGI_MWA_NO_ALT_ENTER);
    return result;
}

} // namespace

int main()
{
    const DeviceResources resources = makeDevice();
    if (!resources.device || !resources.context
        || !resources.adapter || !resources.factory) {
        std::cout
            << "No hardware DXGI device; Advanced Color display test skipped\n";
        return 77;
    }
    const auto outputs = attachedOutputs(resources.adapter.Get());
    if (outputs.empty()) {
        std::cout
            << "No attached output on the D3D11 adapter; "
               "Advanced Color display test skipped\n";
        return 77;
    }

    TestWindow window = makeWindow(outputs.front().DesktopCoordinates);
    if (!window.handle) {
        std::cout
            << "No desktop window; Advanced Color display test skipped\n";
        return 77;
    }
    SwapChainTarget target = makeSwapChainTarget(
        resources,
        window.handle);
    if (!target.swapChain || !target.texture || !target.view) {
        std::cout
            << "FP16 flip-model swap chain unavailable; "
               "Advanced Color display test skipped\n";
        return 77;
    }

    auto access = qtav::D3D11DeviceAccess::create(
        qtav::BorrowedD3D11Device(resources.device.Get()),
        qtav::BorrowedD3D11DeviceContext(resources.context.Get()));
    assert(access);
    auto renderer = std::make_shared<qtav::D3D11VideoRenderer>(
        access,
        [&] {
            return qtav::D3D11RenderTarget {
                target.view.Get(),
                target.swapChain.Get(),
            };
        });
    int errors = 0;
    renderer->setEventCallback(
        [&](const qtav::VideoRenderEvent& event) {
            if (event.type == qtav::VideoRenderEventType::Error
                || event.type
                    == qtav::VideoRenderEventType::SurfaceLost) {
                ++errors;
                std::cerr << event.detail << '\n';
            }
        });
    qtav::VideoRenderConfig config;
    config.surfaceSize = { 64, 64 };
    config.aspectRatio = qtav::VideoAspectRatioMode::Stretch;
    assert(renderer->open(config));

    const qtav::VideoFrame frame = makeSdrFrame();
    assert(renderer->render(frame));
    auto info = renderer->advancedColorInfo();
    assert(info.displayDetected);
    assert(info.monitor == outputs.front().Monitor);
    assert(
        info.outputColorSpace
        == qtav::D3D11OutputColorSpace::ScRGB);
    assert(
        info.swapChainColorSpace
        == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    assert(info.swapChainColorSpaceConfigured);
    assert(info.bitsPerColor >= 8);
    assert(info.sdrWhiteLevelNits > 0.0F);
    assert(info.maximumLuminanceNits > 0.0F);
    assert(
        info.advancedColorActive
        == (info.displayColorSpace
            == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));
    assert(info.isHdrOutput() == info.advancedColorActive);
    if (info.advancedColorActive) {
        assert(info.sdrWhiteLevelFromSystem);
    }
    const qtav::D3D11AdvancedColorInfo testedInfo = info;

    const qtav::VideoFrame pqFrame = makePqFrame();
    assert(renderer->render(pqFrame));
    const auto red = readRedChannel(
        resources.device.Get(),
        resources.context.Get(),
        target.texture.Get());
    const float diffuse = red[32U * 64U + 16U];
    const float highlight = red[32U * 64U + 48U];
    assert(highlight > diffuse);
    if (testedInfo.advancedColorActive) {
        assert(diffuse > 0.9F && diffuse < 1.1F);
        assert(highlight > 1.1F);
    } else {
        assert(diffuse > 0.0F && diffuse < 1.0F);
        assert(highlight <= 1.01F);
    }
    const char* requireHdr = std::getenv("QTAV_REQUIRE_ACTIVE_HDR");
    if (requireHdr && std::strcmp(requireHdr, "1") == 0
        && !testedInfo.advancedColorActive) {
        std::cerr
            << "QTAV_REQUIRE_ACTIVE_HDR=1, but Windows HDR is disabled "
               "for the tested output\n";
        return 3;
    }
    assert(SUCCEEDED(target.swapChain->Present(0, 0)));

    bool hdr10Validated = false;
    TestWindow hdr10Window = makeWindow(
        outputs.front().DesktopCoordinates);
    SwapChainTarget hdr10Target = makeSwapChainTarget(
        resources,
        hdr10Window.handle,
        DXGI_FORMAT_R10G10B10A2_UNORM);
    if (hdr10Window.handle && hdr10Target.swapChain
        && hdr10Target.texture && hdr10Target.view) {
        auto hdr10Renderer =
            std::make_shared<qtav::D3D11VideoRenderer>(
                access,
                [&] {
                    return qtav::D3D11RenderTarget {
                        hdr10Target.view.Get(),
                        hdr10Target.swapChain.Get(),
                    };
                });
        hdr10Renderer->setEventCallback(
            [&](const qtav::VideoRenderEvent& event) {
                if (event.type == qtav::VideoRenderEventType::Error
                    || event.type
                        == qtav::VideoRenderEventType::SurfaceLost) {
                    ++errors;
                    std::cerr << event.detail << '\n';
                }
            });
        assert(hdr10Renderer->open(config));
        assert(hdr10Renderer->render(pqFrame));
        const auto hdr10Info =
            hdr10Renderer->advancedColorInfo();
        assert(hdr10Info.displayDetected);
        assert(hdr10Info.swapChainColorSpaceConfigured);
        if (hdr10Info.advancedColorActive) {
            assert(
                hdr10Info.outputColorSpace
                == qtav::D3D11OutputColorSpace::HDR10);
            assert(
                hdr10Info.swapChainColorSpace
                == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
            assert(hdr10Info.isHdrOutput());
            hdr10Validated = true;
        } else {
            assert(
                hdr10Info.outputColorSpace
                == qtav::D3D11OutputColorSpace::SDR);
            assert(
                hdr10Info.swapChainColorSpace
                == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            assert(!hdr10Info.isHdrOutput());
        }
        assert(SUCCEEDED(hdr10Target.swapChain->Present(0, 0)));
        hdr10Renderer->close();
    }
    if (requireHdr && std::strcmp(requireHdr, "1") == 0
        && !hdr10Validated) {
        std::cerr
            << "Windows HDR is active, but the native RGB10/HDR10 "
               "swap-chain path could not be validated\n";
        return 4;
    }

    if (outputs.size() > 1) {
        const RECT second = outputs[1].DesktopCoordinates;
        assert(SetWindowPos(
            window.handle,
            nullptr,
            second.left + 16,
            second.top + 16,
            64,
            64,
            SWP_NOACTIVATE | SWP_NOZORDER));
        assert(renderer->render(frame));
        info = renderer->advancedColorInfo();
        assert(info.displayDetected);
        assert(info.monitor == outputs[1].Monitor);
        assert(info.swapChainColorSpaceConfigured);
    }

    assert(errors == 0);
    std::cout
        << "Advanced Color output detected: HDR active="
        << (testedInfo.advancedColorActive ? "yes" : "no")
        << ", bits=" << testedInfo.bitsPerColor
        << ", SDR white=" << testedInfo.sdrWhiteLevelNits
        << (testedInfo.sdrWhiteLevelFromSystem
                ? " (system)"
                : " (fallback)")
        << " nits, peak=" << testedInfo.maximumLuminanceNits
        << " nits, PQ 80/1000-nit scRGB="
        << diffuse << '/' << highlight << '\n';
    renderer->close();
    return 0;
}
