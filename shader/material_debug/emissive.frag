#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/material_push.glsl"

layout(location = 1) in vec2 fragTexCoord;
layout(location = 5) in vec4 fragColor;

layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 1, binding = 4) uniform sampler2D emissiveTexture;

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

    vec3 emissive = texture(emissiveTexture, fragTexCoord).rgb *
                    push.emissiveMetallic.rgb;
    outColor = vec4(emissive, 1.0);
}
