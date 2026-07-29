// SPDX-License-Identifier: LGPL-2.1-or-later

#if !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <qtav/d3d11_video_renderer.h>

#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "d3d11_video_renderer_p.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
    float3 colorPadding;
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

float4 pixelMain(PixelInput input) : SV_TARGET
{
    if (sourceType == 0) {
        return source0.Sample(linearSampler, input.texcoord);
    }
    if (sourceType == 1) {
        const float value = source0.Sample(linearSampler, input.texcoord).r;
        return float4(value, value, value, 1.0);
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
    return float4(
        saturate(dot(colorRow0, sample)),
        saturate(dot(colorRow1, sample)),
        saturate(dot(colorRow2, sample)),
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
    std::array<float, 3> padding {};
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
        return true;
    default:
        return false;
    }
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

bool createPackedView(
    ID3D11Device* device,
    const VideoFrame& frame,
    ComPtr<ID3D11ShaderResourceView>& view,
    std::string& error)
{
    const int width = frame.width();
    const int height = frame.height();
    std::size_t size = 0;
    if (!frame.data() || frame.lineSize() == 0
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
    const std::int64_t strideMagnitude = frame.lineSize() < 0
        ? -static_cast<std::int64_t>(frame.lineSize())
        : static_cast<std::int64_t>(frame.lineSize());
    if (strideMagnitude
        < static_cast<std::int64_t>(width) * sourceBytes) {
        error = "The packed software frame stride is too small";
        return false;
    }

    std::vector<std::uint8_t> rgba(size);
    for (int y = 0; y < height; ++y) {
        const auto* source = frame.data()
            + static_cast<std::ptrdiff_t>(y) * frame.lineSize();
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
    const VideoFrame& frame,
    bool p010,
    ColorParameters& parameters)
{
    double kr = 0.2126;
    double kb = 0.0722;
    const VideoColorSpace color = frame.colorSpaceInfo();
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
        if (frame.width() <= 1024 && frame.height() <= 576) {
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
        setYuvMatrix(frame, false, uploaded.color);
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
        setYuvMatrix(frame, false, uploaded.color);
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
        setYuvMatrix(frame, true, uploaded.color);
        return true;
    default:
        error = "The D3D11 renderer does not support this software pixel format";
        return false;
    }
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

BorrowedD3D11Device::BorrowedD3D11Device(ID3D11Device* value) noexcept
    : value_(value)
{
}

ID3D11Device* BorrowedD3D11Device::get() const noexcept
{
    return value_;
}

BorrowedD3D11Device::operator bool() const noexcept
{
    return value_ != nullptr;
}

BorrowedD3D11DeviceContext::BorrowedD3D11DeviceContext(
    ID3D11DeviceContext* value) noexcept
    : value_(value)
{
}

ID3D11DeviceContext* BorrowedD3D11DeviceContext::get() const noexcept
{
    return value_;
}

BorrowedD3D11DeviceContext::operator bool() const noexcept
{
    return value_ != nullptr;
}

bool D3D11RenderTarget::isValid() const noexcept
{
    return view != nullptr;
}

class D3D11VideoRenderer::Impl {
public:
    Impl(
        BorrowedD3D11Device device,
        BorrowedD3D11DeviceContext context,
        D3D11CurrentTargetCallback currentTarget)
        : device_(device)
        , context_(context)
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
    BorrowedD3D11Device device_;
    BorrowedD3D11DeviceContext context_;
    D3D11CurrentTargetCallback currentTarget_;
    EventCallback eventCallback_;
    VideoRenderConfig config_;
    bool open_ = false;

    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11Buffer> colorBuffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
};

D3D11VideoRenderer::D3D11VideoRenderer(
    BorrowedD3D11Device device,
    BorrowedD3D11DeviceContext context,
    D3D11CurrentTargetCallback currentTarget)
    : impl_(std::make_unique<Impl>(
          device,
          context,
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
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        if (!impl_->open_) {
            currentTarget = {};
        } else {
            config = impl_->config_;
            currentTarget = impl_->currentTarget_;
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
    VideoRenderEventType errorType = VideoRenderEventType::Error;
    bool rendered = false;
    {
        std::lock_guard<std::mutex> lock(impl_->renderMutex_);
        const HRESULT removedReason =
            impl_->device_.get()->GetDeviceRemovedReason();
        if (detail::d3d11FailureEvent(removedReason)
            == VideoRenderEventType::SurfaceLost) {
            errorType = VideoRenderEventType::SurfaceLost;
            error = hresultText(
                "The D3D11 device was removed",
                removedReason);
        }

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

        UploadedFrame uploaded;
        if (error.empty()
            && !uploadFrame(
                impl_->device_.get(),
                frame,
                uploaded,
                error)) {
            const HRESULT uploadReason =
                impl_->device_.get()->GetDeviceRemovedReason();
            errorType = detail::d3d11FailureEvent(uploadReason);
        }

        std::array<Vertex, 4> vertices;
        D3D11_VIEWPORT viewport {};
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

    if (!rendered) {
        impl_->notify(errorType, std::move(error));
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

} // namespace qtav
