// SPDX-License-Identifier: LGPL-2.1-or-later

#if !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <qtav/d3d11_video_renderer.h>

#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "d3d11_video_renderer_p.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace qtav {
namespace {

using Microsoft::WRL::ComPtr;

std::int64_t steadyMicroseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void updateMaximum(
    std::atomic<std::int64_t>& destination,
    std::int64_t value) noexcept
{
    auto current = destination.load(std::memory_order_relaxed);
    while (current < value
           && !destination.compare_exchange_weak(
               current,
               value,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

constexpr const char* shaderSource = R"HLSL(
struct VertexInput {
    float2 position : POSITION;
    float2 texcoord : TEXCOORD0;
};

struct PixelInput {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

cbuffer ColorParameters : register(b0) {
    float4 colorRow0;
    float4 colorRow1;
    float4 colorRow2;
    uint sourceType;
    uint sourceTransfer;
    uint sourcePrimaries;
    uint outputColorSpace;
    float sdrWhiteLevelNits;
    float displayMaximumLuminanceNits;
    float contentMaximumLuminanceNits;
    uint advancedColorActive;
};

Texture2D<float4> source0 : register(t0);
Texture2D<float4> source1 : register(t1);
Texture2D<float4> source2 : register(t2);
SamplerState linearSampler : register(s0);

PixelInput vertexMain(VertexInput input)
{
    PixelInput result;
    result.position = float4(input.position, 0.0, 1.0);
    result.texcoord = input.texcoord;
    return result;
}

float3 pqToNits(float3 value)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    const float3 power = pow(max(value, 0.0), 1.0 / m2);
    const float3 numerator = max(power - c1, 0.0);
    const float3 denominator = max(c2 - c3 * power, 0.000001);
    return 10000.0 * pow(numerator / denominator, 1.0 / m1);
}

float3 nitsToPq(float3 value)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    const float3 power = pow(max(value, 0.0) / 10000.0, m1);
    return pow((c1 + c2 * power) / (1.0 + c3 * power), m2);
}

float3 hlgToNits(float3 value)
{
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    const float3 low = value * value / 3.0;
    const float3 high = (exp((value - c) / a) + b) / 12.0;
    return 1000.0 * lerp(low, high, step(0.5, value));
}

float3 sdrToLinear(float3 value)
{
    const float3 low = value / 4.5;
    const float3 high = pow((value + 0.099) / 1.099, 1.0 / 0.45);
    return lerp(low, high, step(0.081, value));
}

float3 srgbToLinear(float3 value)
{
    const float3 low = value / 12.92;
    const float3 high = pow((value + 0.055) / 1.055, 2.4);
    return lerp(low, high, step(0.04045, value));
}

float3 linearToSrgb(float3 value)
{
    const float3 low = 12.92 * value;
    const float3 high =
        1.055 * pow(max(value, 0.0), 1.0 / 2.4) - 0.055;
    return lerp(low, high, step(0.0031308, value));
}

float3 toBt709(float3 value, uint primaries)
{
    if (primaries == 1) {
        return float3(
            1.660491 * value.r - 0.587641 * value.g
                - 0.072850 * value.b,
            -0.124550 * value.r + 1.132900 * value.g
                - 0.008349 * value.b,
            -0.018151 * value.r - 0.100579 * value.g
                + 1.118730 * value.b);
    }
    if (primaries == 2) {
        return float3(
            1.224745 * value.r - 0.224904 * value.g,
            -0.042058 * value.r + 1.042081 * value.g,
            -0.019642 * value.r - 0.078655 * value.g
                + 1.098537 * value.b);
    }
    return value;
}

float3 bt709ToBt2020(float3 value)
{
    return float3(
        0.627404 * value.r + 0.329283 * value.g
            + 0.043313 * value.b,
        0.069097 * value.r + 0.919540 * value.g
            + 0.011362 * value.b,
        0.016391 * value.r + 0.088013 * value.g
            + 0.895595 * value.b);
}

float3 toneMapToPeak(float3 value, float contentPeak, float outputPeak)
{
    const float3 positive = max(value, 0.0);
    const float luminance =
        dot(positive, float3(0.2126, 0.7152, 0.0722));
    if (contentPeak <= outputPeak || luminance <= 0.000001) {
        return value;
    }
    const float relative = luminance / outputPeak;
    const float white = max(contentPeak / outputPeak, 1.0);
    const float mapped =
        relative * (1.0 + relative / (white * white))
        / (1.0 + relative);
    return value * (mapped * outputPeak / luminance);
}

float3 sampleSource(PixelInput input)
{
    if (sourceType == 0) {
        return source0.Sample(linearSampler, input.texcoord).rgb;
    }
    if (sourceType == 1) {
        const float value = source0.Sample(linearSampler, input.texcoord).r;
        return value.xxx;
    }
    const float y = source0.Sample(linearSampler, input.texcoord).r;
    float u = 0.5;
    float v = 0.5;
    if (sourceType == 2) {
        u = source1.Sample(linearSampler, input.texcoord).r;
        v = source2.Sample(linearSampler, input.texcoord).r;
    } else {
        const float2 chroma =
            source1.Sample(linearSampler, input.texcoord).rg;
        u = sourceType == 4 ? chroma.y : chroma.x;
        v = sourceType == 4 ? chroma.x : chroma.y;
    }

    const float4 sample = float4(y, u, v, 1.0);
    return float3(
        dot(colorRow0, sample),
        dot(colorRow1, sample),
        dot(colorRow2, sample));
}

float4 pixelMain(PixelInput input) : SV_TARGET
{
    const float3 encoded = sampleSource(input);
    float3 linearNits;
    if (sourceTransfer == 1) {
        linearNits = pqToNits(encoded);
    } else if (sourceTransfer == 2) {
        linearNits = hlgToNits(encoded);
    } else if (sourceTransfer == 3) {
        linearNits = max(encoded, 0.0) * sdrWhiteLevelNits;
    } else if (sourceTransfer == 4) {
        linearNits = encoded * 80.0;
    } else if (sourceTransfer == 5) {
        linearNits =
            srgbToLinear(max(encoded, 0.0)) * sdrWhiteLevelNits;
    } else {
        linearNits =
            sdrToLinear(max(encoded, 0.0)) * sdrWhiteLevelNits;
    }

    float3 bt709Nits = toBt709(linearNits, sourcePrimaries);
    const float outputPeak = max(displayMaximumLuminanceNits, 1.0);
    bt709Nits = toneMapToPeak(
        bt709Nits,
        max(contentMaximumLuminanceNits, sdrWhiteLevelNits),
        outputPeak);

    if (outputColorSpace == 1) {
        if (advancedColorActive == 0) {
            return float4(
                saturate(bt709Nits / outputPeak),
                1.0);
        }
        return float4(bt709Nits / 80.0, 1.0);
    }
    if (outputColorSpace == 2) {
        const float3 bt2020Nits =
            max(bt709ToBt2020(bt709Nits), 0.0);
        return float4(
            saturate(nitsToPq(min(bt2020Nits, outputPeak))),
            1.0);
    }
    return float4(
        saturate(linearToSrgb(max(bt709Nits, 0.0) / outputPeak)),
        1.0);
}
)HLSL";

struct Vertex {
    float x = 0.0F;
    float y = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
};

struct ColorParameters {
    std::array<float, 4> row0 {};
    std::array<float, 4> row1 {};
    std::array<float, 4> row2 {};
    std::uint32_t sourceType = 0;
    std::uint32_t sourceTransfer = 0;
    std::uint32_t sourcePrimaries = 0;
    std::uint32_t outputColorSpace = 0;
    float sdrWhiteLevelNits = 80.0F;
    float displayMaximumLuminanceNits = 80.0F;
    float contentMaximumLuminanceNits = 80.0F;
    std::uint32_t advancedColorActive = 0;
};

static_assert(sizeof(ColorParameters) % 16 == 0);

struct UploadedFrame {
    std::array<ComPtr<ID3D11ShaderResourceView>, 3> views;
    ColorParameters color;
};

bool isSupportedConfig(const VideoRenderConfig& config) noexcept
{
    if (!config.surfaceSize.isValid()
        || config.deviceOwnership != NativeResourceOwnership::Borrowed
        || config.contextOwnership != NativeResourceOwnership::Borrowed
        || config.surfaceOwnership != NativeResourceOwnership::Borrowed) {
        return false;
    }
    if (!config.viewport.isValid()) {
        return config.viewport.x == 0 && config.viewport.y == 0
            && config.viewport.width == 0 && config.viewport.height == 0;
    }
    const auto& viewport = config.viewport;
    return viewport.x >= 0 && viewport.y >= 0
        && viewport.x <= config.surfaceSize.width - viewport.width
        && viewport.y <= config.surfaceSize.height - viewport.height;
}

bool isSupportedTargetFormat(DXGI_FORMAT format) noexcept
{
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return true;
    default:
        return false;
    }
}

std::string hresultText(const char* operation, HRESULT result);

bool sameAdvancedColorInfo(
    const D3D11AdvancedColorInfo& left,
    const D3D11AdvancedColorInfo& right) noexcept
{
    return left.outputColorSpace == right.outputColorSpace
        && left.swapChainColorSpace == right.swapChainColorSpace
        && left.displayColorSpace == right.displayColorSpace
        && left.monitor == right.monitor
        && left.bitsPerColor == right.bitsPerColor
        && left.sdrWhiteLevelNits == right.sdrWhiteLevelNits
        && left.minimumLuminanceNits == right.minimumLuminanceNits
        && left.maximumLuminanceNits == right.maximumLuminanceNits
        && left.maximumFullFrameLuminanceNits
            == right.maximumFullFrameLuminanceNits
        && left.displayDetected == right.displayDetected
        && left.advancedColorActive == right.advancedColorActive
        && left.swapChainColorSpaceConfigured
            == right.swapChainColorSpaceConfigured
        && left.sdrWhiteLevelFromSystem
            == right.sdrWhiteLevelFromSystem;
}

float querySdrWhiteLevelNits(const wchar_t* deviceName) noexcept
{
    if (!deviceName || !*deviceName) {
        return 0.0F;
    }

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(
            QDC_ONLY_ACTIVE_PATHS,
            &pathCount,
            &modeCount)
        != ERROR_SUCCESS) {
        return 0.0F;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    LONG status = QueryDisplayConfig(
        QDC_ONLY_ACTIVE_PATHS,
        &pathCount,
        paths.data(),
        &modeCount,
        modes.data(),
        nullptr);
    if (status != ERROR_SUCCESS) {
        return 0.0F;
    }
    paths.resize(pathCount);

    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
        source.header.type =
            DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId =
            path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header)
                != ERROR_SUCCESS
            || std::wcscmp(
                   source.viewGdiDeviceName,
                   deviceName)
                != 0) {
            continue;
        }

        DISPLAYCONFIG_SDR_WHITE_LEVEL white {};
        white.header.type =
            DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId =
            path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header)
                == ERROR_SUCCESS
            && white.SDRWhiteLevel > 0) {
            return static_cast<float>(white.SDRWhiteLevel)
                * (80.0F / 1000.0F);
        }
    }
    return 0.0F;
}

HRESULT findOutputForMonitor(
    ID3D11Device* device,
    HMONITOR monitor,
    ComPtr<IDXGIOutput>& output)
{
    if (!device || !monitor) {
        return E_INVALIDARG;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT result = device->QueryInterface(
        IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(result)) {
        return result;
    }

    ComPtr<IDXGIAdapter> deviceAdapter;
    result = dxgiDevice->GetAdapter(&deviceAdapter);
    if (FAILED(result)) {
        return result;
    }

    ComPtr<IDXGIFactory1> factory;
    result = deviceAdapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        return result;
    }

    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumAdapters1(adapterIndex, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) {
            return DXGI_ERROR_NOT_FOUND;
        }
        if (FAILED(result)) {
            return result;
        }

        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> candidate;
            result = adapter->EnumOutputs(outputIndex, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(result)) {
                return result;
            }

            DXGI_OUTPUT_DESC description {};
            result = candidate->GetDesc(&description);
            if (FAILED(result)) {
                continue;
            }
            if (description.Monitor == monitor) {
                output = std::move(candidate);
                return S_OK;
            }
        }
    }
}

bool configureAdvancedColor(
    ID3D11Device* expectedDevice,
    const D3D11RenderTarget& target,
    DXGI_FORMAT targetFormat,
    D3D11AdvancedColorInfo& info,
    std::string& error)
{
    info = {};
    if (targetFormat == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        info.outputColorSpace = D3D11OutputColorSpace::ScRGB;
        info.swapChainColorSpace =
            DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        info.bitsPerColor = 16;
        info.maximumLuminanceNits = 1000.0F;
        info.maximumFullFrameLuminanceNits = 1000.0F;
    } else if (targetFormat == DXGI_FORMAT_R10G10B10A2_UNORM) {
        info.outputColorSpace = D3D11OutputColorSpace::HDR10;
        info.swapChainColorSpace =
            DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        info.bitsPerColor = 10;
        info.maximumLuminanceNits = 1000.0F;
        info.maximumFullFrameLuminanceNits = 1000.0F;
    }

    if (!target.swapChain) {
        return true;
    }

    ComPtr<ID3D11Device> swapChainDevice;
    HRESULT result = target.swapChain->GetDevice(
        IID_PPV_ARGS(&swapChainDevice));
    if (FAILED(result) || swapChainDevice.Get() != expectedDevice) {
        error =
            "The current D3D11 swap chain belongs to another device";
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 swapDescription {};
    result = target.swapChain->GetDesc1(&swapDescription);
    if (FAILED(result)) {
        error = hresultText(
            "IDXGISwapChain1::GetDesc1",
            result);
        return false;
    }
    if (swapDescription.Format != targetFormat
        || (swapDescription.SwapEffect
                != DXGI_SWAP_EFFECT_FLIP_DISCARD
            && swapDescription.SwapEffect
                != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)) {
        error =
            "Advanced Color requires a matching flip-model D3D11 swap chain";
        return false;
    }

    ComPtr<IDXGIOutput> output;
    if (target.monitor) {
        result = findOutputForMonitor(
            expectedDevice,
            target.monitor,
            output);
    } else {
        result = target.swapChain->GetContainingOutput(&output);
    }
    if (FAILED(result) || !output) {
        error = target.monitor
            ? hresultText(
                "DXGI output lookup for the current monitor",
                result)
            : hresultText(
                "IDXGISwapChain::GetContainingOutput",
                result);
        return false;
    }
    ComPtr<IDXGIOutput6> output6;
    result = output.As(&output6);
    if (FAILED(result) || !output6) {
        error =
            "The current display does not expose IDXGIOutput6 Advanced Color information";
        return false;
    }

    DXGI_OUTPUT_DESC1 outputDescription {};
    result = output6->GetDesc1(&outputDescription);
    if (FAILED(result)) {
        error = hresultText(
            "IDXGIOutput6::GetDesc1",
            result);
        return false;
    }

    info.displayDetected = true;
    info.monitor = outputDescription.Monitor;
    info.bitsPerColor =
        static_cast<int>(outputDescription.BitsPerColor);
    info.displayColorSpace = outputDescription.ColorSpace;
    info.minimumLuminanceNits =
        outputDescription.MinLuminance;
    info.maximumLuminanceNits =
        outputDescription.MaxLuminance;
    info.maximumFullFrameLuminanceNits =
        outputDescription.MaxFullFrameLuminance;
    info.advancedColorActive =
        outputDescription.ColorSpace
        == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;

    const float queriedSdrWhite =
        querySdrWhiteLevelNits(outputDescription.DeviceName);
    if (queriedSdrWhite > 0.0F) {
        info.sdrWhiteLevelNits = queriedSdrWhite;
        info.sdrWhiteLevelFromSystem = true;
    }

    if (targetFormat == DXGI_FORMAT_R10G10B10A2_UNORM
        && !info.advancedColorActive) {
        info.outputColorSpace = D3D11OutputColorSpace::SDR;
        info.swapChainColorSpace =
            DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }
    if (!info.advancedColorActive
        || info.outputColorSpace
            == D3D11OutputColorSpace::SDR) {
        info.maximumLuminanceNits =
            info.sdrWhiteLevelNits;
        info.maximumFullFrameLuminanceNits =
            info.sdrWhiteLevelNits;
    } else {
        if (!(info.maximumLuminanceNits > 0.0F)) {
            info.maximumLuminanceNits = 1000.0F;
        }
        if (!(info.maximumFullFrameLuminanceNits > 0.0F)) {
            info.maximumFullFrameLuminanceNits =
                info.maximumLuminanceNits;
        }
    }

    UINT colorSpaceSupport = 0;
    result = target.swapChain->CheckColorSpaceSupport(
        info.swapChainColorSpace,
        &colorSpaceSupport);
    if (FAILED(result)
        || !(colorSpaceSupport
            & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
        error =
            "The current flip-model swap chain cannot present the required Advanced Color space";
        return false;
    }
    result = target.swapChain->SetColorSpace1(
        info.swapChainColorSpace);
    if (FAILED(result)) {
        error = hresultText(
            "IDXGISwapChain3::SetColorSpace1",
            result);
        return false;
    }
    info.swapChainColorSpaceConfigured = true;
    return true;
}

std::string hresultText(const char* operation, HRESULT result)
{
    std::ostringstream stream;
    stream << operation << " failed with HRESULT 0x" << std::hex
           << std::uppercase << static_cast<unsigned long>(result);
    return stream.str();
}

bool checkedSize(
    int width,
    int height,
    int bytesPerPixel,
    std::size_t& result) noexcept
{
    if (width <= 0 || height <= 0 || bytesPerPixel <= 0) {
        return false;
    }
    const auto row = static_cast<std::size_t>(width)
        * static_cast<std::size_t>(bytesPerPixel);
    if (row > std::numeric_limits<std::size_t>::max()
            / static_cast<std::size_t>(height)) {
        return false;
    }
    result = row * static_cast<std::size_t>(height);
    return true;
}

bool copyPlane(
    const std::uint8_t* source,
    int sourceStride,
    int width,
    int height,
    int bytesPerPixel,
    std::vector<std::uint8_t>& destination,
    std::string& error)
{
    std::size_t size = 0;
    if (!source || sourceStride == 0
        || !checkedSize(width, height, bytesPerPixel, size)) {
        error = "The D3D11 renderer received an invalid software frame plane";
        return false;
    }
    const std::size_t rowBytes = static_cast<std::size_t>(width)
        * static_cast<std::size_t>(bytesPerPixel);
    const std::int64_t strideMagnitude = sourceStride < 0
        ? -static_cast<std::int64_t>(sourceStride)
        : static_cast<std::int64_t>(sourceStride);
    if (static_cast<std::uint64_t>(strideMagnitude) < rowBytes) {
        error = "A software frame plane has a stride smaller than its row";
        return false;
    }

    destination.resize(size);
    for (int y = 0; y < height; ++y) {
        std::memcpy(
            destination.data() + static_cast<std::size_t>(y) * rowBytes,
            source + static_cast<std::ptrdiff_t>(y) * sourceStride,
            rowBytes);
    }
    return true;
}

bool createPlaneView(
    ID3D11Device* device,
    const std::uint8_t* source,
    int sourceStride,
    int width,
    int height,
    int bytesPerPixel,
    DXGI_FORMAT format,
    ComPtr<ID3D11ShaderResourceView>& view,
    std::string& error)
{
    std::vector<std::uint8_t> bytes;
    if (!copyPlane(
            source,
            sourceStride,
            width,
            height,
            bytesPerPixel,
            bytes,
            error)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC descriptor {};
    descriptor.Width = static_cast<UINT>(width);
    descriptor.Height = static_cast<UINT>(height);
    descriptor.MipLevels = 1;
    descriptor.ArraySize = 1;
    descriptor.Format = format;
    descriptor.SampleDesc.Count = 1;
    descriptor.Usage = D3D11_USAGE_IMMUTABLE;
    descriptor.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial {};
    initial.pSysMem = bytes.data();
    initial.SysMemPitch =
        static_cast<UINT>(width * bytesPerPixel);

    ComPtr<ID3D11Texture2D> texture;
    HRESULT result = device->CreateTexture2D(
        &descriptor,
        &initial,
        &texture);
    if (FAILED(result)) {
        error = hresultText("ID3D11Device::CreateTexture2D", result);
        return false;
    }
    result = device->CreateShaderResourceView(
        texture.Get(),
        nullptr,
        &view);
    if (FAILED(result)) {
        error = hresultText(
            "ID3D11Device::CreateShaderResourceView",
            result);
        return false;
    }
    return true;
}

template <typename Frame>
bool createPackedView(
    ID3D11Device* device,
    const Frame& frame,
    ComPtr<ID3D11ShaderResourceView>& view,
    std::string& error)
{
    const int width = frame.width();
    const int height = frame.height();
    std::size_t size = 0;
    if (!frame.data(0) || frame.lineSize(0) == 0
        || !checkedSize(width, height, 4, size)) {
        error = "The D3D11 renderer received an invalid packed frame";
        return false;
    }

    int sourceBytes = 0;
    switch (frame.format()) {
    case PixelFormat::RGB24:
    case PixelFormat::BGR24:
        sourceBytes = 3;
        break;
    case PixelFormat::RGBA:
    case PixelFormat::BGRA:
    case PixelFormat::ARGB:
        sourceBytes = 4;
        break;
    default:
        error = "The D3D11 renderer does not support this packed format";
        return false;
    }
    const std::int64_t strideMagnitude = frame.lineSize(0) < 0
        ? -static_cast<std::int64_t>(frame.lineSize(0))
        : static_cast<std::int64_t>(frame.lineSize(0));
    if (strideMagnitude
        < static_cast<std::int64_t>(width) * sourceBytes) {
        error = "The packed software frame stride is too small";
        return false;
    }

    std::vector<std::uint8_t> rgba(size);
    for (int y = 0; y < height; ++y) {
        const auto* source = frame.data(0)
            + static_cast<std::ptrdiff_t>(y) * frame.lineSize(0);
        auto* destination = rgba.data()
            + static_cast<std::size_t>(y * width) * 4;
        for (int x = 0; x < width; ++x) {
            const auto* pixel = source + x * sourceBytes;
            auto* output = destination + x * 4;
            switch (frame.format()) {
            case PixelFormat::RGB24:
                output[0] = pixel[0];
                output[1] = pixel[1];
                output[2] = pixel[2];
                output[3] = 255;
                break;
            case PixelFormat::BGR24:
                output[0] = pixel[2];
                output[1] = pixel[1];
                output[2] = pixel[0];
                output[3] = 255;
                break;
            case PixelFormat::RGBA:
                std::memcpy(output, pixel, 4);
                break;
            case PixelFormat::BGRA:
                output[0] = pixel[2];
                output[1] = pixel[1];
                output[2] = pixel[0];
                output[3] = pixel[3];
                break;
            case PixelFormat::ARGB:
                output[0] = pixel[1];
                output[1] = pixel[2];
                output[2] = pixel[3];
                output[3] = pixel[0];
                break;
            default:
                break;
            }
        }
    }
    return createPlaneView(
        device,
        rgba.data(),
        width * 4,
        width,
        height,
        4,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        view,
        error);
}

void setYuvMatrix(
    int width,
    int height,
    VideoColorSpace color,
    bool p010,
    ColorParameters& parameters)
{
    double kr = 0.2126;
    double kb = 0.0722;
    switch (color.matrix) {
    case ColorMatrix::BT470BG:
    case ColorMatrix::SMPTE170M:
    case ColorMatrix::SMPTE240M:
    case ColorMatrix::FCC:
        kr = 0.2990;
        kb = 0.1140;
        break;
    case ColorMatrix::BT2020NCL:
    case ColorMatrix::BT2020CL:
    case ColorMatrix::ChromaDerivedNCL:
    case ColorMatrix::ChromaDerivedCL:
        kr = 0.2627;
        kb = 0.0593;
        break;
    case ColorMatrix::Unknown:
        if (width <= 1024 && height <= 576) {
            kr = 0.2990;
            kb = 0.1140;
        }
        break;
    default:
        break;
    }
    const double kg = 1.0 - kr - kb;
    const double rCr = 2.0 * (1.0 - kr);
    const double bCb = 2.0 * (1.0 - kb);
    const double gCb = -2.0 * kb * (1.0 - kb) / kg;
    const double gCr = -2.0 * kr * (1.0 - kr) / kg;

    const bool fullRange = color.range == ColorRange::Full;
    double yOffset = 0.0;
    double yScale = 1.0;
    double cOffset = 0.5;
    double cScale = 1.0;
    if (!fullRange) {
        if (p010) {
            constexpr double normalizedCodeScale = 65535.0 / 65472.0;
            yOffset = 64.0 / 1023.0 / normalizedCodeScale;
            yScale = (1023.0 / 876.0) * normalizedCodeScale;
            cOffset = 512.0 / 1023.0 / normalizedCodeScale;
            cScale = (1023.0 / 896.0) * normalizedCodeScale;
        } else {
            yOffset = 16.0 / 255.0;
            yScale = 255.0 / 219.0;
            cOffset = 128.0 / 255.0;
            cScale = 255.0 / 224.0;
        }
    } else if (p010) {
        constexpr double normalizedCodeScale = 65535.0 / 65472.0;
        yScale = normalizedCodeScale;
        cOffset = 512.0 / 1023.0 / normalizedCodeScale;
        cScale = normalizedCodeScale;
    }

    const auto row = [&](double cb, double cr) {
        return std::array<float, 4> {
            static_cast<float>(yScale),
            static_cast<float>(cb * cScale),
            static_cast<float>(cr * cScale),
            static_cast<float>(
                -yScale * yOffset - (cb + cr) * cScale * cOffset),
        };
    };
    parameters.row0 = row(0.0, rCr);
    parameters.row1 = row(gCb, gCr);
    parameters.row2 = row(bCb, 0.0);
}

std::uint32_t shaderTransfer(ColorTransfer transfer) noexcept
{
    switch (transfer) {
    case ColorTransfer::PQ:
        return 1;
    case ColorTransfer::HLG:
        return 2;
    case ColorTransfer::Linear:
        return 3;
    case ColorTransfer::SRGB:
        return 5;
    default:
        return 0;
    }
}

std::uint32_t shaderPrimaries(ColorPrimaries primaries) noexcept
{
    switch (primaries) {
    case ColorPrimaries::BT2020:
        return 1;
    case ColorPrimaries::SMPTE432:
        return 2;
    default:
        return 0;
    }
}

float contentMaximumLuminance(const VideoFrame& frame) noexcept
{
    const ContentLightMetadata content =
        frame.contentLightMetadata();
    if (content.maximumContentLightLevel > 0) {
        return static_cast<float>(
            content.maximumContentLightLevel);
    }
    const MasteringDisplayMetadata mastering =
        frame.masteringDisplayMetadata();
    if (mastering.hasLuminance
        && mastering.maximumLuminance > 0.0) {
        return static_cast<float>(
            mastering.maximumLuminance);
    }
    return frame.colorSpaceInfo().isHdr() ? 1000.0F : 80.0F;
}

void setPresentationParameters(
    const VideoFrame& frame,
    const D3D11TextureFrame* imported,
    const D3D11AdvancedColorInfo& output,
    ColorParameters& parameters) noexcept
{
    const VideoColorSpace source = frame.colorSpaceInfo();
    parameters.sourceTransfer =
        shaderTransfer(source.transfer);
    parameters.sourcePrimaries =
        shaderPrimaries(source.primaries);
    parameters.contentMaximumLuminanceNits =
        contentMaximumLuminance(frame);

    if (imported) {
        switch (imported->colorSpace()) {
        case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
            parameters.sourceTransfer = 1;
            parameters.sourcePrimaries = 1;
            break;
        case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
            parameters.sourceTransfer = 4;
            parameters.sourcePrimaries = 0;
            break;
        default:
            parameters.sourceTransfer = 5;
            parameters.sourcePrimaries = 0;
            parameters.contentMaximumLuminanceNits =
                output.sdrWhiteLevelNits;
            break;
        }
    }

    parameters.outputColorSpace =
        static_cast<std::uint32_t>(output.outputColorSpace);
    parameters.sdrWhiteLevelNits =
        output.sdrWhiteLevelNits > 0.0F
        ? output.sdrWhiteLevelNits
        : 80.0F;
    parameters.displayMaximumLuminanceNits =
        output.maximumLuminanceNits > 0.0F
        ? output.maximumLuminanceNits
        : parameters.sdrWhiteLevelNits;
    parameters.contentMaximumLuminanceNits = std::max(
        parameters.contentMaximumLuminanceNits,
        parameters.sdrWhiteLevelNits);
    parameters.advancedColorActive =
        (output.advancedColorActive || !output.displayDetected)
        ? 1U
        : 0U;
}

template <typename Frame>
bool uploadSoftwareFrame(
    ID3D11Device* device,
    const Frame& frame,
    VideoColorSpace color,
    UploadedFrame& uploaded,
    std::string& error)
{
    const int width = frame.width();
    const int height = frame.height();
    const int chromaWidth = (width + 1) / 2;
    const int chromaHeight = (height + 1) / 2;
    switch (frame.format()) {
    case PixelFormat::RGB24:
    case PixelFormat::BGR24:
    case PixelFormat::RGBA:
    case PixelFormat::BGRA:
    case PixelFormat::ARGB:
        uploaded.color.sourceType = 0;
        return createPackedView(device, frame, uploaded.views[0], error);
    case PixelFormat::Gray8:
        uploaded.color.sourceType = 1;
        return createPlaneView(
            device,
            frame.data(0),
            frame.lineSize(0),
            width,
            height,
            1,
            DXGI_FORMAT_R8_UNORM,
            uploaded.views[0],
            error);
    case PixelFormat::YUV420P:
    case PixelFormat::YUV422P:
    case PixelFormat::YUV444P: {
        uploaded.color.sourceType = 2;
        const int planeWidth = frame.format() == PixelFormat::YUV444P
            ? width
            : chromaWidth;
        const int planeHeight = frame.format() == PixelFormat::YUV420P
            ? chromaHeight
            : height;
        if (!createPlaneView(
                device,
                frame.data(0),
                frame.lineSize(0),
                width,
                height,
                1,
                DXGI_FORMAT_R8_UNORM,
                uploaded.views[0],
                error)
            || !createPlaneView(
                device,
                frame.data(1),
                frame.lineSize(1),
                planeWidth,
                planeHeight,
                1,
                DXGI_FORMAT_R8_UNORM,
                uploaded.views[1],
                error)
            || !createPlaneView(
                device,
                frame.data(2),
                frame.lineSize(2),
                planeWidth,
                planeHeight,
                1,
                DXGI_FORMAT_R8_UNORM,
                uploaded.views[2],
                error)) {
            return false;
        }
        setYuvMatrix(width, height, color, false, uploaded.color);
        return true;
    }
    case PixelFormat::NV12:
    case PixelFormat::NV21:
        uploaded.color.sourceType =
            frame.format() == PixelFormat::NV12 ? 3 : 4;
        if (!createPlaneView(
                device,
                frame.data(0),
                frame.lineSize(0),
                width,
                height,
                1,
                DXGI_FORMAT_R8_UNORM,
                uploaded.views[0],
                error)
            || !createPlaneView(
                device,
                frame.data(1),
                frame.lineSize(1),
                chromaWidth,
                chromaHeight,
                2,
                DXGI_FORMAT_R8G8_UNORM,
                uploaded.views[1],
                error)) {
            return false;
        }
        setYuvMatrix(width, height, color, false, uploaded.color);
        return true;
    case PixelFormat::P010:
        uploaded.color.sourceType = 3;
        if (!createPlaneView(
                device,
                frame.data(0),
                frame.lineSize(0),
                width,
                height,
                2,
                DXGI_FORMAT_R16_UNORM,
                uploaded.views[0],
                error)
            || !createPlaneView(
                device,
                frame.data(1),
                frame.lineSize(1),
                chromaWidth,
                chromaHeight,
                4,
                DXGI_FORMAT_R16G16_UNORM,
                uploaded.views[1],
                error)) {
            return false;
        }
        setYuvMatrix(width, height, color, true, uploaded.color);
        return true;
    default:
        error = "The D3D11 renderer does not support this software pixel format";
        return false;
    }
}

bool uploadFrame(
    ID3D11Device* device,
    const VideoFrame& frame,
    UploadedFrame& uploaded,
    std::string& error)
{
    if (!frame || frame.hasHardwareFrame()) {
        error =
            "The D3D11 software renderer requires a valid software video frame";
        return false;
    }
    return uploadSoftwareFrame(
        device,
        frame,
        frame.colorSpaceInfo(),
        uploaded,
        error);
}

bool uploadMappedFrame(
    ID3D11Device* device,
    const HardwareFrameMapping& frame,
    int expectedWidth,
    int expectedHeight,
    VideoColorSpace color,
    UploadedFrame& uploaded,
    std::string& error)
{
    if (frame.width() != expectedWidth
        || frame.height() != expectedHeight) {
        error =
            "The mapped D3D11 hardware frame dimensions do not match its source";
        return false;
    }
    return uploadSoftwareFrame(
        device,
        frame,
        color,
        uploaded,
        error);
}

bool importTextureFrame(
    ID3D11Device* device,
    const VideoFrame& source,
    const D3D11TextureFrame& imported,
    UploadedFrame& uploaded,
    std::string& error)
{
    if (imported.width() != source.width()
        || imported.height() != source.height()
        || !imported.texture()
        || !imported.shaderResourceView()) {
        error =
            "The imported D3D11 texture frame has invalid dimensions, format, or resources";
        return false;
    }

    ComPtr<ID3D11Device> textureDevice;
    imported.texture()->GetDevice(&textureDevice);
    ComPtr<ID3D11Resource> viewResource;
    imported.shaderResourceView()->GetResource(&viewResource);
    if (textureDevice.Get() != device
        || viewResource.Get() != imported.texture()) {
        error =
            "The imported D3D11 texture frame belongs to another device or resource";
        return false;
    }

    D3D11_TEXTURE2D_DESC textureDescription {};
    imported.texture()->GetDesc(&textureDescription);
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription {};
    imported.shaderResourceView()->GetDesc(&viewDescription);
    const DXGI_FORMAT expectedFormat = imported.dxgiFormat();
    const bool validFormat =
        (expectedFormat == DXGI_FORMAT_B8G8R8A8_UNORM
            && imported.format() == PixelFormat::BGRA)
        || ((expectedFormat == DXGI_FORMAT_R8G8B8A8_UNORM
                || expectedFormat
                    == DXGI_FORMAT_R10G10B10A2_UNORM
                || expectedFormat
                    == DXGI_FORMAT_R16G16B16A16_FLOAT)
            && imported.format() == PixelFormat::RGBA);
    if (textureDescription.Width != static_cast<UINT>(source.width())
        || textureDescription.Height != static_cast<UINT>(source.height())
        || textureDescription.ArraySize != 1
        || textureDescription.SampleDesc.Count != 1
        || !validFormat
        || textureDescription.Format != expectedFormat
        || viewDescription.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D
        || (viewDescription.Format != DXGI_FORMAT_UNKNOWN
            && viewDescription.Format != expectedFormat)) {
        error =
            "The imported D3D11 texture or shader view description is unsupported";
        return false;
    }

    uploaded.views[0] = imported.shaderResourceView();
    uploaded.color.sourceType = 0;
    return true;
}

std::array<float, 2> rotatedCoordinate(
    VideoRotation rotation,
    float u,
    float v) noexcept
{
    switch (rotation) {
    case VideoRotation::Rotate90:
        return { v, 1.0F - u };
    case VideoRotation::Rotate180:
        return { 1.0F - u, 1.0F - v };
    case VideoRotation::Rotate270:
        return { 1.0F - v, u };
    default:
        return { u, v };
    }
}

void makeGeometry(
    const VideoFrame& frame,
    const VideoRenderConfig& config,
    std::array<Vertex, 4>& vertices,
    D3D11_VIEWPORT& viewport)
{
    VideoViewport bounds = config.viewport;
    if (!bounds.isValid()) {
        bounds = {
            0,
            0,
            config.surfaceSize.width,
            config.surfaceSize.height,
        };
    }

    const bool quarterTurn =
        config.rotation == VideoRotation::Rotate90
        || config.rotation == VideoRotation::Rotate270;
    const float displayWidth = static_cast<float>(
        quarterTurn ? frame.height() : frame.width());
    const float displayHeight = static_cast<float>(
        quarterTurn ? frame.width() : frame.height());
    const float sourceAspect = displayWidth / displayHeight;
    const float targetAspect = static_cast<float>(bounds.width)
        / static_cast<float>(bounds.height);

    float drawX = static_cast<float>(bounds.x);
    float drawY = static_cast<float>(bounds.y);
    float drawWidth = static_cast<float>(bounds.width);
    float drawHeight = static_cast<float>(bounds.height);
    float u0 = 0.0F;
    float u1 = 1.0F;
    float v0 = 0.0F;
    float v1 = 1.0F;

    if (config.aspectRatio == VideoAspectRatioMode::Fit) {
        if (targetAspect > sourceAspect) {
            drawWidth = drawHeight * sourceAspect;
            drawX += (static_cast<float>(bounds.width) - drawWidth) * 0.5F;
        } else {
            drawHeight = drawWidth / sourceAspect;
            drawY += (static_cast<float>(bounds.height) - drawHeight) * 0.5F;
        }
    } else if (config.aspectRatio == VideoAspectRatioMode::Fill) {
        if (targetAspect > sourceAspect) {
            const float visible = sourceAspect / targetAspect;
            v0 = (1.0F - visible) * 0.5F;
            v1 = 1.0F - v0;
        } else {
            const float visible = targetAspect / sourceAspect;
            u0 = (1.0F - visible) * 0.5F;
            u1 = 1.0F - u0;
        }
    }

    viewport.TopLeftX = drawX;
    viewport.TopLeftY = drawY;
    viewport.Width = drawWidth;
    viewport.Height = drawHeight;
    viewport.MinDepth = 0.0F;
    viewport.MaxDepth = 1.0F;

    const std::array<std::array<float, 2>, 4> displayed {
        std::array<float, 2> { u0, v0 },
        std::array<float, 2> { u1, v0 },
        std::array<float, 2> { u0, v1 },
        std::array<float, 2> { u1, v1 },
    };
    const std::array<std::array<float, 2>, 4> positions {
        std::array<float, 2> { -1.0F, 1.0F },
        std::array<float, 2> { 1.0F, 1.0F },
        std::array<float, 2> { -1.0F, -1.0F },
        std::array<float, 2> { 1.0F, -1.0F },
    };
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        const auto uv = rotatedCoordinate(
            config.rotation,
            displayed[index][0],
            displayed[index][1]);
        vertices[index] = {
            positions[index][0],
            positions[index][1],
            uv[0],
            uv[1],
        };
    }
}

bool targetDescription(
    ID3D11Device* expectedDevice,
    ID3D11RenderTargetView* view,
    D3D11_TEXTURE2D_DESC& description,
    DXGI_FORMAT& viewFormat,
    std::string& error)
{
    if (!view) {
        error = "The current D3D11 render target is unavailable";
        return false;
    }
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (!resource
        || FAILED(resource.As(&texture))
        || !texture) {
        error = "The current D3D11 render target is not a 2D texture";
        return false;
    }
    ComPtr<ID3D11Device> targetDevice;
    texture->GetDevice(&targetDevice);
    if (targetDevice.Get() != expectedDevice) {
        error = "The current D3D11 render target belongs to another device";
        return false;
    }
    D3D11_RENDER_TARGET_VIEW_DESC viewDescription {};
    view->GetDesc(&viewDescription);
    if (viewDescription.ViewDimension
        != D3D11_RTV_DIMENSION_TEXTURE2D) {
        error = "The current D3D11 render target is not a 2D texture view";
        return false;
    }
    texture->GetDesc(&description);
    viewFormat = viewDescription.Format == DXGI_FORMAT_UNKNOWN
        ? description.Format
        : viewDescription.Format;
    return true;
}

} // namespace

bool D3D11AdvancedColorInfo::isHdrOutput() const noexcept
{
    return advancedColorActive
        && outputColorSpace != D3D11OutputColorSpace::SDR;
}

bool D3D11RenderTarget::isValid() const noexcept
{
    return view != nullptr;
}

D3D11TextureFrame::~D3D11TextureFrame() = default;

DXGI_FORMAT D3D11TextureFrame::dxgiFormat() const noexcept
{
    return format() == PixelFormat::BGRA
        ? DXGI_FORMAT_B8G8R8A8_UNORM
        : format() == PixelFormat::RGBA
        ? DXGI_FORMAT_R8G8B8A8_UNORM
        : DXGI_FORMAT_UNKNOWN;
}

DXGI_COLOR_SPACE_TYPE
D3D11TextureFrame::colorSpace() const noexcept
{
    return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

D3D11HardwareFrameInterop::~D3D11HardwareFrameInterop() = default;
std::shared_ptr<D3D11TextureFrame>
D3D11HardwareFrameInterop::importFrame(
    const HardwareFrame& frame,
    const VideoColorSpace&)
{
    return importFrame(frame);
}

class D3D11VideoRenderer::Impl {
public:
    Impl(
        std::shared_ptr<D3D11DeviceAccess> deviceAccess,
        D3D11CurrentTargetCallback currentTarget)
        : deviceAccess_(std::move(deviceAccess))
        , device_(
              deviceAccess_
                  ? deviceAccess_->device()
                  : BorrowedD3D11Device {})
        , context_(
              deviceAccess_
                  ? deviceAccess_->immediateContext()
                  : BorrowedD3D11DeviceContext {})
        , currentTarget_(std::move(currentTarget))
    {
    }

    void notify(VideoRenderEventType type, std::string detail)
    {
        EventCallback callback;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            callback = eventCallback_;
        }
        if (callback) {
            callback({ type, std::move(detail) });
        }
    }

    bool makePipeline(std::string& error)
    {
        if (vertexShader_ && pixelShader_) {
            return true;
        }
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG;
#endif
        ComPtr<ID3DBlob> vertexCode;
        ComPtr<ID3DBlob> pixelCode;
        ComPtr<ID3DBlob> messages;
        HRESULT result = D3DCompile(
            shaderSource,
            std::strlen(shaderSource),
            "qtav_d3d11_renderer",
            nullptr,
            nullptr,
            "vertexMain",
            "vs_5_0",
            flags,
            0,
            &vertexCode,
            &messages);
        if (FAILED(result)) {
            error = "D3D11 vertex shader compilation failed";
            if (messages && messages->GetBufferPointer()) {
                error += ": ";
                error.append(
                    static_cast<const char*>(messages->GetBufferPointer()),
                    messages->GetBufferSize());
            }
            return false;
        }
        messages.Reset();
        result = D3DCompile(
            shaderSource,
            std::strlen(shaderSource),
            "qtav_d3d11_renderer",
            nullptr,
            nullptr,
            "pixelMain",
            "ps_5_0",
            flags,
            0,
            &pixelCode,
            &messages);
        if (FAILED(result)) {
            error = "D3D11 pixel shader compilation failed";
            if (messages && messages->GetBufferPointer()) {
                error += ": ";
                error.append(
                    static_cast<const char*>(messages->GetBufferPointer()),
                    messages->GetBufferSize());
            }
            return false;
        }

        result = device_.get()->CreateVertexShader(
            vertexCode->GetBufferPointer(),
            vertexCode->GetBufferSize(),
            nullptr,
            &vertexShader_);
        if (FAILED(result)) {
            error = hresultText(
                "ID3D11Device::CreateVertexShader",
                result);
            return false;
        }
        result = device_.get()->CreatePixelShader(
            pixelCode->GetBufferPointer(),
            pixelCode->GetBufferSize(),
            nullptr,
            &pixelShader_);
        if (FAILED(result)) {
            error = hresultText(
                "ID3D11Device::CreatePixelShader",
                result);
            return false;
        }

        const D3D11_INPUT_ELEMENT_DESC elements[] {
            {
                "POSITION",
                0,
                DXGI_FORMAT_R32G32_FLOAT,
                0,
                0,
                D3D11_INPUT_PER_VERTEX_DATA,
                0,
            },
            {
                "TEXCOORD",
                0,
                DXGI_FORMAT_R32G32_FLOAT,
                0,
                8,
                D3D11_INPUT_PER_VERTEX_DATA,
                0,
            },
        };
        result = device_.get()->CreateInputLayout(
            elements,
            static_cast<UINT>(std::size(elements)),
            vertexCode->GetBufferPointer(),
            vertexCode->GetBufferSize(),
            &inputLayout_);
        if (FAILED(result)) {
            error = hresultText(
                "ID3D11Device::CreateInputLayout",
                result);
            return false;
        }

        D3D11_BUFFER_DESC vertexDescriptor {};
        vertexDescriptor.ByteWidth =
            static_cast<UINT>(sizeof(Vertex) * 4);
        vertexDescriptor.Usage = D3D11_USAGE_DYNAMIC;
        vertexDescriptor.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vertexDescriptor.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        result = device_.get()->CreateBuffer(
            &vertexDescriptor,
            nullptr,
            &vertexBuffer_);
        if (FAILED(result)) {
            error = hresultText(
                "ID3D11Device::CreateBuffer(vertex)",
                result);
            return false;
        }

        D3D11_BUFFER_DESC colorDescriptor {};
        colorDescriptor.ByteWidth =
            static_cast<UINT>(sizeof(ColorParameters));
        colorDescriptor.Usage = D3D11_USAGE_DYNAMIC;
        colorDescriptor.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        colorDescriptor.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        result = device_.get()->CreateBuffer(
            &colorDescriptor,
            nullptr,
            &colorBuffer_);
        if (FAILED(result)) {
            error = hresultText(
                "ID3D11Device::CreateBuffer(color)",
                result);
            return false;
        }

        D3D11_SAMPLER_DESC samplerDescriptor {};
        samplerDescriptor.Filter =
            D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        samplerDescriptor.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescriptor.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescriptor.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescriptor.MaxLOD = D3D11_FLOAT32_MAX;
        result = device_.get()->CreateSamplerState(
            &samplerDescriptor,
            &sampler_);
        if (FAILED(result)) {
            error = hresultText(
                "ID3D11Device::CreateSamplerState",
                result);
            return false;
        }

        D3D11_RASTERIZER_DESC rasterizerDescriptor {};
        rasterizerDescriptor.FillMode = D3D11_FILL_SOLID;
        rasterizerDescriptor.CullMode = D3D11_CULL_NONE;
        rasterizerDescriptor.DepthClipEnable = TRUE;
        result = device_.get()->CreateRasterizerState(
            &rasterizerDescriptor,
            &rasterizer_);
        if (FAILED(result)) {
            error = hresultText(
                "ID3D11Device::CreateRasterizerState",
                result);
            return false;
        }
        return true;
    }

    void releasePipeline() noexcept
    {
        rasterizer_.Reset();
        sampler_.Reset();
        colorBuffer_.Reset();
        vertexBuffer_.Reset();
        inputLayout_.Reset();
        pixelShader_.Reset();
        vertexShader_.Reset();
    }

    mutable std::mutex stateMutex_;
    std::mutex renderMutex_;
    std::shared_ptr<D3D11DeviceAccess> deviceAccess_;
    BorrowedD3D11Device device_;
    BorrowedD3D11DeviceContext context_;
    D3D11CurrentTargetCallback currentTarget_;
    std::shared_ptr<D3D11HardwareFrameInterop> hardwareInterop_;
    EventCallback eventCallback_;
    VideoRenderConfig config_;
    D3D11AdvancedColorInfo advancedColorInfo_;
    bool allowSoftwareMappingFallback_ = false;
    bool open_ = false;

    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11Buffer> colorBuffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    std::atomic<std::int64_t> maximumColorSetupMicroseconds_ { 0 };
    std::atomic<std::int64_t> maximumInteropMicroseconds_ { 0 };
    std::atomic<std::int64_t> maximumBufferUpdateMicroseconds_ { 0 };
    std::atomic<std::int64_t> maximumDrawMicroseconds_ { 0 };
};

D3D11VideoRenderer::D3D11VideoRenderer(
    BorrowedD3D11Device device,
    BorrowedD3D11DeviceContext context,
    D3D11CurrentTargetCallback currentTarget)
    : impl_(std::make_unique<Impl>(
          D3D11DeviceAccess::create(device, context),
          std::move(currentTarget)))
{
}

D3D11VideoRenderer::D3D11VideoRenderer(
    std::shared_ptr<D3D11DeviceAccess> deviceAccess,
    D3D11CurrentTargetCallback currentTarget)
    : impl_(std::make_unique<Impl>(
          std::move(deviceAccess),
          std::move(currentTarget)))
{
}

D3D11VideoRenderer::~D3D11VideoRenderer() = default;
D3D11VideoRenderer::D3D11VideoRenderer(D3D11VideoRenderer&&) noexcept =
    default;
D3D11VideoRenderer& D3D11VideoRenderer::operator=(
    D3D11VideoRenderer&&) noexcept = default;

VideoRenderCapabilities D3D11VideoRenderer::capabilities() const
{
    VideoRenderCapabilities result;
    result.softwareFormats = {
        PixelFormat::YUV420P,
        PixelFormat::YUV422P,
        PixelFormat::YUV444P,
        PixelFormat::NV12,
        PixelFormat::NV21,
        PixelFormat::P010,
        PixelFormat::RGB24,
        PixelFormat::BGR24,
        PixelFormat::RGBA,
        PixelFormat::BGRA,
        PixelFormat::ARGB,
        PixelFormat::Gray8,
    };
    std::shared_ptr<D3D11HardwareFrameInterop> hardwareInterop;
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        hardwareInterop = impl_->hardwareInterop_;
    }
    if (hardwareInterop
        && hardwareInterop->deviceAccess() == impl_->deviceAccess_) {
        result.hardwareDevices =
            hardwareInterop->capabilities().sourceDevices;
    }
    result.customViewport = true;
    result.rotation = true;
    return result;
}

void D3D11VideoRenderer::setEventCallback(EventCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex_);
    impl_->eventCallback_ = std::move(callback);
}

bool D3D11VideoRenderer::open(const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }

    std::string error;
    if (!impl_->device_ || !impl_->context_) {
        error = "The D3D11 renderer requires a borrowed device and context";
    } else if (!isSupportedConfig(config)) {
        error =
            "The D3D11 renderer requires a valid borrowed surface configuration";
    } else {
        ComPtr<ID3D11Device> contextDevice;
        impl_->context_.get()->GetDevice(&contextDevice);
        if (contextDevice.Get() != impl_->device_.get()) {
            error = "The borrowed D3D11 context belongs to another device";
        }
    }
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        if (error.empty() && !impl_->currentTarget_) {
            error =
                "The D3D11 renderer requires a current-target callback";
        } else if (
            error.empty() && impl_->hardwareInterop_
            && impl_->hardwareInterop_->deviceAccess()
                != impl_->deviceAccess_) {
            error =
                "The D3D11 hardware interop belongs to another device access";
        }
    }
    if (!error.empty()) {
        impl_->notify(VideoRenderEventType::Error, std::move(error));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->renderMutex_);
        if (!impl_->makePipeline(error)) {
            impl_->releasePipeline();
        }
    }
    if (!error.empty()) {
        impl_->notify(
            detail::d3d11FailureEvent(
                impl_->device_.get()->GetDeviceRemovedReason()),
            std::move(error));
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        impl_->config_ = config;
        impl_->open_ = true;
    }
    return true;
}

bool D3D11VideoRenderer::configure(const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }
    bool configured = false;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        configured = impl_->open_ && isSupportedConfig(config);
        if (configured) {
            impl_->config_ = config;
        }
    }
    if (!configured) {
        impl_->notify(
            VideoRenderEventType::Error,
            "The D3D11 renderer is closed or the configuration is invalid");
    } else {
        impl_->notify(VideoRenderEventType::RedrawRequested, {});
    }
    return configured;
}

bool D3D11VideoRenderer::render(const VideoFrame& frame)
{
    if (!impl_) {
        return false;
    }

    VideoRenderConfig config;
    D3D11CurrentTargetCallback currentTarget;
    std::shared_ptr<D3D11HardwareFrameInterop> hardwareInterop;
    bool allowSoftwareMappingFallback = false;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        if (!impl_->open_) {
            currentTarget = {};
        } else {
            config = impl_->config_;
            currentTarget = impl_->currentTarget_;
            hardwareInterop = impl_->hardwareInterop_;
            allowSoftwareMappingFallback =
                impl_->allowSoftwareMappingFallback_;
        }
    }
    if (!currentTarget) {
        impl_->notify(
            VideoRenderEventType::Error,
            "The D3D11 renderer is not open");
        return false;
    }

    const D3D11RenderTarget target = currentTarget();
    if (!target.isValid()) {
        impl_->notify(
            VideoRenderEventType::SurfaceLost,
            "The current D3D11 render target is unavailable");
        return false;
    }

    std::string error;
    std::string fallbackDetail;
    VideoRenderEventType errorType = VideoRenderEventType::Error;
    bool rendered = false;
    D3D11AdvancedColorInfo advancedColorInfo;
    bool advancedColorInfoResolved = false;
    std::unique_lock<std::mutex> renderLock(
        impl_->renderMutex_,
        std::try_to_lock);
    if (!renderLock.owns_lock()) {
        return false;
    }
    auto contextGuard = impl_->deviceAccess_->tryContextGuard();
    if (!contextGuard) {
        return false;
    }
    {
        const HRESULT removedReason =
            impl_->device_.get()->GetDeviceRemovedReason();
        if (detail::d3d11FailureEvent(removedReason)
            == VideoRenderEventType::SurfaceLost) {
            errorType = VideoRenderEventType::SurfaceLost;
            error = hresultText(
                "The D3D11 device was removed",
                removedReason);
        }

        const auto colorSetupStarted = steadyMicroseconds();
        D3D11_TEXTURE2D_DESC targetDescriptor {};
        DXGI_FORMAT targetFormat = DXGI_FORMAT_UNKNOWN;
        if (error.empty()
            && (!targetDescription(
                    impl_->device_.get(),
                    target.view,
                    targetDescriptor,
                    targetFormat,
                    error)
                || targetDescriptor.Width
                    != static_cast<UINT>(config.surfaceSize.width)
                || targetDescriptor.Height
                    != static_cast<UINT>(config.surfaceSize.height)
                || targetDescriptor.SampleDesc.Count != 1
                || !isSupportedTargetFormat(targetFormat))) {
            if (error.empty()) {
                error =
                    "The current D3D11 target size, sample count, or pixel format is unsupported";
            }
        }
        if (error.empty()) {
            advancedColorInfoResolved = configureAdvancedColor(
                impl_->device_.get(),
                target,
                targetFormat,
                advancedColorInfo,
                error);
        }
        updateMaximum(
            impl_->maximumColorSetupMicroseconds_,
            steadyMicroseconds() - colorSetupStarted);

        UploadedFrame uploaded;
        std::shared_ptr<D3D11TextureFrame> textureFrame;
        std::shared_ptr<HardwareFrameMapping> mappedFrame;
        if (error.empty() && frame.hasHardwareFrame()) {
            const HardwareFrame hardwareFrame = frame.hardwareFrame();
            const bool compatibleInterop =
                hardwareInterop
                && hardwareInterop->deviceAccess()
                    == impl_->deviceAccess_
                && hardwareInterop->supports(hardwareFrame);
            if (compatibleInterop) {
                const auto interopStarted =
                    steadyMicroseconds();
                // Keep the imported object until the Video Processor and draw
                // commands below are submitted. Decoder, interop, and renderer
                // submissions share this serialized immediate context, so
                // later decoder reuse is ordered after those GPU reads. D3D11
                // retains resources referenced by queued commands; a per-frame
                // completion query would only throttle submission and keep
                // scarce decoder surfaces unavailable longer.
                textureFrame = hardwareInterop->importFrame(
                    hardwareFrame,
                    frame.colorSpaceInfo());
                if (textureFrame) {
                    if (!importTextureFrame(
                            impl_->device_.get(),
                            frame,
                            *textureFrame,
                            uploaded,
                            error)) {
                        textureFrame.reset();
                    }
                } else if (error.empty()) {
                    error = "D3D11 hardware-frame import failed";
                }
                updateMaximum(
                    impl_->maximumInteropMicroseconds_,
                    steadyMicroseconds() - interopStarted);
            } else {
                error =
                    "The D3D11 renderer has no compatible interop for this hardware frame";
            }

            if (!error.empty() && allowSoftwareMappingFallback) {
                fallbackDetail = error
                    + "; using explicit software-mapping fallback";
                error.clear();
                if (!hardwareFrame.isMappable(HardwareMapMode::Read)
                    || !(mappedFrame =
                        hardwareFrame.map(HardwareMapMode::Read))) {
                    error =
                        "The D3D11 hardware frame could not be mapped for software fallback";
                } else if (!uploadMappedFrame(
                               impl_->device_.get(),
                               *mappedFrame,
                               frame.width(),
                               frame.height(),
                               frame.colorSpaceInfo(),
                               uploaded,
                               error)) {
                    mappedFrame.reset();
                }
            }
        } else if (
            error.empty()
            && !uploadFrame(
                impl_->device_.get(),
                frame,
                uploaded,
                error)) {
            // The common error classification below handles this failure.
        }
        if (error.empty()) {
            setPresentationParameters(
                frame,
                textureFrame.get(),
                advancedColorInfo,
                uploaded.color);
        }
        if (!error.empty()) {
            const HRESULT uploadReason =
                impl_->device_.get()->GetDeviceRemovedReason();
            errorType = detail::d3d11FailureEvent(uploadReason);
        }

        std::array<Vertex, 4> vertices;
        D3D11_VIEWPORT viewport {};
        const auto bufferUpdateStarted = steadyMicroseconds();
        if (error.empty()) {
            makeGeometry(frame, config, vertices, viewport);

            D3D11_MAPPED_SUBRESOURCE mapped {};
            HRESULT result = impl_->context_.get()->Map(
                impl_->vertexBuffer_.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped);
            if (SUCCEEDED(result)) {
                std::memcpy(
                    mapped.pData,
                    vertices.data(),
                    sizeof(vertices));
                impl_->context_.get()->Unmap(
                    impl_->vertexBuffer_.Get(),
                    0);
            } else {
                error = hresultText(
                    "ID3D11DeviceContext::Map(vertex)",
                    result);
                errorType = detail::d3d11FailureEvent(result);
            }
        }
        if (error.empty()) {
            D3D11_MAPPED_SUBRESOURCE mapped {};
            const HRESULT result = impl_->context_.get()->Map(
                impl_->colorBuffer_.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped);
            if (SUCCEEDED(result)) {
                std::memcpy(
                    mapped.pData,
                    &uploaded.color,
                    sizeof(uploaded.color));
                impl_->context_.get()->Unmap(
                    impl_->colorBuffer_.Get(),
                    0);
            } else {
                error = hresultText(
                    "ID3D11DeviceContext::Map(color)",
                    result);
                errorType = detail::d3d11FailureEvent(result);
            }
        }
        if (error.empty()) {
            updateMaximum(
                impl_->maximumBufferUpdateMicroseconds_,
                steadyMicroseconds() - bufferUpdateStarted);
        }

        if (error.empty()) {
            const auto drawStarted = steadyMicroseconds();
            constexpr float clearColor[] { 0.0F, 0.0F, 0.0F, 1.0F };
            impl_->context_.get()->ClearRenderTargetView(
                target.view,
                clearColor);
            impl_->context_.get()->OMSetRenderTargets(
                1,
                &target.view,
                nullptr);
            constexpr float blendFactor[] {
                0.0F,
                0.0F,
                0.0F,
                0.0F,
            };
            impl_->context_.get()->OMSetBlendState(
                nullptr,
                blendFactor,
                0xffffffffU);
            impl_->context_.get()->RSSetViewports(1, &viewport);
            impl_->context_.get()->RSSetState(
                impl_->rasterizer_.Get());

            const UINT stride = sizeof(Vertex);
            const UINT offset = 0;
            ID3D11Buffer* vertexBuffer = impl_->vertexBuffer_.Get();
            impl_->context_.get()->IASetInputLayout(
                impl_->inputLayout_.Get());
            impl_->context_.get()->IASetVertexBuffers(
                0,
                1,
                &vertexBuffer,
                &stride,
                &offset);
            impl_->context_.get()->IASetPrimitiveTopology(
                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            impl_->context_.get()->VSSetShader(
                impl_->vertexShader_.Get(),
                nullptr,
                0);
            impl_->context_.get()->HSSetShader(nullptr, nullptr, 0);
            impl_->context_.get()->DSSetShader(nullptr, nullptr, 0);
            impl_->context_.get()->GSSetShader(nullptr, nullptr, 0);
            impl_->context_.get()->PSSetShader(
                impl_->pixelShader_.Get(),
                nullptr,
                0);

            ID3D11ShaderResourceView* views[] {
                uploaded.views[0].Get(),
                uploaded.views[1].Get(),
                uploaded.views[2].Get(),
            };
            ID3D11SamplerState* sampler = impl_->sampler_.Get();
            ID3D11Buffer* colorBuffer = impl_->colorBuffer_.Get();
            impl_->context_.get()->PSSetShaderResources(0, 3, views);
            impl_->context_.get()->PSSetSamplers(0, 1, &sampler);
            impl_->context_.get()->PSSetConstantBuffers(
                0,
                1,
                &colorBuffer);
            impl_->context_.get()->Draw(4, 0);

            ID3D11ShaderResourceView* emptyViews[] {
                nullptr,
                nullptr,
                nullptr,
            };
            impl_->context_.get()->PSSetShaderResources(
                0,
                3,
                emptyViews);
            impl_->context_.get()->OMSetRenderTargets(
                0,
                nullptr,
                nullptr);
            updateMaximum(
                impl_->maximumDrawMicroseconds_,
                steadyMicroseconds() - drawStarted);
            rendered = true;

            const HRESULT drawReason =
                impl_->device_.get()->GetDeviceRemovedReason();
            if (detail::d3d11FailureEvent(drawReason)
                == VideoRenderEventType::SurfaceLost) {
                rendered = false;
                errorType = VideoRenderEventType::SurfaceLost;
                error = hresultText(
                    "The D3D11 device was removed during rendering",
                    drawReason);
            }
        }
    }

    if (advancedColorInfoResolved) {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        if (!sameAdvancedColorInfo(
                impl_->advancedColorInfo_,
                advancedColorInfo)) {
            impl_->advancedColorInfo_ = advancedColorInfo;
        }
    }
    if (!rendered) {
        impl_->notify(errorType, std::move(error));
    } else if (!fallbackDetail.empty()) {
        impl_->notify(
            VideoRenderEventType::Error,
            std::move(fallbackDetail));
    }
    return rendered;
}

void D3D11VideoRenderer::close() noexcept
{
    if (!impl_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        impl_->open_ = false;
    }
    std::lock_guard<std::mutex> lock(impl_->renderMutex_);
    impl_->releasePipeline();
}

BorrowedD3D11Device D3D11VideoRenderer::device() const noexcept
{
    return impl_ ? impl_->device_ : BorrowedD3D11Device {};
}

BorrowedD3D11DeviceContext D3D11VideoRenderer::context() const noexcept
{
    return impl_ ? impl_->context_ : BorrowedD3D11DeviceContext {};
}

std::shared_ptr<D3D11DeviceAccess>
D3D11VideoRenderer::deviceAccess() const noexcept
{
    return impl_ ? impl_->deviceAccess_
                 : std::shared_ptr<D3D11DeviceAccess> {};
}

void D3D11VideoRenderer::setCurrentTargetCallback(
    D3D11CurrentTargetCallback callback)
{
    if (!impl_) {
        return;
    }
    bool requestRedraw = false;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        impl_->currentTarget_ = std::move(callback);
        requestRedraw = impl_->open_;
    }
    if (requestRedraw) {
        impl_->notify(VideoRenderEventType::RedrawRequested, {});
    }
}

void D3D11VideoRenderer::setHardwareFrameInterop(
    std::shared_ptr<D3D11HardwareFrameInterop> hardwareInterop)
{
    if (!impl_) {
        return;
    }
    bool requestRedraw = false;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        impl_->hardwareInterop_ = std::move(hardwareInterop);
        requestRedraw = impl_->open_;
    }
    if (requestRedraw) {
        impl_->notify(VideoRenderEventType::RedrawRequested, {});
    }
}

std::shared_ptr<D3D11HardwareFrameInterop>
D3D11VideoRenderer::hardwareFrameInterop() const noexcept
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex_);
    return impl_->hardwareInterop_;
}

void D3D11VideoRenderer::setAllowSoftwareMappingFallback(
    bool allow) noexcept
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex_);
    impl_->allowSoftwareMappingFallback_ = allow;
}

bool D3D11VideoRenderer::allowSoftwareMappingFallback() const noexcept
{
    if (!impl_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex_);
    return impl_->allowSoftwareMappingFallback_;
}

D3D11AdvancedColorInfo
D3D11VideoRenderer::advancedColorInfo() const noexcept
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex_);
    return impl_->advancedColorInfo_;
}

D3D11VideoRendererStatistics
D3D11VideoRenderer::takeStatistics() noexcept
{
    if (!impl_) {
        return {};
    }
    D3D11VideoRendererStatistics result;
    result.maximumColorSetupMicroseconds =
        impl_->maximumColorSetupMicroseconds_.exchange(0);
    result.maximumInteropMicroseconds =
        impl_->maximumInteropMicroseconds_.exchange(0);
    result.maximumBufferUpdateMicroseconds =
        impl_->maximumBufferUpdateMicroseconds_.exchange(0);
    result.maximumDrawMicroseconds =
        impl_->maximumDrawMicroseconds_.exchange(0);
    return result;
}

} // namespace qtav
