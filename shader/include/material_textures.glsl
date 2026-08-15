#ifndef VULKAN_LAB_MATERIAL_TEXTURES_GLSL
#define VULKAN_LAB_MATERIAL_TEXTURES_GLSL

#extension GL_GOOGLE_include_directive : require
#include "include/material_data.glsl"

#if VKL_BINDLESS_MATERIALS
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 1, binding = 1) uniform sampler2D materialTextures[];

vec4 sampleMaterialTexture(uint index, vec2 uv)
{
    return texture(materialTextures[nonuniformEXT(index)], uv);
}

vec4 sampleBaseColor(vec2 uv) { return sampleMaterialTexture(materialData().textureIndices0.x, uv); }
vec4 sampleNormal(vec2 uv) { return sampleMaterialTexture(materialData().textureIndices0.y, uv); }
vec4 sampleMetallicRoughness(vec2 uv) { return sampleMaterialTexture(materialData().textureIndices0.z, uv); }
vec4 sampleOcclusion(vec2 uv) { return sampleMaterialTexture(materialData().textureIndices0.w, uv); }
vec4 sampleEmissive(vec2 uv) { return sampleMaterialTexture(materialData().textureIndices1.x, uv); }
#else
layout(set = 1, binding = 1) uniform sampler2D baseColorTexture;
layout(set = 1, binding = 2) uniform sampler2D normalTexture;
layout(set = 1, binding = 3) uniform sampler2D metallicRoughnessTexture;
layout(set = 1, binding = 4) uniform sampler2D occlusionTexture;
layout(set = 1, binding = 5) uniform sampler2D emissiveTexture;

vec4 sampleBaseColor(vec2 uv) { return texture(baseColorTexture, uv); }
vec4 sampleNormal(vec2 uv) { return texture(normalTexture, uv); }
vec4 sampleMetallicRoughness(vec2 uv) { return texture(metallicRoughnessTexture, uv); }
vec4 sampleOcclusion(vec2 uv) { return texture(occlusionTexture, uv); }
vec4 sampleEmissive(vec2 uv) { return texture(emissiveTexture, uv); }
#endif

#endif
