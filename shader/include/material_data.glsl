#ifndef VULKAN_LAB_MATERIAL_DATA_GLSL
#define VULKAN_LAB_MATERIAL_DATA_GLSL

#extension GL_GOOGLE_include_directive : require
#include "include/material_push.glsl"

struct GpuMaterial {
    vec4 baseColorFactor;
    vec4 emissiveMetallic;
    vec4 roughnessAlphaOcclusionNormal;
    vec4 transmissionVolume;
    vec4 attenuationColor;
    uvec4 textureIndices0;
    uvec4 textureIndices1;
    uvec4 reserved;
};

layout(std430, set = 1, binding = 0) readonly buffer MaterialDataBuffer {
    GpuMaterial materials[];
} materialDataBuffer;

GpuMaterial materialData()
{
    return materialDataBuffer.materials[push.indices.x];
}

bool materialIsMask()
{
    return materialData().textureIndices1.z == 1u;
}

bool materialIsBlend()
{
    return materialData().textureIndices1.z == 2u;
}

#endif
