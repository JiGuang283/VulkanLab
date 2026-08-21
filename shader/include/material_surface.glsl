#ifndef VULKAN_LAB_MATERIAL_SURFACE_GLSL
#define VULKAN_LAB_MATERIAL_SURFACE_GLSL

#extension GL_GOOGLE_include_directive : require
#include "include/material_data.glsl"
#include "include/material_textures.glsl"

const uint VKL_ALPHA_MODE_OPAQUE = 0u;
const uint VKL_ALPHA_MODE_MASK = 1u;
const uint VKL_ALPHA_MODE_BLEND = 2u;

struct EvaluatedMaterialSurface {
    vec4 baseColor;
    vec3 emissive;
    float metallic;
    float roughness;
    float materialOcclusion;
    float transmission;
    float thickness;
    uint alphaMode;
};

vec2 vklMaterialOcclusionUv(vec2 uv0, vec2 uv1)
{
    return materialData().textureIndices1.y == 1u ? uv1 : uv0;
}

vec4 vklEvaluateBaseColor(vec2 uv, vec4 vertexColor)
{
    return sampleBaseColor(uv) * materialData().baseColorFactor *
           vertexColor;
}

vec2 vklEvaluateRoughnessMetallic(vec2 uv)
{
    vec4 sampleValue = sampleMetallicRoughness(uv);
    return vec2(
        clamp(sampleValue.g *
                  materialData().roughnessAlphaOcclusionNormal.x,
              0.04, 1.0),
        clamp(sampleValue.b * materialData().emissiveMetallic.w,
              0.0, 1.0));
}

float vklEvaluateMaterialOcclusion(vec2 uv0, vec2 uv1)
{
    float sampledOcclusion = sampleOcclusion(
        vklMaterialOcclusionUv(uv0, uv1)).r;
    return mix(1.0, sampledOcclusion,
               clamp(materialData().roughnessAlphaOcclusionNormal.z,
                     0.0, 1.0));
}

vec3 vklEvaluateEmissive(vec2 uv)
{
    return sampleEmissive(uv).rgb * materialData().emissiveMetallic.rgb;
}

EvaluatedMaterialSurface vklEvaluateMaterialSurface(
    vec2 uv0, vec2 uv1, vec4 vertexColor)
{
    EvaluatedMaterialSurface surface;
    surface.baseColor = vklEvaluateBaseColor(uv0, vertexColor);
    vec2 roughnessMetallic = vklEvaluateRoughnessMetallic(uv0);
    surface.roughness = roughnessMetallic.x;
    surface.metallic = roughnessMetallic.y;
    surface.materialOcclusion = vklEvaluateMaterialOcclusion(uv0, uv1);
    surface.emissive = vklEvaluateEmissive(uv0);
    surface.transmission = clamp(
        materialData().transmissionVolume.x, 0.0, 1.0);
    surface.thickness = max(materialData().transmissionVolume.y, 0.0);
    surface.alphaMode = materialData().textureIndices1.z;
    return surface;
}

bool vklMaterialIsMask(EvaluatedMaterialSurface surface)
{
    return surface.alphaMode == VKL_ALPHA_MODE_MASK;
}

bool vklMaterialIsBlend(EvaluatedMaterialSurface surface)
{
    return surface.alphaMode == VKL_ALPHA_MODE_BLEND;
}

bool vklMaterialAlphaRejected(EvaluatedMaterialSurface surface)
{
    return vklMaterialIsMask(surface) &&
           surface.baseColor.a <
               materialData().roughnessAlphaOcclusionNormal.y;
}

float vklMaterialOutputAlpha(EvaluatedMaterialSurface surface)
{
    if (surface.transmission > 0.0 && !vklMaterialIsBlend(surface))
        return mix(1.0, 0.28, surface.transmission);
    return surface.baseColor.a;
}

vec3 vklGeometricNormal(vec3 normalWS, bool frontFacing)
{
    vec3 normal = normalize(normalWS);
    return frontFacing ? normal : -normal;
}

vec3 vklMappedNormal(vec3 normalWS, vec4 tangentWS, vec2 uv,
                     bool frontFacing)
{
    vec3 normal = normalize(normalWS);
    vec3 tangent = normalize(tangentWS.xyz);
    tangent = normalize(tangent - normal * dot(normal, tangent));
    vec3 bitangent = normalize(cross(normal, tangent) * tangentWS.w);
    vec3 tangentNormal = sampleNormal(uv).xyz * 2.0 - 1.0;
    tangentNormal.xy *= max(
        materialData().roughnessAlphaOcclusionNormal.w, 0.0);
    vec3 mapped = normalize(mat3(tangent, bitangent, normal) *
                            tangentNormal);
    return frontFacing ? mapped : -mapped;
}

vec3 vklApplyTransmissionApprox(EvaluatedMaterialSurface surface,
                                vec3 color, vec3 normal, vec3 view)
{
    if (surface.transmission <= 0.0)
        return color;
    float ndv = max(dot(normal, view), 0.0);
    float rim = pow(clamp(1.0 - ndv, 0.0, 1.0), 4.0);
    float smoothness = 1.0 - surface.roughness;
    vec3 rimColor = vec3(0.35 + 0.45 * smoothness);
    vec3 tint = mix(color, vec3(1.0), 0.12 * surface.transmission);
    return tint + rimColor * rim * surface.transmission;
}

#endif
