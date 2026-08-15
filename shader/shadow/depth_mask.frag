#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/material_data.glsl"
#include "include/material_textures.glsl"

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;


void main()
{
    float alpha = sampleBaseColor( fragTexCoord).a *
                  materialData().baseColorFactor.a * fragColor.a;
    if (alpha < materialData().roughnessAlphaOcclusionNormal.y)
        discard;
}
