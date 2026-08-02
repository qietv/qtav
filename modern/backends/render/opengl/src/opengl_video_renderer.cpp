// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/opengl_video_renderer.h>

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace qtav {
namespace {

constexpr char VertexShader[] = R"qtav(
#version 300 es

void main()
{
    const vec2 positions[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
)qtav";

constexpr char FragmentShader[] = R"qtav(
#version 300 es

#if defined(QTAV_EXTERNAL_OES)
#extension GL_OES_EGL_image_external_essl3 : require
#endif

precision highp float;
precision highp int;
precision highp sampler2D;
precision highp usampler2D;
#if defined(QTAV_EXTERNAL_OES)
precision highp samplerExternalOES;
#endif

layout(location = 0) out vec4 outputColor;

uniform sampler2D plane0;
uniform sampler2D plane1;
uniform sampler2D plane2;
uniform usampler2D p010Luma;
uniform usampler2D p010Chroma;
#if defined(QTAV_EXTERNAL_OES)
uniform samplerExternalOES externalImage;
uniform mat4 externalTransform;
#endif

// width, height, format, output color space/attachment encoding
uniform uvec4 source;
// surface width, height, transfer, primaries
uniform uvec4 surface;
// top-left x, y, width, height
uniform ivec4 viewport;
// rotation, aspect ratio, matrix, range
uniform uvec4 presentation;
// reference white and maximum source luminance
uniform vec2 luminance;

vec3 yuvToRgb(float y, float u, float v, uint matrix, uint range)
{
    if (range == 1U) {
        u -= 0.5;
        v -= 0.5;
    } else {
        y = (y - 16.0 / 255.0) * (255.0 / 219.0);
        u = (u - 128.0 / 255.0) * (255.0 / 224.0);
        v = (v - 128.0 / 255.0) * (255.0 / 224.0);
    }

    float kr = 0.2126;
    float kb = 0.0722;
    if (matrix == 1U) {
        kr = 0.2990;
        kb = 0.1140;
    } else if (matrix == 2U) {
        kr = 0.2627;
        kb = 0.0593;
    }
    float kg = 1.0 - kr - kb;
    return vec3(
        y + 2.0 * (1.0 - kr) * v,
        y - 2.0 * kb * (1.0 - kb) / kg * u
            - 2.0 * kr * (1.0 - kr) / kg * v,
        y + 2.0 * (1.0 - kb) * u);
}

vec3 p010ToRgb(float y, float u, float v, uint matrix, uint range)
{
    const float normalizedCodeScale = 65535.0 / 65472.0;
    y *= normalizedCodeScale;
    u *= normalizedCodeScale;
    v *= normalizedCodeScale;
    if (range == 1U) {
        u -= 512.0 / 1023.0;
        v -= 512.0 / 1023.0;
    } else {
        y = (y - 64.0 / 1023.0) * (1023.0 / 876.0);
        u = (u - 512.0 / 1023.0) * (1023.0 / 896.0);
        v = (v - 512.0 / 1023.0) * (1023.0 / 896.0);
    }
    return yuvToRgb(y, u + 0.5, v + 0.5, matrix, 1U);
}

vec3 pqToNits(vec3 value)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    vec3 power = pow(max(value, vec3(0.0)), vec3(1.0 / m2));
    vec3 numerator = max(power - c1, vec3(0.0));
    vec3 denominator = max(c2 - c3 * power, vec3(0.000001));
    return 10000.0 * pow(numerator / denominator, vec3(1.0 / m1));
}

vec3 hlgToNits(vec3 value)
{
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    vec3 low = value * value / 3.0;
    vec3 high = (exp((value - c) / a) + b) / 12.0;
    return 1000.0 * mix(low, high, greaterThan(value, vec3(0.5)));
}

vec3 sdrToLinear(vec3 value)
{
    vec3 low = value / 4.5;
    vec3 high = pow((value + 0.099) / 1.099, vec3(1.0 / 0.45));
    return mix(low, high, greaterThanEqual(value, vec3(0.081)));
}

vec3 linearToSrgb(vec3 value)
{
    vec3 low = 12.92 * value;
    vec3 high =
        1.055 * pow(max(value, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(low, high, greaterThan(value, vec3(0.0031308)));
}

vec3 sourceToBt709(vec3 value, uint primaries)
{
    if (primaries == 1U) {
        return mat3(
             1.6605, -0.1246, -0.0182,
            -0.5876,  1.1329, -0.1006,
            -0.0728, -0.0083,  1.1187) * value;
    }
    if (primaries == 2U) {
        return mat3(
             1.2249, -0.0420, -0.0197,
            -0.2247,  1.0420, -0.0786,
            -0.0002,  0.0000,  1.0983) * value;
    }
    return value;
}

vec3 sourceToBt2020(vec3 value, uint primaries)
{
    if (primaries == 0U) {
        return mat3(
            0.6274, 0.0691, 0.0164,
            0.3293, 0.9195, 0.0880,
            0.0433, 0.0114, 0.8956) * value;
    }
    if (primaries == 2U) {
        return mat3(
             0.7538, 0.0457, -0.0012,
             0.1986, 0.9418,  0.0176,
             0.0475, 0.0125,  0.9836) * value;
    }
    return value;
}

vec3 nitsToPq(vec3 value)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    vec3 power =
        pow(clamp(value / 10000.0, 0.0, 1.0), vec3(m1));
    return pow(
        (c1 + c2 * power) / (1.0 + c3 * power),
        vec3(m2));
}

vec3 nitsToHlg(vec3 value)
{
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    vec3 linear = max(value, vec3(0.0)) / 1000.0;
    vec3 low = sqrt(3.0 * linear);
    vec3 high =
        a * log(max(12.0 * linear - b, vec3(0.000001))) + c;
    return mix(low, high, greaterThan(linear, vec3(1.0 / 12.0)));
}

vec3 presentColor(vec3 rgb)
{
    uint transfer = surface.z;
    uint primaries = surface.w;
    uint outputMode = source.w;
    vec3 linear;
    float sourceWhite = luminance.x;
    if (transfer == 1U) {
        linear = pqToNits(max(rgb, vec3(0.0)));
    } else if (transfer == 2U) {
        linear = hlgToNits(max(rgb, vec3(0.0)));
    } else if (transfer == 3U) {
        linear = max(rgb, vec3(0.0)) * sourceWhite;
    } else {
        linear = sdrToLinear(clamp(rgb, 0.0, 1.0)) * sourceWhite;
    }
    if (outputMode == 1U || outputMode == 2U) {
        linear = sourceToBt2020(linear, primaries);
    } else {
        linear = sourceToBt709(linear, primaries);
    }
    if (outputMode == 1U) {
        return nitsToPq(linear);
    }
    if (outputMode == 2U) {
        return nitsToHlg(linear);
    }

    // SDR targets keep the deterministic HDR-to-SDR shoulder.
    float maximum = max(luminance.y, sourceWhite);
    if (maximum > sourceWhite) {
        linear = linear / (vec3(1.0) + linear / maximum);
    }
    vec3 normalized = clamp(linear / sourceWhite, 0.0, 1.0);
    // An sRGB framebuffer converts linear fragment output to sRGB during the
    // fixed-function write. Linear/UNORM attachments need explicit encoding.
    return outputMode == 3U ? normalized : linearToSrgb(normalized);
}

bool sourceCoordinate(out vec2 coordinate)
{
    // OpenGL has a bottom-left framebuffer origin. Convert to the common
    // top-left viewport contract before applying shared geometry semantics.
    vec2 pixel = vec2(
        gl_FragCoord.x,
        float(surface.y) - gl_FragCoord.y);
    vec2 origin = vec2(viewport.xy);
    vec2 size = vec2(viewport.zw);
    if (any(lessThan(pixel, origin))
        || any(greaterThanEqual(pixel, origin + size))) {
        return false;
    }

    vec2 destination = (pixel - origin) / size;
    bool swapsAxes =
        presentation.x == 1U || presentation.x == 3U;
    float sourceAspect = swapsAxes
        ? float(source.y) / float(source.x)
        : float(source.x) / float(source.y);
    float viewportAspect = size.x / size.y;

    if (presentation.y == 0U) {
        if (sourceAspect > viewportAspect) {
            float height = viewportAspect / sourceAspect;
            float top = (1.0 - height) * 0.5;
            if (destination.y < top || destination.y >= top + height) {
                return false;
            }
            destination.y = (destination.y - top) / height;
        } else {
            float width = sourceAspect / viewportAspect;
            float left = (1.0 - width) * 0.5;
            if (destination.x < left || destination.x >= left + width) {
                return false;
            }
            destination.x = (destination.x - left) / width;
        }
    } else if (presentation.y == 1U) {
        if (sourceAspect > viewportAspect) {
            float width = sourceAspect / viewportAspect;
            destination.x = (destination.x - 0.5) * width + 0.5;
        } else {
            float height = viewportAspect / sourceAspect;
            destination.y = (destination.y - 0.5) * height + 0.5;
        }
    }

    if (presentation.x == 1U) {
        coordinate = vec2(destination.y, 1.0 - destination.x);
    } else if (presentation.x == 2U) {
        coordinate = 1.0 - destination;
    } else if (presentation.x == 3U) {
        coordinate = vec2(1.0 - destination.y, destination.x);
    } else {
        coordinate = destination;
    }
    coordinate = clamp(coordinate, vec2(0.0), vec2(0.999999));
    return true;
}

void main()
{
    vec2 coordinate;
    if (!sourceCoordinate(coordinate)) {
        outputColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    uint x = min(
        uint(coordinate.x * float(source.x)),
        source.x - 1U);
    uint y = min(
        uint(coordinate.y * float(source.y)),
        source.y - 1U);
    ivec2 position = ivec2(int(x), int(y));

    vec3 rgb;
    uint format = source.z;
#if defined(QTAV_EXTERNAL_OES)
    if (format == 13U) {
        vec4 transformed =
            externalTransform * vec4(coordinate, 0.0, 1.0);
        rgb = texture(externalImage, transformed.xy).rgb;
    } else
#endif
    if (format <= 5U || format == 12U) {
        float luma;
        float chromaU;
        float chromaV;
        if (format == 12U) {
            luma = float(
                texelFetch(p010Luma, position, 0).r & 1023U)
                / 1023.0;
            ivec2 chromaPosition =
                ivec2(int(x / 2U), int(y / 2U));
            uvec2 chroma =
                texelFetch(p010Chroma, chromaPosition, 0).rg;
            chromaU = float(chroma.r & 1023U) / 1023.0;
            chromaV = float(chroma.g & 1023U) / 1023.0;
            rgb = p010ToRgb(
                luma * (65472.0 / 65535.0),
                chromaU * (65472.0 / 65535.0),
                chromaV * (65472.0 / 65535.0),
                presentation.z,
                presentation.w);
        } else if (format == 5U) {
            luma = float(texelFetch(p010Luma, position, 0).r) / 65535.0;
            ivec2 chromaPosition =
                ivec2(int(x / 2U), int(y / 2U));
            uvec2 chroma =
                texelFetch(p010Chroma, chromaPosition, 0).rg;
            chromaU = float(chroma.r) / 65535.0;
            chromaV = float(chroma.g) / 65535.0;
            rgb = p010ToRgb(
                luma,
                chromaU,
                chromaV,
                presentation.z,
                presentation.w);
        } else {
            luma = texelFetch(plane0, position, 0).r;
            ivec2 chromaPosition = position;
            if (format == 0U) {
                chromaPosition /= 2;
            } else if (format == 1U) {
                chromaPosition.x /= 2;
            } else if (format == 3U || format == 4U) {
                chromaPosition /= 2;
            }

            if (format <= 2U) {
                chromaU =
                    texelFetch(plane1, chromaPosition, 0).r;
                chromaV =
                    texelFetch(plane2, chromaPosition, 0).r;
            } else {
                vec2 chroma =
                    texelFetch(plane1, chromaPosition, 0).rg;
                chromaU = format == 3U ? chroma.r : chroma.g;
                chromaV = format == 3U ? chroma.g : chroma.r;
            }
            rgb = yuvToRgb(
                luma,
                chromaU,
                chromaV,
                presentation.z,
                presentation.w);
        }
    } else {
        vec4 packed = texelFetch(plane0, position, 0);
        if (format == 6U || format == 8U) {
            rgb = packed.rgb;
        } else if (format == 7U || format == 9U) {
            rgb = packed.bgr;
        } else if (format == 10U) {
            rgb = packed.gba;
        } else {
            rgb = vec3(packed.r);
        }
    }
    outputColor = vec4(presentColor(rgb), 1.0);
}
)qtav";

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
    YUV420P10,
    ExternalOES,
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

std::uint32_t shaderOutputColorSpace(
    OpenGLOutputColorSpace colorSpace,
    bool framebufferSrgb) noexcept
{
    switch (colorSpace) {
    case OpenGLOutputColorSpace::HDR10PQ:
        return 1;
    case OpenGLOutputColorSpace::HDR10HLG:
        return 2;
    default:
        return framebufferSrgb ? 3 : 0;
    }
}

bool framebufferUsesSrgbEncoding(std::uint32_t framebuffer) noexcept
{
    GLint encoding = GL_LINEAR;
    glGetFramebufferAttachmentParameteriv(
        GL_FRAMEBUFFER,
        framebuffer == 0 ? GL_BACK : GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING,
        &encoding);
    return encoding == GL_SRGB;
}

struct PlaneLayout {
    int width = 0;
    int height = 0;
    int bytesPerPixel = 0;
    GLenum internalFormat = GL_R8;
    GLenum format = GL_RED;
    GLenum type = GL_UNSIGNED_BYTE;
};

struct PackedFrame {
    ShaderPixelFormat format = ShaderPixelFormat::YUV420P;
    std::array<PlaneLayout, 3> layouts {};
    std::array<std::vector<std::uint8_t>, 3> bytes;
    int planeCount = 0;
};

bool validViewport(
    const VideoViewport& viewport,
    const VideoSize& surface) noexcept
{
    if (!viewport.isValid()) {
        return true;
    }
    return viewport.x >= 0 && viewport.y >= 0
        && viewport.x <= surface.width
        && viewport.y <= surface.height
        && viewport.width <= surface.width - viewport.x
        && viewport.height <= surface.height - viewport.y;
}

bool supportedConfig(const VideoRenderConfig& config) noexcept
{
    return config.surfaceSize.isValid()
        && validViewport(config.viewport, config.surfaceSize)
        && config.deviceOwnership == NativeResourceOwnership::Borrowed
        && config.contextOwnership == NativeResourceOwnership::Borrowed
        && config.surfaceOwnership == NativeResourceOwnership::Borrowed;
}

VideoViewport effectiveViewport(
    const VideoRenderConfig& config) noexcept
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

ShaderColorPrimaries shaderColorPrimaries(
    ColorPrimaries primaries) noexcept
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
        return static_cast<float>(
            content.maximumContentLightLevel);
    }
    return frame.colorSpaceInfo().isHdr() ? 1000.0F : 100.0F;
}

bool copyPlane(
    const VideoFrame& frame,
    int plane,
    const PlaneLayout& layout,
    std::vector<std::uint8_t>& destination) noexcept
{
    const int rowBytes = layout.width * layout.bytesPerPixel;
    const int lineSize = frame.lineSize(plane);
    const auto* source = frame.data(plane);
    if (!source || lineSize == 0 || rowBytes <= 0 || layout.height <= 0
        || std::abs(static_cast<long long>(lineSize)) < rowBytes) {
        return false;
    }
    const std::size_t rowSize = static_cast<std::size_t>(rowBytes);
    const std::size_t height =
        static_cast<std::size_t>(layout.height);
    if (height > std::numeric_limits<std::size_t>::max() / rowSize) {
        return false;
    }
    destination.resize(rowSize * height);
    for (int row = 0; row < layout.height; ++row) {
        std::memcpy(
            destination.data()
                + static_cast<std::size_t>(row) * rowSize,
            source
                + static_cast<std::ptrdiff_t>(row) * lineSize,
            rowSize);
    }
    return true;
}

bool interleavePlanar10Chroma(
    const VideoFrame& frame,
    const PlaneLayout& layout,
    std::vector<std::uint8_t>& destination) noexcept
{
    const int sourceRowBytes = layout.width * 2;
    const int destinationRowBytes = layout.width * 4;
    const int uLineSize = frame.lineSize(1);
    const int vLineSize = frame.lineSize(2);
    const auto* u = frame.data(1);
    const auto* v = frame.data(2);
    if (!u || !v || uLineSize == 0 || vLineSize == 0
        || sourceRowBytes <= 0 || destinationRowBytes <= 0
        || layout.height <= 0
        || std::abs(static_cast<long long>(uLineSize)) < sourceRowBytes
        || std::abs(static_cast<long long>(vLineSize)) < sourceRowBytes) {
        return false;
    }
    const std::size_t rowSize =
        static_cast<std::size_t>(destinationRowBytes);
    const std::size_t height = static_cast<std::size_t>(layout.height);
    if (height > std::numeric_limits<std::size_t>::max() / rowSize) {
        return false;
    }
    destination.resize(rowSize * height);
    for (int row = 0; row < layout.height; ++row) {
        const auto* uRow =
            u + static_cast<std::ptrdiff_t>(row) * uLineSize;
        const auto* vRow =
            v + static_cast<std::ptrdiff_t>(row) * vLineSize;
        auto* output = destination.data()
            + static_cast<std::size_t>(row) * rowSize;
        for (int column = 0; column < layout.width; ++column) {
            std::memcpy(output + column * 4, uRow + column * 2, 2);
            std::memcpy(output + column * 4 + 2, vRow + column * 2, 2);
        }
    }
    return true;
}

bool packFrame(
    const VideoFrame& frame,
    PackedFrame& result,
    std::string& error)
{
    if (!frame) {
        error = "The OpenGL ES renderer received an invalid frame";
        return false;
    }
    const int width = frame.width();
    const int height = frame.height();
    const int halfWidth = (width + 1) / 2;
    const int halfHeight = (height + 1) / 2;
    switch (frame.format()) {
    case PixelFormat::YUV420P:
        result.format = ShaderPixelFormat::YUV420P;
        result.planeCount = 3;
        result.layouts = {{
            { width, height, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE },
            { halfWidth, halfHeight, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE },
            { halfWidth, halfHeight, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE },
        }};
        break;
    case PixelFormat::YUV422P:
        result.format = ShaderPixelFormat::YUV422P;
        result.planeCount = 3;
        result.layouts = {{
            { width, height, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE },
            { halfWidth, height, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE },
            { halfWidth, height, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE },
        }};
        break;
    case PixelFormat::YUV444P:
        result.format = ShaderPixelFormat::YUV444P;
        result.planeCount = 3;
        result.layouts = {{
            { width, height, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE },
            { width, height, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE },
            { width, height, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE },
        }};
        break;
    case PixelFormat::NV12:
    case PixelFormat::NV21:
        result.format = frame.format() == PixelFormat::NV12
            ? ShaderPixelFormat::NV12
            : ShaderPixelFormat::NV21;
        result.planeCount = 2;
        result.layouts = {{
            { width, height, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE },
            { halfWidth, halfHeight, 2, GL_RG8, GL_RG, GL_UNSIGNED_BYTE },
            {},
        }};
        break;
    case PixelFormat::P010:
        if (frame.formatName().find("p010le") == std::string::npos) {
            error =
                "The OpenGL ES renderer currently supports little-endian P010";
            return false;
        }
        result.format = ShaderPixelFormat::P010;
        result.planeCount = 2;
        result.layouts = {{
            { width, height, 2, GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT },
            { halfWidth, halfHeight, 4, GL_RG16UI, GL_RG_INTEGER, GL_UNSIGNED_SHORT },
            {},
        }};
        break;
    case PixelFormat::YUV420P10:
        if (frame.formatName().find("yuv420p10le") == std::string::npos) {
            error = "The OpenGL ES renderer currently supports little-endian "
                    "10-bit planar YUV";
            return false;
        }
        result.format = ShaderPixelFormat::YUV420P10;
        result.planeCount = 2;
        result.layouts = {{
            { width, height, 2, GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT },
            { halfWidth, halfHeight, 4, GL_RG16UI, GL_RG_INTEGER, GL_UNSIGNED_SHORT },
            {},
        }};
        if (!copyPlane(frame, 0, result.layouts[0], result.bytes[0])
            || !interleavePlanar10Chroma(
                frame,
                result.layouts[1],
                result.bytes[1])) {
            error = "The OpenGL ES renderer could not pack a 10-bit planar "
                    "YUV frame";
            return false;
        }
        return true;
    case PixelFormat::RGB24:
    case PixelFormat::BGR24:
        result.format = frame.format() == PixelFormat::RGB24
            ? ShaderPixelFormat::RGB24
            : ShaderPixelFormat::BGR24;
        result.planeCount = 1;
        result.layouts[0] = {
            width, height, 3, GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE
        };
        break;
    case PixelFormat::RGBA:
    case PixelFormat::BGRA:
    case PixelFormat::ARGB:
        result.format = frame.format() == PixelFormat::RGBA
            ? ShaderPixelFormat::RGBA
            : frame.format() == PixelFormat::BGRA
                ? ShaderPixelFormat::BGRA
                : ShaderPixelFormat::ARGB;
        result.planeCount = 1;
        result.layouts[0] = {
            width, height, 4, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE
        };
        break;
    case PixelFormat::Gray8:
        result.format = ShaderPixelFormat::Gray8;
        result.planeCount = 1;
        result.layouts[0] = {
            width, height, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE
        };
        break;
    default:
        error =
            "The OpenGL ES renderer does not support this software pixel format";
        return false;
    }

    for (int plane = 0; plane < result.planeCount; ++plane) {
        if (!copyPlane(
                frame,
                plane,
                result.layouts[plane],
                result.bytes[plane])) {
            error =
                "The OpenGL ES renderer could not copy a software frame plane";
            return false;
        }
    }
    return true;
}

std::string shaderLog(GLuint object, bool program)
{
    GLint length = 0;
    if (program) {
        glGetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
    } else {
        glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
    }
    if (length <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    GLsizei written = 0;
    if (program) {
        glGetProgramInfoLog(
            object, length, &written, result.data());
    } else {
        glGetShaderInfoLog(
            object, length, &written, result.data());
    }
    result.resize(
        written > 0 ? static_cast<std::size_t>(written) : 0U);
    return result;
}

GLuint compileShader(
    GLenum type,
    const char* source,
    std::string& error)
{
    const GLuint shader = glCreateShader(type);
    if (!shader) {
        error = "glCreateShader failed";
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        error = "OpenGL ES shader compilation failed: "
            + shaderLog(shader, false);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint createProgram(bool externalOES, std::string& error)
{
    const GLuint vertex =
        compileShader(GL_VERTEX_SHADER, VertexShader, error);
    if (!vertex) {
        return 0;
    }
    std::string fragmentSource = FragmentShader;
    if (externalOES) {
        const std::size_t versionStart =
            fragmentSource.find("#version 300 es");
        const std::size_t versionEnd =
            versionStart == std::string::npos
            ? std::string::npos
            : fragmentSource.find('\n', versionStart);
        if (versionEnd == std::string::npos) {
            error = "The OpenGL ES fragment shader has no version line";
            glDeleteShader(vertex);
            return 0;
        }
        fragmentSource.insert(
            versionEnd + 1,
            "#define QTAV_EXTERNAL_OES 1\n");
    }
    const GLuint fragment =
        compileShader(
            GL_FRAGMENT_SHADER,
            fragmentSource.c_str(),
            error);
    if (!fragment) {
        glDeleteShader(vertex);
        return 0;
    }
    const GLuint program = glCreateProgram();
    if (!program) {
        error = "glCreateProgram failed";
    } else {
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            error = "OpenGL ES program link failed: "
                + shaderLog(program, true);
            glDeleteProgram(program);
        }
    }
    glDeleteShader(fragment);
    glDeleteShader(vertex);
    return error.empty() ? program : 0;
}

const char* glErrorName(GLenum error) noexcept
{
    switch (error) {
    case GL_NO_ERROR:
        return "GL_NO_ERROR";
    case GL_INVALID_ENUM:
        return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE:
        return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION:
        return "GL_INVALID_OPERATION";
    case GL_INVALID_FRAMEBUFFER_OPERATION:
        return "GL_INVALID_FRAMEBUFFER_OPERATION";
    case GL_OUT_OF_MEMORY:
        return "GL_OUT_OF_MEMORY";
    default:
        return "unknown OpenGL ES error";
    }
}

bool checkError(const char* operation, std::string& error)
{
    const GLenum code = glGetError();
    if (code == GL_NO_ERROR) {
        return true;
    }
    error = std::string(operation) + " failed: " + glErrorName(code)
        + " (" + std::to_string(code) + ')';
    while (glGetError() != GL_NO_ERROR) {
    }
    return false;
}

struct ProgramResources {
    GLuint program = 0;
    GLint source = -1;
    GLint surface = -1;
    GLint viewport = -1;
    GLint presentation = -1;
    GLint luminance = -1;
    GLint externalTransform = -1;
};

struct HardwareFrameKey {
    std::uintptr_t buffer = 0;
    std::uint32_t generation = 0;
    std::int64_t timestampMilliseconds = 0;

    bool operator==(const HardwareFrameKey& other) const noexcept
    {
        return buffer == other.buffer
            && generation == other.generation
            && timestampMilliseconds == other.timestampMilliseconds;
    }
};

HardwareFrameKey hardwareFrameKey(
    const VideoFrame& frame) noexcept
{
    HardwareFrameKey result;
    if (!frame || !frame.hasHardwareFrame()) {
        return result;
    }
    const NativeHandle output = frame.hardwareFrame().nativeHandle(
        HardwareHandleType::Frame);
    result.buffer = output.value;
    result.generation = output.subresource;
    result.timestampMilliseconds = frame.timestamp();
    return result;
}

class RendererEventState final {
public:
    void set(VideoRenderAPI::EventCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(callback);
    }

    void notify(VideoRenderEventType type, std::string detail)
    {
        VideoRenderAPI::EventCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = callback_;
        }
        if (callback) {
            callback({ type, std::move(detail) });
        }
    }

private:
    std::mutex mutex_;
    VideoRenderAPI::EventCallback callback_;
};

} // namespace

bool OpenGLRenderTarget::isHdr() const noexcept
{
    return isValid() && openGLColorSpaceIsHdr(colorSpace);
}

bool openGLColorSpaceIsHdr(
    OpenGLOutputColorSpace colorSpace) noexcept
{
    return colorSpace == OpenGLOutputColorSpace::HDR10PQ
        || colorSpace == OpenGLOutputColorSpace::HDR10HLG;
}

class OpenGLVideoRenderer::Impl {
public:
    Impl(
        OpenGLCurrentTargetCallback currentTarget,
        std::shared_ptr<OpenGLHardwareFrameInterop>
            hardwareInterop)
        : currentTarget_(std::move(currentTarget))
        , hardwareInterop_(std::move(hardwareInterop))
        , eventState_(std::make_shared<RendererEventState>())
    {
        connectHardwareInterop();
    }

    ~Impl()
    {
        if (eventState_) {
            eventState_->set({});
        }
        if (hardwareInterop_) {
            hardwareInterop_->setFrameAvailableCallback({});
        }
        close();
    }

    void connectHardwareInterop()
    {
        if (!hardwareInterop_ || !eventState_) {
            return;
        }
        std::weak_ptr<RendererEventState> weak = eventState_;
        hardwareInterop_->setFrameAvailableCallback([weak] {
            if (const auto state = weak.lock()) {
                state->notify(
                    VideoRenderEventType::RedrawRequested,
                    {});
            }
        });
    }

    void notify(VideoRenderEventType type, std::string detail)
    {
        if (eventState_) {
            eventState_->notify(type, std::move(detail));
        }
    }

    bool createProgramResources(
        bool externalOES,
        ProgramResources& result,
        std::string& error)
    {
        result.program = createProgram(externalOES, error);
        if (!result.program) {
            return false;
        }
        glUseProgram(result.program);
        const std::array<const char*, 5> samplerNames {
            "plane0", "plane1", "plane2", "p010Luma", "p010Chroma"
        };
        for (std::size_t index = 0;
             index < samplerNames.size();
             ++index) {
            const GLint location =
                glGetUniformLocation(result.program, samplerNames[index]);
            if (location < 0) {
                error = std::string("Missing OpenGL ES uniform ")
                    + samplerNames[index];
                return false;
            }
            glUniform1i(location, static_cast<GLint>(index));
        }
        if (externalOES) {
            const GLint externalImage =
                glGetUniformLocation(result.program, "externalImage");
            result.externalTransform =
                glGetUniformLocation(
                    result.program,
                    "externalTransform");
            if (externalImage < 0
                || result.externalTransform < 0) {
                error =
                    "The external-OES OpenGL ES shader parameters are unavailable";
                return false;
            }
            glUniform1i(externalImage, 5);
        }
        result.source =
            glGetUniformLocation(result.program, "source");
        result.surface =
            glGetUniformLocation(result.program, "surface");
        result.viewport =
            glGetUniformLocation(result.program, "viewport");
        result.presentation =
            glGetUniformLocation(result.program, "presentation");
        result.luminance =
            glGetUniformLocation(result.program, "luminance");
        if (result.source < 0 || result.surface < 0
            || result.viewport < 0 || result.presentation < 0
            || result.luminance < 0) {
            error =
                "The OpenGL ES renderer could not resolve shader parameters";
            return false;
        }
        return true;
    }

    bool ensureExternalProgram(std::string& error)
    {
        if (externalProgram_.program) {
            return true;
        }
        if (!createProgramResources(
                true,
                externalProgram_,
                error)) {
            if (externalProgram_.program) {
                glDeleteProgram(externalProgram_.program);
            }
            externalProgram_ = {};
            return false;
        }
        return checkError(
            "OpenGL ES external-OES resource creation",
            error);
    }

    bool createResources(std::string& error)
    {
        const GLubyte* version = glGetString(GL_VERSION);
        if (!version) {
            error =
                "The OpenGL ES renderer requires a current context";
            return false;
        }
        GLint major = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        if (major < 3) {
            error =
                "The OpenGL ES renderer requires OpenGL ES 3.x";
            return false;
        }
        if (!createProgramResources(
                false,
                softwareProgram_,
                error)) {
            return false;
        }
        glGenVertexArrays(1, &vertexArray_);
        glGenTextures(
            static_cast<GLsizei>(textures_.size()),
            textures_.data());
        if (!vertexArray_
            || std::any_of(
                textures_.begin(),
                textures_.end(),
                [](GLuint texture) { return texture == 0; })) {
            error =
                "The OpenGL ES renderer could not create draw resources";
            return false;
        }

        if (hardwareInterop_
            && !ensureExternalProgram(error)) {
            return false;
        }

        const std::uint8_t normalizedZero[4] {};
        const std::uint16_t integerZero[2] {};
        for (int index = 0; index < 3; ++index) {
            uploadTexture(
                index,
                { 1, 1, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE },
                normalizedZero);
        }
        uploadTexture(
            3,
            { 1, 1, 2, GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT },
            integerZero);
        uploadTexture(
            4,
            { 1, 1, 4, GL_RG16UI, GL_RG_INTEGER, GL_UNSIGNED_SHORT },
            integerZero);
        return checkError("OpenGL ES resource creation", error);
    }

    OpenGLHardwareImportStatus prepareHardwareFrame(
        const VideoFrame& frame,
        std::string& detail)
    {
        std::shared_ptr<OpenGLHardwareFrameInterop> interop;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            if (!open_) {
                detail = "The OpenGL ES renderer is not open";
                preparedHardware_ = {};
                preparedKey_ = {};
                return OpenGLHardwareImportStatus::Error;
            }
            interop = hardwareInterop_;
        }
        if (!frame || !frame.hasHardwareFrame()) {
            detail =
                "The frame is not an OpenGL ES-interoperable hardware frame";
            preparedHardware_ = {};
            preparedKey_ = {};
            return OpenGLHardwareImportStatus::Unsupported;
        }
        if (!interop
            || !interop->supports(frame.hardwareFrame())) {
            detail =
                "The OpenGL ES renderer has no compatible interop for this hardware frame";
            preparedHardware_ = {};
            preparedKey_ = {};
            return OpenGLHardwareImportStatus::Unsupported;
        }
        if (!ensureExternalProgram(detail)) {
            preparedHardware_ = {};
            preparedKey_ = {};
            return OpenGLHardwareImportStatus::Error;
        }
        OpenGLHardwareImportResult imported =
            interop->prepareFrame(frame);
        if (imported.status
                == OpenGLHardwareImportStatus::Ready
            && !imported) {
            imported.status =
                OpenGLHardwareImportStatus::Error;
            if (imported.detail.empty()) {
                imported.detail =
                    "The OpenGL ES interop returned an invalid external texture";
            }
        }
        detail = imported.detail;
        if (imported.status
            == OpenGLHardwareImportStatus::Ready) {
            preparedKey_ = hardwareFrameKey(frame);
            preparedHardware_ = std::move(imported.texture);
        } else {
            preparedKey_ = {};
            preparedHardware_ = {};
        }
        return imported.status;
    }

    void uploadTexture(
        int unit,
        const PlaneLayout& layout,
        const void* data)
    {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, textures_[unit]);
        glTexParameteri(
            GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(
            GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(
            GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(
            GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            static_cast<GLint>(layout.internalFormat),
            layout.width,
            layout.height,
            0,
            layout.format,
            layout.type,
            data);
    }

    void destroyResources() noexcept
    {
        if (hardwareInterop_) {
            hardwareInterop_->releaseCurrentContextResources();
        }
        preparedHardware_ = {};
        preparedKey_ = {};
        if (std::any_of(
                textures_.begin(),
                textures_.end(),
                [](GLuint texture) { return texture != 0; })) {
            glDeleteTextures(
                static_cast<GLsizei>(textures_.size()),
                textures_.data());
        }
        textures_.fill(0);
        if (vertexArray_) {
            glDeleteVertexArrays(1, &vertexArray_);
            vertexArray_ = 0;
        }
        if (externalProgram_.program) {
            glDeleteProgram(externalProgram_.program);
        }
        externalProgram_ = {};
        if (softwareProgram_.program) {
            glDeleteProgram(softwareProgram_.program);
        }
        softwareProgram_ = {};
    }

    void close() noexcept
    {
        std::lock_guard<std::mutex> renderLock(renderMutex_);
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        if (open_ || softwareProgram_.program || vertexArray_) {
            destroyResources();
        }
        open_ = false;
        config_ = {};
    }

    std::mutex stateMutex_;
    std::mutex renderMutex_;
    OpenGLCurrentTargetCallback currentTarget_;
    std::shared_ptr<OpenGLHardwareFrameInterop> hardwareInterop_;
    std::shared_ptr<RendererEventState> eventState_;
    VideoRenderConfig config_;
    bool open_ = false;
    GLuint vertexArray_ = 0;
    std::array<GLuint, 5> textures_ {};
    ProgramResources softwareProgram_;
    ProgramResources externalProgram_;
    HardwareFrameKey preparedKey_;
    OpenGLExternalTextureFrame preparedHardware_;
};

OpenGLHardwareFrameInterop::~OpenGLHardwareFrameInterop() = default;

OpenGLVideoRenderer::OpenGLVideoRenderer(
    OpenGLCurrentTargetCallback currentTarget,
    std::shared_ptr<OpenGLHardwareFrameInterop> hardwareInterop)
    : impl_(std::make_unique<Impl>(
          std::move(currentTarget),
          std::move(hardwareInterop)))
{
}

OpenGLVideoRenderer::~OpenGLVideoRenderer() = default;
OpenGLVideoRenderer::OpenGLVideoRenderer(
    OpenGLVideoRenderer&&) noexcept = default;
OpenGLVideoRenderer& OpenGLVideoRenderer::operator=(
    OpenGLVideoRenderer&&) noexcept = default;

VideoRenderCapabilities OpenGLVideoRenderer::capabilities() const
{
    VideoRenderCapabilities result;
    result.softwareFormats = {
        PixelFormat::YUV420P,
        PixelFormat::YUV422P,
        PixelFormat::YUV444P,
        PixelFormat::YUV420P10,
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
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        if (impl_->hardwareInterop_) {
            result.hardwareDevices =
                impl_->hardwareInterop_
                    ->capabilities()
                    .sourceDevices;
        }
    }
    return result;
}

void OpenGLVideoRenderer::setEventCallback(EventCallback callback)
{
    if (!impl_) {
        return;
    }
    impl_->eventState_->set(std::move(callback));
}

bool OpenGLVideoRenderer::open(const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }
    std::string error;
    bool opened = false;
    {
        std::lock_guard<std::mutex> renderLock(impl_->renderMutex_);
        std::lock_guard<std::mutex> stateLock(impl_->stateMutex_);
        if (!impl_->currentTarget_) {
            error =
                "The OpenGL ES renderer requires a current-target callback";
        } else if (!supportedConfig(config)) {
            error =
                "The OpenGL ES renderer requires a valid borrowed surface configuration";
        } else if (impl_->open_) {
            impl_->config_ = config;
            opened = true;
        } else {
            if (impl_->createResources(error)) {
                impl_->config_ = config;
                impl_->open_ = true;
                opened = true;
            } else {
                impl_->destroyResources();
            }
        }
    }
    if (!opened) {
        impl_->notify(VideoRenderEventType::Error, std::move(error));
    }
    return opened;
}

bool OpenGLVideoRenderer::configure(
    const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }
    bool configured = false;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        configured = impl_->open_ && supportedConfig(config);
        if (configured) {
            impl_->config_ = config;
        }
    }
    if (configured) {
        impl_->notify(VideoRenderEventType::RedrawRequested, {});
    } else {
        impl_->notify(
            VideoRenderEventType::Error,
            "The OpenGL ES renderer is closed or the configuration is invalid");
    }
    return configured;
}

bool OpenGLVideoRenderer::render(const VideoFrame& frame)
{
    if (!impl_) {
        return false;
    }
    std::lock_guard<std::mutex> renderLock(impl_->renderMutex_);

    VideoRenderConfig config;
    OpenGLCurrentTargetCallback currentTarget;
    {
        std::lock_guard<std::mutex> stateLock(impl_->stateMutex_);
        if (impl_->open_) {
            config = impl_->config_;
            currentTarget = impl_->currentTarget_;
        }
    }
    if (!config.surfaceSize.isValid()) {
        impl_->notify(
            VideoRenderEventType::Error,
            "The OpenGL ES renderer is not open");
        return false;
    }
    const OpenGLRenderTarget target =
        currentTarget ? currentTarget() : OpenGLRenderTarget {};
    if (!target.isValid()) {
        impl_->notify(
            VideoRenderEventType::SurfaceLost,
            "The current OpenGL ES target is unavailable");
        return false;
    }
    if (target.size.width != config.surfaceSize.width
        || target.size.height != config.surfaceSize.height) {
        impl_->notify(
            VideoRenderEventType::Error,
            "The current OpenGL ES target size does not match the configured surface");
        return false;
    }
    PackedFrame packed;
    OpenGLExternalTextureFrame externalTexture;
    ProgramResources* program = &impl_->softwareProgram_;
    ShaderPixelFormat shaderFormat = ShaderPixelFormat::YUV420P;
    std::string error;
    if (frame.hasHardwareFrame()) {
        const HardwareFrameKey key = hardwareFrameKey(frame);
        if (!(impl_->preparedKey_ == key)
            || !impl_->preparedHardware_) {
            const OpenGLHardwareImportStatus status =
                impl_->prepareHardwareFrame(frame, error);
            if (status == OpenGLHardwareImportStatus::Pending) {
                return false;
            }
            if (status == OpenGLHardwareImportStatus::Stale) {
                return false;
            }
            if (status != OpenGLHardwareImportStatus::Ready) {
                impl_->notify(
                    VideoRenderEventType::Error,
                    error.empty()
                        ? "The OpenGL ES hardware-frame preparation failed"
                        : std::move(error));
                return false;
            }
        }
        externalTexture = impl_->preparedHardware_;
        program = &impl_->externalProgram_;
        shaderFormat = ShaderPixelFormat::ExternalOES;
    } else {
        if (!packFrame(frame, packed, error)) {
            impl_->notify(
                VideoRenderEventType::Error,
                std::move(error));
            return false;
        }
        shaderFormat = packed.format;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    if (target.framebuffer != 0
        && glCheckFramebufferStatus(GL_FRAMEBUFFER)
            != GL_FRAMEBUFFER_COMPLETE) {
        impl_->notify(
            VideoRenderEventType::SurfaceLost,
            "The current OpenGL ES framebuffer is incomplete");
        return false;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (externalTexture) {
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(
            GL_TEXTURE_EXTERNAL_OES,
            externalTexture.texture);
    } else if (packed.format == ShaderPixelFormat::P010
               || packed.format == ShaderPixelFormat::YUV420P10) {
        impl_->uploadTexture(
            3, packed.layouts[0], packed.bytes[0].data());
        impl_->uploadTexture(
            4, packed.layouts[1], packed.bytes[1].data());
    } else {
        for (int plane = 0; plane < packed.planeCount; ++plane) {
            impl_->uploadTexture(
                plane,
                packed.layouts[plane],
                packed.bytes[plane].data());
        }
    }

    const VideoColorSpace color = frame.colorSpaceInfo();
    const VideoViewport viewport = effectiveViewport(config);
    glUseProgram(program->program);
    const bool framebufferSrgb =
        target.colorSpace == OpenGLOutputColorSpace::SdrSrgb
        && framebufferUsesSrgbEncoding(target.framebuffer);
    glUniform4ui(
        program->source,
        static_cast<GLuint>(frame.width()),
        static_cast<GLuint>(frame.height()),
        static_cast<GLuint>(shaderFormat),
        shaderOutputColorSpace(
            target.colorSpace,
            framebufferSrgb));
    glUniform4ui(
        program->surface,
        static_cast<GLuint>(config.surfaceSize.width),
        static_cast<GLuint>(config.surfaceSize.height),
        static_cast<GLuint>(shaderColorTransfer(color.transfer)),
        static_cast<GLuint>(shaderColorPrimaries(color.primaries)));
    glUniform4i(
        program->viewport,
        viewport.x,
        viewport.y,
        viewport.width,
        viewport.height);
    glUniform4ui(
        program->presentation,
        static_cast<GLuint>(config.rotation),
        static_cast<GLuint>(config.aspectRatio),
        static_cast<GLuint>(shaderColorMatrix(color.matrix)),
        color.range == ColorRange::Full ? 1U : 0U);
    glUniform2f(
        program->luminance,
        100.0F,
        maximumLuminance(frame));
    if (externalTexture) {
        glUniformMatrix4fv(
            program->externalTransform,
            1,
            GL_FALSE,
            externalTexture.transform.data());
    }
    glViewport(
        0,
        0,
        config.surfaceSize.width,
        config.surfaceSize.height);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindVertexArray(impl_->vertexArray_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFlush();
    if (externalTexture) {
        impl_->preparedHardware_ = {};
        impl_->preparedKey_ = {};
    }
    if (!checkError("OpenGL ES frame rendering", error)) {
        impl_->notify(VideoRenderEventType::Error, std::move(error));
        return false;
    }
    return true;
}

void OpenGLVideoRenderer::close() noexcept
{
    if (impl_) {
        impl_->close();
    }
}

void OpenGLVideoRenderer::setCurrentTargetCallback(
    OpenGLCurrentTargetCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex_);
    impl_->currentTarget_ = std::move(callback);
}

OpenGLHardwareImportStatus
OpenGLVideoRenderer::prepareHardwareFrame(
    const VideoFrame& frame,
    std::string* detail)
{
    if (!impl_) {
        if (detail) {
            *detail = "The OpenGL ES renderer object is empty";
        }
        return OpenGLHardwareImportStatus::Error;
    }
    std::lock_guard<std::mutex> renderLock(impl_->renderMutex_);
    std::string localDetail;
    const OpenGLHardwareImportStatus status =
        impl_->prepareHardwareFrame(frame, localDetail);
    if (detail) {
        *detail = std::move(localDetail);
    }
    return status;
}

void OpenGLVideoRenderer::setHardwareFrameInterop(
    std::shared_ptr<OpenGLHardwareFrameInterop> hardwareInterop)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> renderLock(impl_->renderMutex_);
    std::shared_ptr<OpenGLHardwareFrameInterop> previous;
    {
        std::lock_guard<std::mutex> stateLock(impl_->stateMutex_);
        previous = std::move(impl_->hardwareInterop_);
        impl_->hardwareInterop_ = std::move(hardwareInterop);
    }
    if (previous) {
        previous->setFrameAvailableCallback({});
        previous->releaseCurrentContextResources();
    }
    if (impl_->externalProgram_.program) {
        glDeleteProgram(impl_->externalProgram_.program);
        impl_->externalProgram_ = {};
    }
    impl_->preparedHardware_ = {};
    impl_->preparedKey_ = {};
    impl_->connectHardwareInterop();
}

std::shared_ptr<OpenGLHardwareFrameInterop>
OpenGLVideoRenderer::hardwareFrameInterop() const noexcept
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex_);
    return impl_->hardwareInterop_;
}

} // namespace qtav
