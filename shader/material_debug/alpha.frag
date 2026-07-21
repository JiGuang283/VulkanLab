#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/material_push.glsl"

layout(location = 1) in vec2 fragTexCoord;
layout(location = 5) in vec4 fragColor;

layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;

layout(location = 0) out vec4 outColor;

void main()
{
    float alpha = texture(baseColorTexture, fragTexCoord).a *
                  push.baseColorFactor.a * fragColor.a;
    outColor = vec4(vec3(clamp(alpha, 0.0, 1.0)), 1.0);
}
