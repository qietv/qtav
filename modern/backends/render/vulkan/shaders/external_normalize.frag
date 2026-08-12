#version 450

layout(location = 0) in vec2 textureCoordinate;
layout(location = 0) out vec4 outputColor;

layout(set = 0, binding = 0) uniform sampler2D sourceImage;
layout(push_constant) uniform Parameters {
    vec4 sourceRect;
    vec4 normalization;
} parameters;

void main()
{
    const vec2 coordinate = mix(
        parameters.sourceRect.xy,
        parameters.sourceRect.zw,
        clamp(textureCoordinate, vec2(0.0), vec2(1.0)));
    const vec3 sampleValue = texture(sourceImage, coordinate).rgb;
    // Vulkan's YCbCr sampler convention assigns Cr/Y/Cb to R/G/B. With
    // RGB_IDENTITY the encoded values are unmodified, so normalize them into
    // the Y/Cb/Cr component order expected by libplacebo. A driver-suggested
    // conversion already returns RGB and must remain untouched.
    const vec3 normalized = parameters.normalization.x > 0.5
        ? sampleValue.gbr
        : sampleValue.rgb;
    outputColor = vec4(normalized, 1.0);
}
