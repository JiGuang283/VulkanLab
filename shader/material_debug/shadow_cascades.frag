#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/global_frame.glsl"
#include "include/shadow_sampling.glsl"

layout(location = 0) in vec3 fragPositionWS;
layout(location = 0) out vec4 outColor;

const vec3 kCascadeColors[CSM_CASCADE_COUNT] = vec3[](
    vec3(0.90, 0.20, 0.18),
    vec3(0.20, 0.75, 0.30),
    vec3(0.20, 0.45, 0.95),
    vec3(0.90, 0.70, 0.18));

void main()
{
    float viewZ = -(ubo.view * vec4(fragPositionWS, 1.0)).z;
    uint cascadeIndex = csmCascadeIndex(viewZ);
    float blend = csmCascadeBlendFactor(cascadeIndex, viewZ);
    vec3 color = kCascadeColors[cascadeIndex];
    if (cascadeIndex < CSM_CASCADE_COUNT - 1u)
        color = mix(color, kCascadeColors[cascadeIndex + 1u], blend);
    float visibility = csmShadowVisibility(fragPositionWS);
    outColor = vec4(color * mix(0.25, 1.0, visibility), 1.0);
}
