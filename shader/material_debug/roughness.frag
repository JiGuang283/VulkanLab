#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/material_push.glsl"

layout(location = 1) in vec2 fragTexCoord;
layout(location = 5) in vec4 fragColor;

layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 1, binding = 2) uniform sampler2D metallicRoughnessTexture;

layout(location = 0) out vec4 outColor;

bool isMaskAlphaMode()
{
    return abs(push.reserved.x - 1.0) < 0.5;
}

void applyAlphaCutoff(float alpha)
{
    if (isMaskAlphaMode() && alpha < push.roughnessAlpha.y) discard;
}

void main()
{
    float baseAlpha = texture(baseColorTexture, fragTexCoord).a *
                      push.baseColorFactor.a * fragColor.a;
    applyAlphaCutoff(baseAlpha);

    float roughness = texture(metallicRoughnessTexture, fragTexCoord).g *
                      push.roughnessAlpha.x;
    outColor = vec4(vec3(clamp(roughness, 0.0, 1.0)), 1.0);
}
