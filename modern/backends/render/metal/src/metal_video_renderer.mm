// SPDX-License-Identifier: LGPL-2.1-or-later

#import <qtav/metal_video_renderer.h>

#import <CoreGraphics/CGColorSpace.h>
#import <TargetConditionals.h>

#if TARGET_OS_OSX
#  import <AppKit/NSScreen.h>
#else
#  import <UIKit/UIScreen.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace qtav {
namespace {

enum class ShaderPixelFormat : std::uint32_t {
    YUV420P,
    YUV422P,
    YUV444P,
    NV12,
    NV21,
    P010,
    RGB24,
    BGR24,
    RGBA,
    BGRA,
    ARGB,
    Gray8,
};

enum class ShaderColorMatrix : std::uint32_t {
    BT709,
    BT601,
    BT2020,
};

enum class ShaderColorTransfer : std::uint32_t {
    SDR,
    PQ,
    HLG,
    Linear,
};

enum class ShaderColorPrimaries : std::uint32_t {
    BT709,
    BT2020,
    DisplayP3,
};

struct ShaderParameters {
    std::uint32_t sourceWidth = 0;
    std::uint32_t sourceHeight = 0;
    std::uint32_t format = 0;
    std::uint32_t stride0 = 0;
    std::uint32_t stride1 = 0;
    std::uint32_t stride2 = 0;
    std::uint32_t offset0 = 0;
    std::uint32_t offset1 = 0;
    std::uint32_t offset2 = 0;
    std::uint32_t surfaceWidth = 0;
    std::uint32_t surfaceHeight = 0;
    std::uint32_t viewportX = 0;
    std::uint32_t viewportY = 0;
    std::uint32_t viewportWidth = 0;
    std::uint32_t viewportHeight = 0;
    std::uint32_t rotation = 0;
    std::uint32_t aspectRatio = 0;
    std::uint32_t colorMatrix = 0;
    std::uint32_t colorRange = 0;
    std::uint32_t colorTransfer = 0;
    std::uint32_t colorPrimaries = 0;
    std::uint32_t outputColorSpace = 0;
    std::uint32_t edrToneMapping = 0;
    float referenceWhiteNits = 100.0F;
    float maximumLuminanceNits = 100.0F;
    float edrHeadroom = 1.0F;
};

struct PackedFrame {
    std::vector<std::uint8_t> bytes;
    ShaderParameters parameters;
};

constexpr const char* shaderSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct Parameters {
    uint sourceWidth;
    uint sourceHeight;
    uint format;
    uint stride0;
    uint stride1;
    uint stride2;
    uint offset0;
    uint offset1;
    uint offset2;
    uint surfaceWidth;
    uint surfaceHeight;
    uint viewportX;
    uint viewportY;
    uint viewportWidth;
    uint viewportHeight;
    uint rotation;
    uint aspectRatio;
    uint colorMatrix;
    uint colorRange;
    uint colorTransfer;
    uint colorPrimaries;
    uint outputColorSpace;
    uint edrToneMapping;
    float referenceWhiteNits;
    float maximumLuminanceNits;
    float edrHeadroom;
};

struct VertexOutput {
    float4 position [[position]];
};

vertex VertexOutput vertexMain(uint vertexId [[vertex_id]])
{
    constexpr float2 positions[] = {
        float2(-1.0, -1.0),
        float2( 1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0,  1.0),
    };
    VertexOutput result;
    result.position = float4(positions[vertexId], 0.0, 1.0);
    return result;
}

float byteValue(
    device const uchar* bytes,
    uint offset,
    uint stride,
    uint x,
    uint y)
{
    return float(bytes[offset + y * stride + x]) / 255.0;
}

float ushortValue(
    device const uchar* bytes,
    uint offset,
    uint stride,
    uint x,
    uint y)
{
    const uint index = offset + y * stride + x * 2;
    const uint value = uint(bytes[index]) | (uint(bytes[index + 1]) << 8);
    return float(value) / 65535.0;
}

float3 yuvToRgb(
    float y,
    float u,
    float v,
    uint matrix,
    uint range)
{
    if (range == 1) {
        u -= 0.5;
        v -= 0.5;
    } else {
        y = (y - 16.0 / 255.0) * (255.0 / 219.0);
        u = (u - 128.0 / 255.0) * (255.0 / 224.0);
        v = (v - 128.0 / 255.0) * (255.0 / 224.0);
    }

    float kr = 0.2126;
    float kb = 0.0722;
    if (matrix == 1) {
        kr = 0.2990;
        kb = 0.1140;
    } else if (matrix == 2) {
        kr = 0.2627;
        kb = 0.0593;
    }
    const float kg = 1.0 - kr - kb;
    return float3(
        y + 2.0 * (1.0 - kr) * v,
        y - 2.0 * kb * (1.0 - kb) / kg * u
            - 2.0 * kr * (1.0 - kr) / kg * v,
        y + 2.0 * (1.0 - kb) * u);
}

float3 p010ToRgb(
    float y,
    float u,
    float v,
    uint matrix,
    uint range)
{
    constexpr float normalizedCodeScale = 65535.0 / 65472.0;
    y *= normalizedCodeScale;
    u *= normalizedCodeScale;
    v *= normalizedCodeScale;
    if (range == 1) {
        u -= 512.0 / 1023.0;
        v -= 512.0 / 1023.0;
    } else {
        y = (y - 64.0 / 1023.0) * (1023.0 / 876.0);
        u = (u - 512.0 / 1023.0) * (1023.0 / 896.0);
        v = (v - 512.0 / 1023.0) * (1023.0 / 896.0);
    }

    float kr = 0.2126;
    float kb = 0.0722;
    if (matrix == 1) {
        kr = 0.2990;
        kb = 0.1140;
    } else if (matrix == 2) {
        kr = 0.2627;
        kb = 0.0593;
    }
    const float kg = 1.0 - kr - kb;
    return float3(
        y + 2.0 * (1.0 - kr) * v,
        y - 2.0 * kb * (1.0 - kb) / kg * u
            - 2.0 * kr * (1.0 - kr) / kg * v,
        y + 2.0 * (1.0 - kb) * u);
}

float3 pqToNits(float3 value)
{
    constexpr float m1 = 2610.0 / 16384.0;
    constexpr float m2 = 2523.0 / 32.0;
    constexpr float c1 = 3424.0 / 4096.0;
    constexpr float c2 = 2413.0 / 128.0;
    constexpr float c3 = 2392.0 / 128.0;
    const float3 power = pow(max(value, 0.0), 1.0 / m2);
    const float3 numerator = max(power - c1, 0.0);
    const float3 denominator = max(c2 - c3 * power, 0.000001);
    return 10000.0 * pow(numerator / denominator, 1.0 / m1);
}

float3 hlgToNits(float3 value)
{
    constexpr float a = 0.17883277;
    constexpr float b = 0.28466892;
    constexpr float c = 0.55991073;
    const float3 low = value * value / 3.0;
    const float3 high = (exp((value - c) / a) + b) / 12.0;
    return 1000.0 * select(low, high, value > 0.5);
}

float3 sdrToLinear(float3 value)
{
    const float3 low = value / 4.5;
    const float3 high = pow((value + 0.099) / 1.099, 1.0 / 0.45);
    return select(low, high, value >= 0.081);
}

float3 linearToSrgb(float3 value)
{
    const float3 low = 12.92 * value;
    const float3 high = 1.055 * pow(max(value, 0.0), 1.0 / 2.4) - 0.055;
    return select(low, high, value > 0.0031308);
}

float3 convertPrimaries(float3 value, uint primaries, uint outputColorSpace)
{
    if (outputColorSpace == 2) {
        if (primaries == 0) {
            return float3(
                0.627404 * value.r + 0.329283 * value.g + 0.043313 * value.b,
                0.069097 * value.r + 0.919540 * value.g + 0.011362 * value.b,
                0.016391 * value.r + 0.088013 * value.g + 0.895595 * value.b);
        }
        if (primaries == 2) {
            return float3(
                0.753833 * value.r + 0.198597 * value.g + 0.047570 * value.b,
                0.045744 * value.r + 0.941777 * value.g + 0.012479 * value.b,
                -0.001210 * value.r + 0.017602 * value.g + 0.983609 * value.b);
        }
        return value;
    }

    if (primaries == 1) {
        return float3(
            1.660491 * value.r - 0.587641 * value.g - 0.072850 * value.b,
            -0.124550 * value.r + 1.132900 * value.g - 0.008349 * value.b,
            -0.018151 * value.r - 0.100579 * value.g + 1.118730 * value.b);
    }
    if (primaries == 2) {
        return float3(
            1.224940 * value.r - 0.224940 * value.g,
            -0.042057 * value.r + 1.042057 * value.g,
            -0.019638 * value.r - 0.078636 * value.g + 1.098274 * value.b);
    }
    return value;
}

float3 toneMapToHeadroom(
    float3 relative,
    constant Parameters& parameters)
{
    if (parameters.edrToneMapping == 0) {
        return relative;
    }

    const float targetPeak = max(parameters.edrHeadroom, 1.0);
    const float sourcePeak = max(
        parameters.maximumLuminanceNits / parameters.referenceWhiteNits,
        1.0);
    const float pixelPeak = max(relative.r, max(relative.g, relative.b));
    if (sourcePeak <= targetPeak) {
        return min(relative, targetPeak);
    }
    if (targetPeak <= 1.000001) {
        const float sourceScale = sourcePeak / (1.0 + sourcePeak);
        const float mappedPeak = min(
            (pixelPeak / (1.0 + pixelPeak)) / sourceScale,
            1.0);
        return pixelPeak > 0.0
            ? relative * (mappedPeak / pixelPeak)
            : relative;
    }
    if (pixelPeak <= 1.0) {
        return relative;
    }

    const float normalized = clamp(
        (pixelPeak - 1.0) / max(sourcePeak - 1.0, 0.000001),
        0.0,
        1.0);
    const float mappedPeak = 1.0 + normalized * (targetPeak - 1.0);
    return relative * (mappedPeak / pixelPeak);
}

float3 presentColor(float3 nonlinear, constant Parameters& parameters)
{
    const bool hdr = parameters.colorTransfer == 1
        || parameters.colorTransfer == 2;
    if (parameters.outputColorSpace == 0 && !hdr
        && parameters.colorPrimaries == 0) {
        return clamp(nonlinear, 0.0, 1.0);
    }

    float3 linearNits;
    if (parameters.colorTransfer == 1) {
        linearNits = pqToNits(nonlinear);
    } else if (parameters.colorTransfer == 2) {
        linearNits = hlgToNits(nonlinear);
    } else if (parameters.colorTransfer == 3) {
        linearNits = max(nonlinear, 0.0) * parameters.referenceWhiteNits;
    } else {
        linearNits =
            sdrToLinear(max(nonlinear, 0.0)) * parameters.referenceWhiteNits;
    }
    linearNits = max(convertPrimaries(
        linearNits,
        parameters.colorPrimaries,
        parameters.outputColorSpace), 0.0);
    if (parameters.outputColorSpace != 0) {
        return toneMapToHeadroom(
            linearNits / parameters.referenceWhiteNits,
            parameters);
    }

    const float peak = max(
        parameters.maximumLuminanceNits,
        parameters.referenceWhiteNits);
    const float3 relative = linearNits / parameters.referenceWhiteNits;
    const float white = peak / parameters.referenceWhiteNits;
    const float3 mapped =
        relative * (1.0 + relative / (white * white))
        / (1.0 + relative);
    return clamp(linearToSrgb(mapped), 0.0, 1.0);
}

bool sourceCoordinate(
    float2 pixel,
    constant Parameters& parameters,
    thread float2& source)
{
    const float2 viewportOrigin =
        float2(parameters.viewportX, parameters.viewportY);
    const float2 viewportSize =
        float2(parameters.viewportWidth, parameters.viewportHeight);
    if (any(pixel < viewportOrigin)
        || any(pixel >= viewportOrigin + viewportSize)) {
        return false;
    }

    float2 destination = (pixel - viewportOrigin) / viewportSize;
    const bool swapsAxes = parameters.rotation == 1
        || parameters.rotation == 3;
    const float2 rotatedSize = swapsAxes
        ? float2(parameters.sourceHeight, parameters.sourceWidth)
        : float2(parameters.sourceWidth, parameters.sourceHeight);
    const float sourceAspect = rotatedSize.x / rotatedSize.y;
    const float viewportAspect = viewportSize.x / viewportSize.y;

    if (parameters.aspectRatio == 0) {
        if (viewportAspect > sourceAspect) {
            const float width = sourceAspect / viewportAspect;
            const float left = (1.0 - width) * 0.5;
            if (destination.x < left || destination.x >= left + width) {
                return false;
            }
            destination.x = (destination.x - left) / width;
        } else {
            const float height = viewportAspect / sourceAspect;
            const float top = (1.0 - height) * 0.5;
            if (destination.y < top || destination.y >= top + height) {
                return false;
            }
            destination.y = (destination.y - top) / height;
        }
    } else if (parameters.aspectRatio == 1) {
        if (viewportAspect > sourceAspect) {
            const float height = sourceAspect / viewportAspect;
            destination.y = (destination.y - 0.5) * height + 0.5;
        } else {
            const float width = viewportAspect / sourceAspect;
            destination.x = (destination.x - 0.5) * width + 0.5;
        }
    }

    if (parameters.rotation == 1) {
        source = float2(destination.y, 1.0 - destination.x);
    } else if (parameters.rotation == 2) {
        source = 1.0 - destination;
    } else if (parameters.rotation == 3) {
        source = float2(1.0 - destination.y, destination.x);
    } else {
        source = destination;
    }
    source = clamp(source, float2(0.0), float2(0.999999));
    return true;
}

fragment float4 fragmentMain(
    VertexOutput input [[stage_in]],
    device const uchar* bytes [[buffer(0)]],
    constant Parameters& parameters [[buffer(1)]])
{
    float2 coordinate;
    if (!sourceCoordinate(input.position.xy, parameters, coordinate)) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const uint x = min(
        uint(coordinate.x * parameters.sourceWidth),
        parameters.sourceWidth - 1);
    const uint y = min(
        uint(coordinate.y * parameters.sourceHeight),
        parameters.sourceHeight - 1);

    float3 rgb;
    if (parameters.format <= 5) {
        float luma;
        float chromaU;
        float chromaV;
        if (parameters.format == 5) {
            luma = ushortValue(
                bytes, parameters.offset0, parameters.stride0, x, y);
            const uint chromaX = x / 2;
            const uint chromaY = y / 2;
            chromaU = ushortValue(
                bytes,
                parameters.offset1,
                parameters.stride1,
                chromaX * 2,
                chromaY);
            chromaV = ushortValue(
                bytes,
                parameters.offset1,
                parameters.stride1,
                chromaX * 2 + 1,
                chromaY);
            rgb = p010ToRgb(
                luma,
                chromaU,
                chromaV,
                parameters.colorMatrix,
                parameters.colorRange);
        } else {
            luma = byteValue(
                bytes, parameters.offset0, parameters.stride0, x, y);
            uint chromaX = x;
            uint chromaY = y;
            if (parameters.format == 0) {
                chromaX /= 2;
                chromaY /= 2;
            } else if (parameters.format == 1) {
                chromaX /= 2;
            } else if (parameters.format == 3
                || parameters.format == 4) {
                chromaX /= 2;
                chromaY /= 2;
            }

            if (parameters.format <= 2) {
                chromaU = byteValue(
                    bytes,
                    parameters.offset1,
                    parameters.stride1,
                    chromaX,
                    chromaY);
                chromaV = byteValue(
                    bytes,
                    parameters.offset2,
                    parameters.stride2,
                    chromaX,
                    chromaY);
            } else {
                const uint first = chromaX * 2;
                const float a = byteValue(
                    bytes,
                    parameters.offset1,
                    parameters.stride1,
                    first,
                    chromaY);
                const float b = byteValue(
                    bytes,
                    parameters.offset1,
                    parameters.stride1,
                    first + 1,
                    chromaY);
                chromaU = parameters.format == 3 ? a : b;
                chromaV = parameters.format == 3 ? b : a;
            }
            rgb = yuvToRgb(
                luma,
                chromaU,
                chromaV,
                parameters.colorMatrix,
                parameters.colorRange);
        }
    } else {
        const uint index = parameters.offset0 + y * parameters.stride0;
        if (parameters.format == 6) {
            rgb = float3(
                bytes[index + x * 3],
                bytes[index + x * 3 + 1],
                bytes[index + x * 3 + 2]) / 255.0;
        } else if (parameters.format == 7) {
            rgb = float3(
                bytes[index + x * 3 + 2],
                bytes[index + x * 3 + 1],
                bytes[index + x * 3]) / 255.0;
        } else if (parameters.format == 8) {
            rgb = float3(
                bytes[index + x * 4],
                bytes[index + x * 4 + 1],
                bytes[index + x * 4 + 2]) / 255.0;
        } else if (parameters.format == 9) {
            rgb = float3(
                bytes[index + x * 4 + 2],
                bytes[index + x * 4 + 1],
                bytes[index + x * 4]) / 255.0;
        } else if (parameters.format == 10) {
            rgb = float3(
                bytes[index + x * 4 + 1],
                bytes[index + x * 4 + 2],
                bytes[index + x * 4 + 3]) / 255.0;
        } else {
            const float gray = float(bytes[index + x]) / 255.0;
            rgb = float3(gray);
        }
    }
    return float4(presentColor(rgb, parameters), 1.0);
}

fragment float4 fragmentTextureMain(
    VertexOutput input [[stage_in]],
    texture2d<float> lumaTexture [[texture(0)]],
    texture2d<float> chromaTexture [[texture(1)]],
    constant Parameters& parameters [[buffer(1)]])
{
    float2 coordinate;
    if (!sourceCoordinate(input.position.xy, parameters, coordinate)) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    constexpr sampler nearestSampler(
        coord::normalized,
        address::clamp_to_edge,
        filter::nearest);
    const float luma = lumaTexture.sample(nearestSampler, coordinate).r;
    const float2 chroma =
        chromaTexture.sample(nearestSampler, coordinate).rg;
    const float3 rgb = parameters.format == 5
        ? p010ToRgb(
            luma,
            chroma.r,
            chroma.g,
            parameters.colorMatrix,
            parameters.colorRange)
        : yuvToRgb(
            luma,
            chroma.r,
            chroma.g,
            parameters.colorMatrix,
            parameters.colorRange);
    return float4(presentColor(rgb, parameters), 1.0);
}
)METAL";

bool checkedUint(std::size_t value, std::uint32_t& result) noexcept
{
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    result = static_cast<std::uint32_t>(value);
    return true;
}

bool validViewport(
    const VideoViewport& viewport,
    const VideoSize& surface) noexcept
{
    if (!viewport.isValid()) {
        return true;
    }
    return viewport.x >= 0 && viewport.y >= 0
        && viewport.x <= surface.width - viewport.width
        && viewport.y <= surface.height - viewport.height;
}

bool supportedConfig(const VideoRenderConfig& config) noexcept
{
    return config.surfaceSize.isValid()
        && validViewport(config.viewport, config.surfaceSize)
        && config.deviceOwnership == NativeResourceOwnership::Borrowed
        && config.contextOwnership == NativeResourceOwnership::Borrowed
        && config.surfaceOwnership == NativeResourceOwnership::Borrowed;
}

VideoViewport effectiveViewport(const VideoRenderConfig& config) noexcept
{
    return config.viewport.isValid()
        ? config.viewport
        : VideoViewport {
              0,
              0,
              config.surfaceSize.width,
              config.surfaceSize.height,
          };
}

bool pixelFormat(
    PixelFormat source,
    ShaderPixelFormat& destination,
    int& planeCount,
    std::array<int, 3>& horizontalDivisors,
    std::array<int, 3>& verticalDivisors,
    std::array<int, 3>& bytesPerElement) noexcept
{
    horizontalDivisors = { 1, 1, 1 };
    verticalDivisors = { 1, 1, 1 };
    bytesPerElement = { 1, 1, 1 };
    switch (source) {
    case PixelFormat::YUV420P:
        destination = ShaderPixelFormat::YUV420P;
        planeCount = 3;
        horizontalDivisors = { 1, 2, 2 };
        verticalDivisors = { 1, 2, 2 };
        return true;
    case PixelFormat::YUV422P:
        destination = ShaderPixelFormat::YUV422P;
        planeCount = 3;
        horizontalDivisors = { 1, 2, 2 };
        return true;
    case PixelFormat::YUV444P:
        destination = ShaderPixelFormat::YUV444P;
        planeCount = 3;
        return true;
    case PixelFormat::NV12:
        destination = ShaderPixelFormat::NV12;
        planeCount = 2;
        horizontalDivisors = { 1, 2, 1 };
        verticalDivisors = { 1, 2, 1 };
        bytesPerElement = { 1, 2, 1 };
        return true;
    case PixelFormat::NV21:
        destination = ShaderPixelFormat::NV21;
        planeCount = 2;
        horizontalDivisors = { 1, 2, 1 };
        verticalDivisors = { 1, 2, 1 };
        bytesPerElement = { 1, 2, 1 };
        return true;
    case PixelFormat::P010:
        destination = ShaderPixelFormat::P010;
        planeCount = 2;
        horizontalDivisors = { 1, 2, 1 };
        verticalDivisors = { 1, 2, 1 };
        bytesPerElement = { 2, 4, 1 };
        return true;
    case PixelFormat::RGB24:
        destination = ShaderPixelFormat::RGB24;
        planeCount = 1;
        bytesPerElement[0] = 3;
        return true;
    case PixelFormat::BGR24:
        destination = ShaderPixelFormat::BGR24;
        planeCount = 1;
        bytesPerElement[0] = 3;
        return true;
    case PixelFormat::RGBA:
        destination = ShaderPixelFormat::RGBA;
        planeCount = 1;
        bytesPerElement[0] = 4;
        return true;
    case PixelFormat::BGRA:
        destination = ShaderPixelFormat::BGRA;
        planeCount = 1;
        bytesPerElement[0] = 4;
        return true;
    case PixelFormat::ARGB:
        destination = ShaderPixelFormat::ARGB;
        planeCount = 1;
        bytesPerElement[0] = 4;
        return true;
    case PixelFormat::Gray8:
        destination = ShaderPixelFormat::Gray8;
        planeCount = 1;
        return true;
    default:
        return false;
    }
}

bool appendPlane(
    const std::uint8_t* data,
    int lineSize,
    int rowBytes,
    int height,
    std::vector<std::uint8_t>& destination,
    std::uint32_t& offset) noexcept
{
    if (!data || lineSize == 0 || rowBytes <= 0 || height <= 0
        || std::abs(static_cast<long long>(lineSize)) < rowBytes
        || !checkedUint(destination.size(), offset)) {
        return false;
    }
    const std::size_t originalSize = destination.size();
    const std::size_t planeSize =
        static_cast<std::size_t>(rowBytes) * static_cast<std::size_t>(height);
    if (planeSize > std::numeric_limits<std::uint32_t>::max()
        || originalSize
            > std::numeric_limits<std::uint32_t>::max() - planeSize) {
        return false;
    }
    destination.resize(originalSize + planeSize);
    for (int row = 0; row < height; ++row) {
        std::memcpy(
            destination.data() + originalSize
                + static_cast<std::size_t>(row) * rowBytes,
            data + static_cast<std::ptrdiff_t>(row) * lineSize,
            static_cast<std::size_t>(rowBytes));
    }
    return true;
}

ShaderColorMatrix shaderColorMatrix(ColorMatrix matrix) noexcept
{
    switch (matrix) {
    case ColorMatrix::BT709:
        return ShaderColorMatrix::BT709;
    case ColorMatrix::BT2020NCL:
    case ColorMatrix::BT2020CL:
        return ShaderColorMatrix::BT2020;
    default:
        return ShaderColorMatrix::BT601;
    }
}

ShaderColorTransfer shaderColorTransfer(ColorTransfer transfer) noexcept
{
    switch (transfer) {
    case ColorTransfer::PQ:
        return ShaderColorTransfer::PQ;
    case ColorTransfer::HLG:
        return ShaderColorTransfer::HLG;
    case ColorTransfer::Linear:
        return ShaderColorTransfer::Linear;
    default:
        return ShaderColorTransfer::SDR;
    }
}

ShaderColorPrimaries shaderColorPrimaries(ColorPrimaries primaries) noexcept
{
    switch (primaries) {
    case ColorPrimaries::BT2020:
        return ShaderColorPrimaries::BT2020;
    case ColorPrimaries::SMPTE432:
        return ShaderColorPrimaries::DisplayP3;
    default:
        return ShaderColorPrimaries::BT709;
    }
}

float maximumLuminance(const VideoFrame& frame) noexcept
{
    const MasteringDisplayMetadata mastering =
        frame.masteringDisplayMetadata();
    if (mastering.hasLuminance && mastering.maximumLuminance > 0.0) {
        return static_cast<float>(mastering.maximumLuminance);
    }
    const ContentLightMetadata content = frame.contentLightMetadata();
    if (content.maximumContentLightLevel > 0) {
        return static_cast<float>(content.maximumContentLightLevel);
    }
    return frame.colorSpaceInfo().isHdr() ? 1000.0F : 100.0F;
}

float minimumLuminance(const VideoFrame& frame) noexcept
{
    const MasteringDisplayMetadata mastering =
        frame.masteringDisplayMetadata();
    return mastering.hasLuminance && mastering.minimumLuminance >= 0.0
        ? static_cast<float>(mastering.minimumLuminance)
        : 0.0F;
}

float targetEDRHeadroom(const MetalRenderTarget& target) noexcept
{
    if (std::isfinite(target.currentEDRHeadroom)
        && target.currentEDRHeadroom > 0.0F) {
        return std::max(target.currentEDRHeadroom, 1.0F);
    }
    return metalCurrentEDRHeadroom(target.display);
}

void fillPresentationParameters(
    const VideoFrame& frame,
    ShaderPixelFormat format,
    const VideoRenderConfig& config,
    const MetalRenderTarget& target,
    ColorRange interopRange,
    ShaderParameters& parameters) noexcept
{
    parameters.sourceWidth = static_cast<std::uint32_t>(frame.width());
    parameters.sourceHeight = static_cast<std::uint32_t>(frame.height());
    parameters.format = static_cast<std::uint32_t>(format);

    const VideoViewport viewport = effectiveViewport(config);
    parameters.surfaceWidth =
        static_cast<std::uint32_t>(config.surfaceSize.width);
    parameters.surfaceHeight =
        static_cast<std::uint32_t>(config.surfaceSize.height);
    parameters.viewportX = static_cast<std::uint32_t>(viewport.x);
    parameters.viewportY = static_cast<std::uint32_t>(viewport.y);
    parameters.viewportWidth = static_cast<std::uint32_t>(viewport.width);
    parameters.viewportHeight = static_cast<std::uint32_t>(viewport.height);
    parameters.rotation = static_cast<std::uint32_t>(config.rotation);
    parameters.aspectRatio =
        static_cast<std::uint32_t>(config.aspectRatio);
    const VideoColorSpace color = frame.colorSpaceInfo();
    const ColorRange range = interopRange != ColorRange::Unknown
        ? interopRange
        : color.range;
    parameters.colorMatrix =
        static_cast<std::uint32_t>(shaderColorMatrix(color.matrix));
    parameters.colorRange =
        range == ColorRange::Full ? 1U : 0U;
    parameters.colorTransfer =
        static_cast<std::uint32_t>(shaderColorTransfer(color.transfer));
    parameters.colorPrimaries =
        static_cast<std::uint32_t>(shaderColorPrimaries(color.primaries));
    parameters.outputColorSpace = target.outputColorSpace
            == MetalOutputColorSpace::ExtendedLinearSRGB
        ? 1U
        : target.outputColorSpace
                == MetalOutputColorSpace::ExtendedLinearBT2020
            ? 2U
            : 0U;
    parameters.edrToneMapping =
        target.edrToneMapping == MetalEDRToneMapping::DisplayAdaptive
        && target.outputColorSpace != MetalOutputColorSpace::SDR
        ? 1U
        : 0U;
    parameters.referenceWhiteNits = target.referenceWhiteNits;
    parameters.maximumLuminanceNits = maximumLuminance(frame);
    parameters.edrHeadroom = targetEDRHeadroom(target);
}

bool packFrame(
    const VideoFrame& frame,
    const VideoRenderConfig& config,
    const MetalRenderTarget& target,
    PackedFrame& result,
    std::string& error)
{
    ShaderPixelFormat format {};
    int planeCount = 0;
    std::array<int, 3> horizontalDivisors {};
    std::array<int, 3> verticalDivisors {};
    std::array<int, 3> bytesPerElement {};
    if (!frame
        || !pixelFormat(
            frame.format(),
            format,
            planeCount,
            horizontalDivisors,
            verticalDivisors,
            bytesPerElement)) {
        error = "The Metal renderer does not support this software pixel format";
        return false;
    }
    if (frame.format() == PixelFormat::P010
        && frame.formatName().find("p010le") == std::string::npos) {
        error = "The Metal renderer currently supports little-endian P010";
        return false;
    }

    fillPresentationParameters(
        frame,
        format,
        config,
        target,
        ColorRange::Unknown,
        result.parameters);

    std::array<std::uint32_t*, 3> strides {
        &result.parameters.stride0,
        &result.parameters.stride1,
        &result.parameters.stride2,
    };
    std::array<std::uint32_t*, 3> offsets {
        &result.parameters.offset0,
        &result.parameters.offset1,
        &result.parameters.offset2,
    };
    for (int plane = 0; plane < planeCount; ++plane) {
        const int width =
            (frame.width() + horizontalDivisors[plane] - 1)
            / horizontalDivisors[plane];
        const int height =
            (frame.height() + verticalDivisors[plane] - 1)
            / verticalDivisors[plane];
        const int rowBytes = width * bytesPerElement[plane];
        *strides[plane] = static_cast<std::uint32_t>(rowBytes);
        if (!appendPlane(
                frame.data(plane),
                frame.lineSize(plane),
                rowBytes,
                height,
                result.bytes,
                *offsets[plane])) {
            error = "The Metal renderer could not copy a software frame plane";
            return false;
        }
    }

    return true;
}

bool prepareTextureFrame(
    const VideoFrame& frame,
    const MetalTextureFrame& textureFrame,
    const VideoRenderConfig& config,
    const MetalRenderTarget& target,
    ShaderParameters& parameters,
    std::string& error)
{
    ShaderPixelFormat shaderFormat {};
    if (textureFrame.width() != frame.width()
        || textureFrame.height() != frame.height()
        || textureFrame.planeCount() != 2
        || (textureFrame.format() != PixelFormat::NV12
            && textureFrame.format() != PixelFormat::P010)) {
        error = "Metal hardware interop returned an unsupported texture frame";
        return false;
    }

    id<MTLTexture> luma = textureFrame.texture(0);
    id<MTLTexture> chroma = textureFrame.texture(1);
    const bool nv12 = textureFrame.format() == PixelFormat::NV12;
    if (!luma || !chroma
        || luma.width != static_cast<NSUInteger>(frame.width())
        || luma.height != static_cast<NSUInteger>(frame.height())
        || chroma.width
            != static_cast<NSUInteger>((frame.width() + 1) / 2)
        || chroma.height
            != static_cast<NSUInteger>((frame.height() + 1) / 2)
        || luma.pixelFormat
            != (nv12 ? MTLPixelFormatR8Unorm : MTLPixelFormatR16Unorm)
        || chroma.pixelFormat
            != (nv12 ? MTLPixelFormatRG8Unorm : MTLPixelFormatRG16Unorm)) {
        error = "Metal hardware interop returned invalid plane textures";
        return false;
    }

    shaderFormat = nv12
        ? ShaderPixelFormat::NV12
        : ShaderPixelFormat::P010;
    fillPresentationParameters(
        frame,
        shaderFormat,
        config,
        target,
        textureFrame.colorRange(),
        parameters);
    return true;
}

std::uint16_t metadataChromaticity(double value) noexcept
{
    return static_cast<std::uint16_t>(std::clamp(
        std::llround(value * 50000.0),
        0LL,
        static_cast<long long>(std::numeric_limits<std::uint16_t>::max())));
}

std::uint32_t metadataLuminance(double value) noexcept
{
    return static_cast<std::uint32_t>(std::clamp(
        std::llround(value * 10000.0),
        0LL,
        static_cast<long long>(std::numeric_limits<std::uint32_t>::max())));
}

void writeBigEndian16(
    std::uint8_t* destination,
    std::uint16_t value) noexcept
{
    destination[0] = static_cast<std::uint8_t>(value >> 8);
    destination[1] = static_cast<std::uint8_t>(value);
}

void writeBigEndian32(
    std::uint8_t* destination,
    std::uint32_t value) noexcept
{
    destination[0] = static_cast<std::uint8_t>(value >> 24);
    destination[1] = static_cast<std::uint8_t>(value >> 16);
    destination[2] = static_cast<std::uint8_t>(value >> 8);
    destination[3] = static_cast<std::uint8_t>(value);
}

CAEDRMetadata* hdr10Metadata(
    const VideoFrame& frame,
    float referenceWhiteNits)
{
    const MasteringDisplayMetadata mastering =
        frame.masteringDisplayMetadata();
    const ContentLightMetadata content = frame.contentLightMetadata();
    if (mastering.hasPrimaries && mastering.hasLuminance) {
        std::array<std::uint8_t, 24> displayBytes {};
        std::size_t offset = 0;
        constexpr std::array<std::size_t, 3> SeiPrimaryOrder { 1, 2, 0 };
        for (const std::size_t index : SeiPrimaryOrder) {
            const Chromaticity& primary = mastering.primaries[index];
            writeBigEndian16(
                displayBytes.data() + offset,
                metadataChromaticity(primary.x));
            writeBigEndian16(
                displayBytes.data() + offset + 2,
                metadataChromaticity(primary.y));
            offset += 4;
        }
        writeBigEndian16(
            displayBytes.data() + offset,
            metadataChromaticity(mastering.whitePoint.x));
        writeBigEndian16(
            displayBytes.data() + offset + 2,
            metadataChromaticity(mastering.whitePoint.y));
        offset += 4;
        writeBigEndian32(
            displayBytes.data() + offset,
            metadataLuminance(mastering.maximumLuminance));
        writeBigEndian32(
            displayBytes.data() + offset + 4,
            metadataLuminance(mastering.minimumLuminance));

        NSData* displayData = [NSData dataWithBytes:displayBytes.data()
                                            length:displayBytes.size()];
        NSData* contentData = nil;
        std::array<std::uint8_t, 4> contentBytes {};
        if (content.isValid()) {
            writeBigEndian16(
                contentBytes.data(),
                static_cast<std::uint16_t>(std::min(
                    content.maximumContentLightLevel,
                    static_cast<std::uint32_t>(
                        std::numeric_limits<std::uint16_t>::max()))));
            writeBigEndian16(
                contentBytes.data() + 2,
                static_cast<std::uint16_t>(std::min(
                    content.maximumFrameAverageLightLevel,
                    static_cast<std::uint32_t>(
                        std::numeric_limits<std::uint16_t>::max()))));
            contentData = [NSData dataWithBytes:contentBytes.data()
                                        length:contentBytes.size()];
        }
        return [CAEDRMetadata
            HDR10MetadataWithDisplayInfo:displayData
                             contentInfo:contentData
                      opticalOutputScale:referenceWhiteNits];
    }
    return [CAEDRMetadata
        HDR10MetadataWithMinLuminance:minimumLuminance(frame)
                         maxLuminance:maximumLuminance(frame)
                   opticalOutputScale:referenceWhiteNits];
}

CGColorSpaceRef outputColorSpace(MetalOutputColorSpace output)
{
    const CFStringRef name =
        output == MetalOutputColorSpace::ExtendedLinearBT2020
        ? kCGColorSpaceExtendedLinearITUR_2020
        : output == MetalOutputColorSpace::ExtendedLinearSRGB
            ? kCGColorSpaceExtendedLinearSRGB
            : kCGColorSpaceSRGB;
    return CGColorSpaceCreateWithName(name);
}

bool configureLayer(
    CAMetalLayer* layer,
    id<MTLDevice> device,
    const VideoFrame& frame,
    const VideoRenderConfig& config,
    const MetalRenderTarget& target,
    std::string& error)
{
    if (!layer || !device) {
        error = "The Metal layer or device is unavailable";
        return false;
    }
    if (layer.device && layer.device != device) {
        error = "The current Metal layer belongs to another device";
        return false;
    }

    const bool edr =
        target.outputColorSpace != MetalOutputColorSpace::SDR;
    @try {
        layer.device = device;
        layer.drawableSize = CGSizeMake(
            static_cast<CGFloat>(config.surfaceSize.width),
            static_cast<CGFloat>(config.surfaceSize.height));
        layer.framebufferOnly = YES;
        layer.pixelFormat =
            edr ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;

        CGColorSpaceRef colorSpace =
            outputColorSpace(target.outputColorSpace);
        if (!colorSpace) {
            error = "The requested Metal layer color space is unavailable";
            return false;
        }
        layer.colorspace = colorSpace;
        CGColorSpaceRelease(colorSpace);

#if TARGET_OS_OSX || TARGET_OS_IOS || TARGET_OS_MACCATALYST
#  if TARGET_OS_OSX
        if (@available(macOS 10.15, *)) {
#  else
        if (@available(iOS 16.0, *)) {
#  endif
            layer.wantsExtendedDynamicRangeContent = edr;
            layer.EDRMetadata = nil;
            if (edr
                && target.edrToneMapping == MetalEDRToneMapping::System) {
                const VideoColorSpace color = frame.colorSpaceInfo();
                if (color.transfer == ColorTransfer::PQ) {
                    layer.EDRMetadata =
                        hdr10Metadata(frame, target.referenceWhiteNits);
                } else if (color.transfer == ColorTransfer::HLG) {
                    layer.EDRMetadata = CAEDRMetadata.HLGMetadata;
                }
            }
        } else if (edr) {
            error =
                "Apple EDR Metal layers require macOS 10.15 or iOS 16";
            return false;
        }
#else
        if (edr) {
            error = "Apple EDR Metal layers require macOS or iOS";
            return false;
        }
#endif
    } @catch (NSException* exception) {
        NSString* reason = exception.reason;
        error = "Metal layer EDR configuration failed: "
            + std::string(reason ? reason.UTF8String : "unknown exception");
        return false;
    }
    return true;
}

bool supportedTargetFormat(MTLPixelFormat format) noexcept
{
    return format == MTLPixelFormatRGBA8Unorm
        || format == MTLPixelFormatRGBA8Unorm_sRGB
        || format == MTLPixelFormatBGRA8Unorm
        || format == MTLPixelFormatBGRA8Unorm_sRGB
        || format == MTLPixelFormatRGBA16Float;
}

bool supportedOutput(
    const MetalRenderTarget& target,
    MTLPixelFormat format) noexcept
{
    return std::isfinite(target.referenceWhiteNits)
        && target.referenceWhiteNits > 0.0F
        && std::isfinite(target.currentEDRHeadroom)
        && target.currentEDRHeadroom >= 0.0F
        && (target.outputColorSpace == MetalOutputColorSpace::SDR
            || format == MTLPixelFormatRGBA16Float);
}

std::string nsString(NSString* value)
{
    return value ? std::string(value.UTF8String) : std::string {};
}

} // namespace

float metalCurrentEDRHeadroom(MetalEDRDisplay display) noexcept
{
    @autoreleasepool {
#if TARGET_OS_OSX
        if (![display isKindOfClass:NSScreen.class]) {
            return 1.0F;
        }
        const CGFloat value =
            static_cast<NSScreen*>(display)
                .maximumExtendedDynamicRangeColorComponentValue;
        return std::isfinite(static_cast<double>(value)) && value > 1.0
            ? static_cast<float>(value)
            : 1.0F;
#elif TARGET_OS_IOS || TARGET_OS_MACCATALYST
        if (@available(iOS 16.0, *)) {
            if (![display isKindOfClass:UIScreen.class]) {
                return 1.0F;
            }
            const CGFloat value =
                static_cast<UIScreen*>(display).currentEDRHeadroom;
            return std::isfinite(static_cast<double>(value))
                    && value > 1.0
                ? static_cast<float>(value)
                : 1.0F;
        }
        return 1.0F;
#else
        (void)display;
        return 1.0F;
#endif
    }
}

float metalPotentialEDRHeadroom(MetalEDRDisplay display) noexcept
{
    @autoreleasepool {
#if TARGET_OS_OSX
        if (@available(macOS 10.15, *)) {
            if (![display isKindOfClass:NSScreen.class]) {
                return 1.0F;
            }
            const CGFloat value =
                static_cast<NSScreen*>(display)
                    .maximumPotentialExtendedDynamicRangeColorComponentValue;
            return std::isfinite(static_cast<double>(value)) && value > 1.0
                ? static_cast<float>(value)
                : 1.0F;
        }
        return 1.0F;
#elif TARGET_OS_IOS || TARGET_OS_MACCATALYST
        if (@available(iOS 16.0, *)) {
            if (![display isKindOfClass:UIScreen.class]) {
                return 1.0F;
            }
            const CGFloat value =
                static_cast<UIScreen*>(display).potentialEDRHeadroom;
            return std::isfinite(static_cast<double>(value))
                    && value > 1.0
                ? static_cast<float>(value)
                : 1.0F;
        }
        return 1.0F;
#else
        (void)display;
        return 1.0F;
#endif
    }
}

BorrowedMetalDevice::BorrowedMetalDevice(id<MTLDevice> value) noexcept
    : value_(value)
{
}

id<MTLDevice> BorrowedMetalDevice::get() const noexcept
{
    return value_;
}

BorrowedMetalDevice::operator bool() const noexcept
{
    return value_ != nil;
}

BorrowedMetalCommandQueue::BorrowedMetalCommandQueue(
    id<MTLCommandQueue> value) noexcept
    : value_(value)
{
}

id<MTLCommandQueue> BorrowedMetalCommandQueue::get() const noexcept
{
    return value_;
}

BorrowedMetalCommandQueue::operator bool() const noexcept
{
    return value_ != nil;
}

bool MetalRenderTarget::isValid() const noexcept
{
    const int sourceCount = (texture ? 1 : 0) + (drawable ? 1 : 0)
        + (layer ? 1 : 0);
    return sourceCount == 1
        && std::isfinite(referenceWhiteNits)
        && referenceWhiteNits > 0.0F
        && std::isfinite(currentEDRHeadroom)
        && currentEDRHeadroom >= 0.0F;
}

MetalTextureFrame::~MetalTextureFrame() = default;
MetalHardwareFrameInterop::~MetalHardwareFrameInterop() = default;

class MetalVideoRenderer::Impl {
public:
    Impl(
        BorrowedMetalDevice device,
        BorrowedMetalCommandQueue commandQueue,
        MetalCurrentTargetCallback currentTarget,
        std::shared_ptr<MetalHardwareFrameInterop> hardwareInterop)
        : device_(device)
        , commandQueue_(commandQueue)
        , currentTarget_(std::move(currentTarget))
        , hardwareInterop_(std::move(hardwareInterop))
    {
    }

    void notify(VideoRenderEventType type, std::string detail)
    {
        EventCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = eventCallback_;
        }
        if (callback) {
            callback({ type, std::move(detail) });
        }
    }

    bool makeLibrary(std::string& error)
    {
        if (library_) {
            return true;
        }
        NSError* nativeError = nil;
        NSString* source = [NSString stringWithUTF8String:shaderSource];
        library_ = [device_.get()
            newLibraryWithSource:source
                         options:nil
                           error:&nativeError];
        if (!library_) {
            error = "Metal shader compilation failed: "
                + nsString(nativeError.localizedDescription);
            return false;
        }
        return true;
    }

    bool makePipeline(
        MTLPixelFormat format,
        bool hardware,
        std::string& error)
    {
        id<MTLRenderPipelineState> cached =
            hardware ? hardwarePipeline_ : softwarePipeline_;
        const MTLPixelFormat cachedFormat =
            hardware ? hardwarePipelineFormat_ : softwarePipelineFormat_;
        if (cached && cachedFormat == format) {
            return true;
        }
        id<MTLFunction> vertex = [library_ newFunctionWithName:@"vertexMain"];
        id<MTLFunction> fragment = [library_ newFunctionWithName:
            hardware ? @"fragmentTextureMain" : @"fragmentMain"];
        if (!vertex || !fragment) {
            error = "Metal shader entry points are unavailable";
            return false;
        }

        MTLRenderPipelineDescriptor* descriptor =
            [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = fragment;
        descriptor.colorAttachments[0].pixelFormat = format;

        NSError* nativeError = nil;
        id<MTLRenderPipelineState> pipeline =
            [device_.get() newRenderPipelineStateWithDescriptor:descriptor
                                                          error:&nativeError];
        if (!pipeline) {
            error = "Metal pipeline creation failed: "
                + nsString(nativeError.localizedDescription);
            return false;
        }
        if (hardware) {
            hardwarePipeline_ = pipeline;
            hardwarePipelineFormat_ = format;
        } else {
            softwarePipeline_ = pipeline;
            softwarePipelineFormat_ = format;
        }
        return true;
    }

    mutable std::mutex mutex_;
    BorrowedMetalDevice device_;
    BorrowedMetalCommandQueue commandQueue_;
    MetalCurrentTargetCallback currentTarget_;
    std::shared_ptr<MetalHardwareFrameInterop> hardwareInterop_;
    EventCallback eventCallback_;
    VideoRenderConfig config_;
    id<MTLLibrary> library_ = nil;
    id<MTLRenderPipelineState> softwarePipeline_ = nil;
    id<MTLRenderPipelineState> hardwarePipeline_ = nil;
    MTLPixelFormat softwarePipelineFormat_ = MTLPixelFormatInvalid;
    MTLPixelFormat hardwarePipelineFormat_ = MTLPixelFormatInvalid;
    bool open_ = false;
};

MetalVideoRenderer::MetalVideoRenderer(
    BorrowedMetalDevice device,
    BorrowedMetalCommandQueue commandQueue,
    MetalCurrentTargetCallback currentTarget,
    std::shared_ptr<MetalHardwareFrameInterop> hardwareInterop)
    : impl_(std::make_unique<Impl>(
          device,
          commandQueue,
          std::move(currentTarget),
          std::move(hardwareInterop)))
{
}

MetalVideoRenderer::~MetalVideoRenderer() = default;
MetalVideoRenderer::MetalVideoRenderer(MetalVideoRenderer&&) noexcept = default;
MetalVideoRenderer& MetalVideoRenderer::operator=(
    MetalVideoRenderer&&) noexcept = default;

VideoRenderCapabilities MetalVideoRenderer::capabilities() const
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
    std::shared_ptr<MetalHardwareFrameInterop> hardwareInterop;
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        hardwareInterop = impl_->hardwareInterop_;
    }
    if (hardwareInterop
        && hardwareInterop->device().get() == impl_->device_.get()) {
        result.hardwareDevices =
            hardwareInterop->capabilities().sourceDevices;
    }
    result.customViewport = true;
    result.rotation = true;
    return result;
}

void MetalVideoRenderer::setEventCallback(EventCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->eventCallback_ = std::move(callback);
}

bool MetalVideoRenderer::open(const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }

    std::string error;
    bool opened = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        if (!impl_->device_ || !impl_->commandQueue_) {
            error = "The Metal renderer requires borrowed device and command queue";
        } else if (impl_->commandQueue_.get().device != impl_->device_.get()) {
            error = "The borrowed Metal command queue belongs to another device";
        } else if (
            impl_->hardwareInterop_
            && impl_->hardwareInterop_->device().get()
                != impl_->device_.get()) {
            error = "The Metal hardware interop belongs to another device";
        } else if (!impl_->currentTarget_) {
            error = "The Metal renderer requires a current-target callback";
        } else if (!supportedConfig(config)) {
            error =
                "The Metal renderer requires a valid borrowed surface configuration";
        } else if (impl_->makeLibrary(error)) {
            impl_->config_ = config;
            impl_->open_ = true;
            opened = true;
        }
    }
    if (!opened) {
        impl_->notify(VideoRenderEventType::Error, std::move(error));
    }
    return opened;
}

bool MetalVideoRenderer::configure(const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }

    bool configured = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        configured = impl_->open_ && supportedConfig(config);
        if (configured) {
            impl_->config_ = config;
        }
    }
    if (!configured) {
        impl_->notify(
            VideoRenderEventType::Error,
            "The Metal renderer is closed or the configuration is invalid");
    } else {
        impl_->notify(VideoRenderEventType::RedrawRequested, {});
    }
    return configured;
}

bool MetalVideoRenderer::render(const VideoFrame& frame)
{
    if (!impl_) {
        return false;
    }

    @autoreleasepool {
        VideoRenderConfig config;
        MetalCurrentTargetCallback currentTarget;
        std::shared_ptr<MetalHardwareFrameInterop> hardwareInterop;
        bool open = false;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex_);
            open = impl_->open_;
            if (open) {
                config = impl_->config_;
                currentTarget = impl_->currentTarget_;
                hardwareInterop = impl_->hardwareInterop_;
            }
        }
        if (!open) {
            impl_->notify(
                VideoRenderEventType::Error,
                "The Metal renderer is not open");
            return false;
        }

        const MetalRenderTarget target = currentTarget
            ? currentTarget()
            : MetalRenderTarget {};
        if (!target.texture && !target.drawable && !target.layer) {
            impl_->notify(
                VideoRenderEventType::SurfaceLost,
                "The current Metal target is unavailable");
            return false;
        }
        if (!target.isValid()) {
            impl_->notify(
                VideoRenderEventType::Error,
                "The current Metal target configuration is invalid");
            return false;
        }

        std::string error;
        id<CAMetalDrawable> drawable = target.drawable;
        if (target.layer) {
            if (!configureLayer(
                    target.layer,
                    impl_->device_.get(),
                    frame,
                    config,
                    target,
                    error)) {
                impl_->notify(
                    VideoRenderEventType::Error,
                    std::move(error));
                return false;
            }
            drawable = [target.layer nextDrawable];
        }
        id<MTLTexture> texture =
            target.texture ? target.texture : drawable.texture;
        if (!texture) {
            impl_->notify(
                VideoRenderEventType::SurfaceLost,
                "The current Metal target is unavailable");
            return false;
        }
        if (texture.width != static_cast<NSUInteger>(config.surfaceSize.width)
            || texture.height
                != static_cast<NSUInteger>(config.surfaceSize.height)
            || !supportedTargetFormat(texture.pixelFormat)
            || !supportedOutput(target, texture.pixelFormat)) {
            impl_->notify(
                VideoRenderEventType::Error,
                "The current Metal texture size, pixel format, or color output is unsupported");
            return false;
        }

        PackedFrame packed;
        ShaderParameters textureParameters;
        std::shared_ptr<MetalTextureFrame> textureFrame;
        const bool hardware = frame.hasHardwareFrame();
        if (hardware) {
            const HardwareFrame nativeFrame = frame.hardwareFrame();
            if (!hardwareInterop
                || hardwareInterop->device().get() != impl_->device_.get()
                || !hardwareInterop->supports(nativeFrame)) {
                impl_->notify(
                    VideoRenderEventType::Error,
                    "The Metal renderer has no interop for this hardware frame");
                return false;
            }
            textureFrame = hardwareInterop->importFrame(nativeFrame);
            if (!textureFrame
                || !prepareTextureFrame(
                    frame,
                    *textureFrame,
                    config,
                    target,
                    textureParameters,
                    error)) {
                if (error.empty()) {
                    error = "Metal hardware-frame import failed";
                }
                impl_->notify(VideoRenderEventType::Error, std::move(error));
                return false;
            }
        } else if (!packFrame(frame, config, target, packed, error)) {
            impl_->notify(VideoRenderEventType::Error, std::move(error));
            return false;
        }

        id<MTLBuffer> source = hardware
            ? nil
            : [impl_->device_.get()
                newBufferWithBytes:packed.bytes.data()
                           length:packed.bytes.size()
                          options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> commandBuffer =
            [impl_->commandQueue_.get() commandBuffer];
        if ((!hardware && !source) || !commandBuffer) {
            impl_->notify(
                VideoRenderEventType::Error,
                "Metal could not allocate the upload or command buffer");
            return false;
        }

        id<MTLRenderPipelineState> pipeline = nil;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex_);
            if (!impl_->makePipeline(
                    texture.pixelFormat,
                    hardware,
                    error)) {
                pipeline = nil;
            } else {
                pipeline = hardware
                    ? impl_->hardwarePipeline_
                    : impl_->softwarePipeline_;
            }
        }
        if (!pipeline) {
            impl_->notify(VideoRenderEventType::Error, std::move(error));
            return false;
        }

        MTLRenderPassDescriptor* pass =
            [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

        id<MTLRenderCommandEncoder> encoder =
            [commandBuffer renderCommandEncoderWithDescriptor:pass];
        if (!encoder) {
            impl_->notify(
                VideoRenderEventType::Error,
                "Metal could not create a render command encoder");
            return false;
        }
        [encoder setRenderPipelineState:pipeline];
        if (hardware) {
            [encoder setFragmentTexture:textureFrame->texture(0) atIndex:0];
            [encoder setFragmentTexture:textureFrame->texture(1) atIndex:1];
            [encoder setFragmentBytes:&textureParameters
                               length:sizeof(textureParameters)
                              atIndex:1];
        } else {
            [encoder setFragmentBuffer:source offset:0 atIndex:0];
            [encoder setFragmentBytes:&packed.parameters
                               length:sizeof(packed.parameters)
                              atIndex:1];
        }
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                    vertexStart:0
                    vertexCount:4];
        [encoder endEncoding];

        if (drawable) {
            [commandBuffer presentDrawable:drawable];
        }
        if (textureFrame) {
            const auto retainedTextureFrame = textureFrame;
            [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>) {
                (void)retainedTextureFrame;
            }];
        }
        [commandBuffer commit];
        if (target.waitUntilCompleted) {
            [commandBuffer waitUntilCompleted];
            if (commandBuffer.status == MTLCommandBufferStatusError) {
                impl_->notify(
                    VideoRenderEventType::Error,
                    "Metal command execution failed: "
                        + nsString(commandBuffer.error.localizedDescription));
                return false;
            }
        }
        return true;
    }
}

void MetalVideoRenderer::close() noexcept
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->open_ = false;
    impl_->softwarePipeline_ = nil;
    impl_->hardwarePipeline_ = nil;
    impl_->softwarePipelineFormat_ = MTLPixelFormatInvalid;
    impl_->hardwarePipelineFormat_ = MTLPixelFormatInvalid;
    impl_->library_ = nil;
}

BorrowedMetalDevice MetalVideoRenderer::device() const noexcept
{
    return impl_ ? impl_->device_ : BorrowedMetalDevice {};
}

BorrowedMetalCommandQueue MetalVideoRenderer::commandQueue() const noexcept
{
    return impl_ ? impl_->commandQueue_ : BorrowedMetalCommandQueue {};
}

void MetalVideoRenderer::setCurrentTargetCallback(
    MetalCurrentTargetCallback callback)
{
    if (!impl_) {
        return;
    }
    bool requestRedraw = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        impl_->currentTarget_ = std::move(callback);
        requestRedraw = impl_->open_;
    }
    if (requestRedraw) {
        impl_->notify(VideoRenderEventType::RedrawRequested, {});
    }
}

void MetalVideoRenderer::setHardwareFrameInterop(
    std::shared_ptr<MetalHardwareFrameInterop> hardwareInterop)
{
    if (!impl_) {
        return;
    }
    bool requestRedraw = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        impl_->hardwareInterop_ = std::move(hardwareInterop);
        requestRedraw = impl_->open_;
    }
    if (requestRedraw) {
        impl_->notify(VideoRenderEventType::RedrawRequested, {});
    }
}

std::shared_ptr<MetalHardwareFrameInterop>
MetalVideoRenderer::hardwareFrameInterop() const noexcept
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return impl_->hardwareInterop_;
}

} // namespace qtav
