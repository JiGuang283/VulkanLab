#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/material_push.glsl"

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;

void main()
{
    float alpha = texture(baseColorTexture, fragTexCoord).a *
                  push.baseColorFactor.a * fragColor.a;
    if (alpha < push.roughnessAlpha.y)
        discard;
}
