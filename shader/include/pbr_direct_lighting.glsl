#ifndef VULKAN_LAB_PBR_DIRECT_LIGHTING_GLSL
#define VULKAN_LAB_PBR_DIRECT_LIGHTING_GLSL

#extension GL_GOOGLE_include_directive : require
#include "include/global_frame.glsl"
#include "include/scene_lights.glsl"
#include "include/atmosphere.glsl"
#include "include/shadow_sampling.glsl"
#include "include/pbr_brdf.glsl"
#include "include/clustered_lighting.glsl"

float vklRangeAttenuation(float distanceToLight, float range)
{
    float attenuation = 1.0 / max(distanceToLight * distanceToLight, 0.01);
    if (range <= 0.0)
        return attenuation;
    float normalizedDistance = clamp(distanceToLight / range, 0.0, 1.0);
    float fade = clamp(1.0 - pow(normalizedDistance, 4.0), 0.0, 1.0);
    return attenuation * fade * fade;
}

float vklSpotAttenuation(vec3 light, vec3 spotDirection,
                         float innerCos, float outerCos)
{
    float cosAngle = dot(normalize(-light), normalize(spotDirection));
    float cone = clamp((cosAngle - outerCos) /
                       max(innerCos - outerCos, 0.001), 0.0, 1.0);
    return cone * cone;
}

void vklEvaluatePunctualLight(uint lightIndex, uint spotOffset,
                              vec3 normal, vec3 view, vec3 positionWS,
                              vec3 albedo, float roughness, float metallic,
                              inout vec3 direct)
{
    GpuLight gpuLight = sceneLightBuffer.lights[lightIndex];
    vec3 toLight = gpuLight.positionRange.xyz - positionWS;
    float distanceToLight = length(toLight);
    if (distanceToLight <= 0.0001 ||
        (gpuLight.positionRange.w > 0.0 &&
         distanceToLight >= gpuLight.positionRange.w))
        return;

    vec3 light = toLight / distanceToLight;
    float attenuation = vklRangeAttenuation(
        distanceToLight, gpuLight.positionRange.w);
    float visibility = 1.0;
    if (lightIndex < spotOffset) {
        visibility = pointShadowVisibility(
            positionWS, gpuLight.positionRange.xyz,
            int(gpuLight.params.z), gpuLight.params.w);
    } else {
        attenuation *= vklSpotAttenuation(
            light, gpuLight.directionInnerCos.xyz,
            gpuLight.directionInnerCos.w, gpuLight.params.y);
        visibility = spotShadowVisibility(
            positionWS, int(gpuLight.params.z));
    }
    vec3 radiance = gpuLight.colorIntensity.rgb *
                    gpuLight.colorIntensity.a * attenuation;
    direct += visibility * vklEvaluatePbrLight(
        normal, view, light, radiance, albedo, roughness, metallic);
}

vec3 vklEvaluateDirectLighting(vec3 normal, vec3 view, vec3 positionWS,
                               vec3 albedo, float roughness,
                               float metallic)
{
    vec3 direct = vec3(0.0);
    uint directionalCount = ubo.lightCounts.x;
    uint pointCount = ubo.lightCounts.y;
    uint spotCount = ubo.lightCounts.z;
    int shadowLightIndex = int(ubo.shadowParams.w);

    for (uint i = 0u; i < directionalCount; ++i) {
        GpuLight gpuLight = sceneLightBuffer.lights[i];
        vec3 light = normalize(gpuLight.directionInnerCos.xyz);
        vec3 radiance = gpuLight.colorIntensity.rgb *
                        gpuLight.colorIntensity.a;
        if (atmosphereIsActive() && i == atmosphere.runtimeParams.y) {
            radiance *= sampleAtmosphereTransmittance(
                atmosphereWorldPositionKm(positionWS), light);
        }
        float visibility = int(i) == shadowLightIndex
                               ? csmShadowVisibility(positionWS)
                               : 1.0;
        direct += visibility * vklEvaluatePbrLight(
            normal, view, light, radiance, albedo, roughness, metallic);
    }

    uint pointOffset = directionalCount;
    uint spotOffset = pointOffset + pointCount;
    uint clusterIndex = 0u;
    uint clusteredCount = 0u;
    bool clusterOverflowed = false;
    bool clustered = vklClusterIndexForWorldPosition(
        positionWS, clusterIndex, clusteredCount, clusterOverflowed);
    if (clustered && !clusterOverflowed) {
        for (uint i = 0u; i < clusteredCount; ++i) {
            vklEvaluatePunctualLight(
                vklClusterLightIndex(clusterIndex, i), spotOffset,
                normal, view, positionWS, albedo, roughness, metallic,
                direct);
        }
    } else {
        uint punctualCount = pointCount + spotCount;
        for (uint i = 0u; i < punctualCount; ++i) {
            vklEvaluatePunctualLight(
                pointOffset + i, spotOffset, normal, view, positionWS,
                albedo, roughness, metallic, direct);
        }
    }
    return direct;
}

#endif
