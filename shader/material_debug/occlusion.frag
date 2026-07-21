#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/material_push.glsl"

layout(location = 1) in vec2 fragTexCoord;
layout(location = 4) in vec2 fragTexCoord1;
layout(location = 5) in vec4 fragColor;

layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 1, binding = 3) uniform sampler2D occlusionTexture;

layout(location = 0) out vec4 outColor;

bool isMaskAlphaMode()
{
    return abs(push.reserved.x - 1.0) < 0.5;
}

void applyAlphaCutoff(float alpha)
{
    if (isMaskAlphaMode() && alpha < push.roughnessAlpha.y) discard;
}

vec2 occlusionTexCoord()
{
    return abs(push.roughnessAlpha.w - 1.0) < 0.5 ? fragTexCoord1
                                                  : fragTexCoord;
}

float materialOcclusion()
{
    float sampled = texture(occlusionTexture, occlusionTexCoord()).r;
    float strength = clamp(push.roughnessAlpha.z, 0.0, 1.0);
    return mix(1.0, sampled, strength);
}

void main()
{
    float baseAlpha = texture(baseColorTexture, fragTexCoord).a *
                      push.baseColorFactor.a * fragColor.a;
    applyAlphaCutoff(baseAlpha);

    float occlusion = materialOcclusion();
    outColor = vec4(vec3(occlusion), 1.0);
}
