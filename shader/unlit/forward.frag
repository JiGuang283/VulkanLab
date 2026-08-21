#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/material_surface.glsl"

layout(location = 1) in vec2 fragTexCoord;
layout(location = 5) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 baseColor = vklEvaluateBaseColor(fragTexCoord, fragColor);
    if (materialIsMask() &&
        baseColor.a < materialData().roughnessAlphaOcclusionNormal.y) {
        discard;
    }
    outColor = baseColor;
}
