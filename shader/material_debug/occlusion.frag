#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/material_data.glsl"
#include "include/material_textures.glsl"

layout(location = 1) in vec2 fragTexCoord;
layout(location = 4) in vec2 fragTexCoord1;
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

vec2 occlusionTexCoord()
{
    return abs(float(materialData().textureIndices1.y) - 1.0) < 0.5 ? fragTexCoord1
                                                  : fragTexCoord;
}

float materialOcclusion()
{
    float sampled = sampleOcclusion( occlusionTexCoord()).r;
    float strength = clamp(materialData().roughnessAlphaOcclusionNormal.z, 0.0, 1.0);
    return mix(1.0, sampled, strength);
}

void main()
{
    float baseAlpha = sampleBaseColor( fragTexCoord).a *
                      materialData().baseColorFactor.a * fragColor.a;
    applyAlphaCutoff(baseAlpha);

    float occlusion = materialOcclusion();
    outColor = vec4(vec3(occlusion), 1.0);
}
