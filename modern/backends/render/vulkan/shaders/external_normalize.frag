#version 450

layout(location = 0) in vec2 textureCoordinate;
layout(location = 0) out vec4 outputColor;

layout(set = 0, binding = 0) uniform sampler2D sourceImage;
layout(push_constant) uniform Parameters {
    vec4 sourceRect;
} parameters;

void main()
{
    const vec2 coordinate = mix(
        parameters.sourceRect.xy,
        parameters.sourceRect.zw,
        clamp(textureCoordinate, vec2(0.0), vec2(1.0)));
    outputColor = vec4(texture(sourceImage, coordinate).rgb, 1.0);
}
