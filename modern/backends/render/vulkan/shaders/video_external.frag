#version 450

layout(location = 0) out vec4 outputColor;

layout(set = 0, binding = 0) uniform sampler2D sourceImage;

layout(std140, set = 0, binding = 1) uniform Parameters {
    // width, height, unused, output color space
    uvec4 source;
    uvec4 strides;
    uvec4 offsets;
    // surface width, height, transfer, primaries
    uvec4 surface;
    // x, y, width, height
    uvec4 viewport;
    // rotation, aspect ratio, unused, unused
    uvec4 presentation;
    // reference white, maximum source luminance, unused, unused
    vec4 luminance;
    // normalized source crop: left, top, right, bottom
    vec4 normalizedSourceRect;
} parameters;

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
    const float sourceWhite = parameters.luminance.x;
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
    coordinate = mix(
        parameters.normalizedSourceRect.xy,
        parameters.normalizedSourceRect.zw,
        coordinate);
    const vec3 rgb = texture(sourceImage, coordinate).rgb;
    outputColor = vec4(presentColor(rgb), 1.0);
}
