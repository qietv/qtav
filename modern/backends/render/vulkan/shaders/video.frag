#version 450

layout(location = 0) out vec4 outputColor;

layout(std430, set = 0, binding = 0) readonly buffer FrameBytes {
    uint words[];
} frameBytes;

layout(std140, set = 0, binding = 1) uniform Parameters {
    // width, height, format, output color space
    uvec4 source;
    // byte strides for planes 0..2
    uvec4 strides;
    // byte offsets for planes 0..2
    uvec4 offsets;
    // surface width, height, transfer, primaries
    uvec4 surface;
    // x, y, width, height
    uvec4 viewport;
    // rotation, aspect ratio, matrix, range
    uvec4 presentation;
    // reference white, maximum source luminance, unused, unused
    vec4 luminance;
    // normalized source crop: left, top, right, bottom
    vec4 normalizedSourceRect;
} parameters;

uint byteAt(uint index)
{
    const uint word = frameBytes.words[index >> 2U];
    return (word >> ((index & 3U) * 8U)) & 255U;
}

float byteValue(uint offset, uint stride, uint x, uint y)
{
    return float(byteAt(offset + y * stride + x)) / 255.0;
}

float ushortValue(uint offset, uint stride, uint x, uint y)
{
    const uint index = offset + y * stride + x * 2U;
    const uint value = byteAt(index) | (byteAt(index + 1U) << 8U);
    return float(value) / 65535.0;
}

float ushort10Value(uint offset, uint stride, uint x, uint y)
{
    const uint index = offset + y * stride + x * 2U;
    const uint value = byteAt(index) | (byteAt(index + 1U) << 8U);
    return float(value & 1023U) / 1023.0;
}

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
    const float kg = 1.0 - kr - kb;
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
    const vec3 power = pow(max(value, vec3(0.0)), vec3(1.0 / m2));
    const vec3 numerator = max(power - c1, vec3(0.0));
    const vec3 denominator = max(c2 - c3 * power, vec3(0.000001));
    return 10000.0 * pow(numerator / denominator, vec3(1.0 / m1));
}

vec3 hlgToNits(vec3 value)
{
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    const vec3 low = value * value / 3.0;
    const vec3 high = (exp((value - c) / a) + b) / 12.0;
    return 1000.0 * mix(low, high, greaterThan(value, vec3(0.5)));
}

vec3 sdrToLinear(vec3 value)
{
    const vec3 low = value / 4.5;
    const vec3 high = pow((value + 0.099) / 1.099, vec3(1.0 / 0.45));
    return mix(low, high, greaterThanEqual(value, vec3(0.081)));
}

vec3 linearToSrgb(vec3 value)
{
    const vec3 low = 12.92 * value;
    const vec3 high =
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
    const vec3 power =
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
    const vec3 linear = max(value, vec3(0.0)) / 1000.0;
    const vec3 low = sqrt(3.0 * linear);
    const vec3 high = a * log(max(12.0 * linear - b, vec3(0.000001))) + c;
    return mix(low, high, greaterThan(linear, vec3(1.0 / 12.0)));
}

vec3 presentColor(vec3 rgb)
{
    const uint transfer = parameters.surface.z;
    const uint primaries = parameters.surface.w;
    const uint outputMode = parameters.source.w;
    vec3 linear;
    float sourceWhite = parameters.luminance.x;
    if (transfer == 1U) {
        linear = pqToNits(max(rgb, vec3(0.0)));
    } else if (transfer == 2U) {
        linear = hlgToNits(max(rgb, vec3(0.0)));
    } else if (transfer == 3U) {
        linear = max(rgb, vec3(0.0)) * sourceWhite;
    } else {
        linear = sdrToLinear(clamp(rgb, 0.0, 1.0)) * sourceWhite;
    }

    if (outputMode == 4U || outputMode == 5U || outputMode == 3U) {
        linear = sourceToBt2020(linear, primaries);
    } else {
        linear = sourceToBt709(linear, primaries);
    }
    if (outputMode == 4U) {
        return nitsToPq(linear);
    }
    if (outputMode == 5U) {
        return nitsToHlg(linear);
    }
    if (outputMode == 2U || outputMode == 3U) {
        return max(linear / sourceWhite, vec3(0.0));
    }

    // SDR targets keep the deterministic HDR-to-SDR shoulder.
    const float maximum = max(parameters.luminance.y, sourceWhite);
    if (maximum > sourceWhite) {
        linear = linear / (vec3(1.0) + linear / maximum);
    }
    linear = clamp(linear / sourceWhite, 0.0, 1.0);
    return outputMode == 1U ? linear : linearToSrgb(linear);
}

bool sourceCoordinate(out vec2 sourceCoordinate)
{
    const vec2 pixel = gl_FragCoord.xy;
    const vec2 origin = vec2(parameters.viewport.xy);
    const vec2 size = vec2(parameters.viewport.zw);
    if (any(lessThan(pixel, origin))
        || any(greaterThanEqual(pixel, origin + size))) {
        return false;
    }

    vec2 destination = (pixel - origin) / size;
    const bool swapsAxes =
        parameters.presentation.x == 1U || parameters.presentation.x == 3U;
    const float sourceAspect = swapsAxes
        ? float(parameters.source.y) / float(parameters.source.x)
        : float(parameters.source.x) / float(parameters.source.y);
    const float viewportAspect = size.x / size.y;

    if (parameters.presentation.y == 0U) {
        if (sourceAspect > viewportAspect) {
            const float height = viewportAspect / sourceAspect;
            const float top = (1.0 - height) * 0.5;
            if (destination.y < top || destination.y >= top + height) {
                return false;
            }
            destination.y = (destination.y - top) / height;
        } else {
            const float width = sourceAspect / viewportAspect;
            const float left = (1.0 - width) * 0.5;
            if (destination.x < left || destination.x >= left + width) {
                return false;
            }
            destination.x = (destination.x - left) / width;
        }
    } else if (parameters.presentation.y == 1U) {
        if (sourceAspect > viewportAspect) {
            const float width = sourceAspect / viewportAspect;
            destination.x = (destination.x - 0.5) * width + 0.5;
        } else {
            const float height = viewportAspect / sourceAspect;
            destination.y = (destination.y - 0.5) * height + 0.5;
        }
    }

    if (parameters.presentation.x == 1U) {
        sourceCoordinate = vec2(destination.y, 1.0 - destination.x);
    } else if (parameters.presentation.x == 2U) {
        sourceCoordinate = 1.0 - destination;
    } else if (parameters.presentation.x == 3U) {
        sourceCoordinate = vec2(1.0 - destination.y, destination.x);
    } else {
        sourceCoordinate = destination;
    }
    sourceCoordinate = clamp(sourceCoordinate, vec2(0.0), vec2(0.999999));
    return true;
}

void main()
{
    vec2 coordinate;
    if (!sourceCoordinate(coordinate)) {
        outputColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    const uint x = min(
        uint(coordinate.x * float(parameters.source.x)),
        parameters.source.x - 1U);
    const uint y = min(
        uint(coordinate.y * float(parameters.source.y)),
        parameters.source.y - 1U);

    vec3 rgb;
    const uint format = parameters.source.z;
    if (format <= 5U || format == 12U) {
        float luma;
        float chromaU;
        float chromaV;
        if (format == 12U) {
            luma = ushort10Value(
                parameters.offsets.x, parameters.strides.x, x, y);
            const uint chromaX = x / 2U;
            const uint chromaY = y / 2U;
            chromaU = ushort10Value(
                parameters.offsets.y,
                parameters.strides.y,
                chromaX,
                chromaY);
            chromaV = ushort10Value(
                parameters.offsets.z,
                parameters.strides.z,
                chromaX,
                chromaY);
            rgb = p010ToRgb(
                luma * (65472.0 / 65535.0),
                chromaU * (65472.0 / 65535.0),
                chromaV * (65472.0 / 65535.0),
                parameters.presentation.z,
                parameters.presentation.w);
        } else if (format == 5U) {
            luma = ushortValue(
                parameters.offsets.x, parameters.strides.x, x, y);
            const uint chromaX = x / 2U;
            const uint chromaY = y / 2U;
            chromaU = ushortValue(
                parameters.offsets.y,
                parameters.strides.y,
                chromaX * 2U,
                chromaY);
            chromaV = ushortValue(
                parameters.offsets.y,
                parameters.strides.y,
                chromaX * 2U + 1U,
                chromaY);
            rgb = p010ToRgb(
                luma,
                chromaU,
                chromaV,
                parameters.presentation.z,
                parameters.presentation.w);
        } else {
            luma = byteValue(
                parameters.offsets.x, parameters.strides.x, x, y);
            uint chromaX = x;
            uint chromaY = y;
            if (format == 0U) {
                chromaX /= 2U;
                chromaY /= 2U;
            } else if (format == 1U) {
                chromaX /= 2U;
            } else if (format == 3U || format == 4U) {
                chromaX /= 2U;
                chromaY /= 2U;
            }

            if (format <= 2U) {
                chromaU = byteValue(
                    parameters.offsets.y,
                    parameters.strides.y,
                    chromaX,
                    chromaY);
                chromaV = byteValue(
                    parameters.offsets.z,
                    parameters.strides.z,
                    chromaX,
                    chromaY);
            } else {
                const uint first = chromaX * 2U;
                const float a = byteValue(
                    parameters.offsets.y,
                    parameters.strides.y,
                    first,
                    chromaY);
                const float b = byteValue(
                    parameters.offsets.y,
                    parameters.strides.y,
                    first + 1U,
                    chromaY);
                chromaU = format == 3U ? a : b;
                chromaV = format == 3U ? b : a;
            }
            rgb = yuvToRgb(
                luma,
                chromaU,
                chromaV,
                parameters.presentation.z,
                parameters.presentation.w);
        }
    } else {
        const uint index =
            parameters.offsets.x + y * parameters.strides.x;
        if (format == 6U) {
            rgb = vec3(
                byteAt(index + x * 3U),
                byteAt(index + x * 3U + 1U),
                byteAt(index + x * 3U + 2U)) / 255.0;
        } else if (format == 7U) {
            rgb = vec3(
                byteAt(index + x * 3U + 2U),
                byteAt(index + x * 3U + 1U),
                byteAt(index + x * 3U)) / 255.0;
        } else if (format == 8U) {
            rgb = vec3(
                byteAt(index + x * 4U),
                byteAt(index + x * 4U + 1U),
                byteAt(index + x * 4U + 2U)) / 255.0;
        } else if (format == 9U) {
            rgb = vec3(
                byteAt(index + x * 4U + 2U),
                byteAt(index + x * 4U + 1U),
                byteAt(index + x * 4U)) / 255.0;
        } else if (format == 10U) {
            rgb = vec3(
                byteAt(index + x * 4U + 1U),
                byteAt(index + x * 4U + 2U),
                byteAt(index + x * 4U + 3U)) / 255.0;
        } else {
            const float gray =
                float(byteAt(index + x)) / 255.0;
            rgb = vec3(gray);
        }
    }
    outputColor = vec4(presentColor(rgb), 1.0);
}
