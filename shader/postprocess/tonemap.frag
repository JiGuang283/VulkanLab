#version 450

layout(location = 0) in vec2 fragUv;
layout(set = 0, binding = 0) uniform sampler2D hdrColor;
layout(set = 0, binding = 1) uniform sampler2D bloomColor;

layout(push_constant) uniform ToneMapPushConstants {
    float exposureEv;
    float bloomIntensity;
    uint toneMapper;
    uint encodeGamma;
    uint applyExposure;
    uint applyBloom;
    uint reserved0;
    uint reserved1;
} push;

layout(location = 0) out vec4 outColor;

vec3 acesFitted(vec3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) /
                 (color * (c * color + d) + e), 0.0, 1.0);
}

void main()
{
    vec3 color = max(texture(hdrColor, fragUv).rgb, vec3(0.0));
    if (push.applyBloom != 0u) {
        color += max(texture(bloomColor, fragUv).rgb, vec3(0.0)) *
                 push.bloomIntensity;
    }
    if (push.applyExposure != 0u)
        color *= exp2(push.exposureEv);
    if (push.toneMapper == 1u)
        color = color / (vec3(1.0) + color);
    else if (push.toneMapper == 2u)
        color = acesFitted(color);
    if (push.encodeGamma != 0u)
        color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
    outColor = vec4(color, 1.0);
}
