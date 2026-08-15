#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/material_data.glsl"
#include "include/material_textures.glsl"

layout(location = 1) in vec2 fragTexCoord;
layout(location = 5) in vec4 fragColor;


layout(location = 0) out vec4 outColor;

bool isMaskAlphaMode()
{
    return abs(float(materialData().textureIndices1.z) - 1.0) < 0.5;
}

void applyAlphaCutoff(float alpha)
{
    if (isMaskAlphaMode() && alpha < materialData().roughnessAlphaOcclusionNormal.y) discard;
}

void main()
{
    float baseAlpha = sampleBaseColor( fragTexCoord).a *
                      materialData().baseColorFactor.a * fragColor.a;
    applyAlphaCutoff(baseAlpha);

    float roughness = sampleMetallicRoughness( fragTexCoord).g *
                      materialData().roughnessAlphaOcclusionNormal.x;
    outColor = vec4(vec3(clamp(roughness, 0.0, 1.0)), 1.0);
}
