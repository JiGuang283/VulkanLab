#ifndef VULKAN_LAB_PBR_SURFACE_LIGHTING_GLSL
#define VULKAN_LAB_PBR_SURFACE_LIGHTING_GLSL

#extension GL_GOOGLE_include_directive : require
#include "include/global_frame.glsl"
#include "include/ibl.glsl"
#include "include/atmosphere.glsl"
#include "include/ddgi_sampling.glsl"
#include "include/pbr_direct_lighting.glsl"

struct PbrSurfaceLightingInput {
    vec3 positionWS;
    vec3 normalWS;
    vec3 baseColor;
    float roughness;
    float metallic;
    float materialOcclusion;
    vec3 emissive;
    uint receivesScreenAo;
};

struct PbrSurfaceLightingResult {
    vec3 color;
    vec3 baselineDiffuse;
    vec3 baselineSpecular;
    uint debugOverride;
};

PbrSurfaceLightingResult vklEvaluatePbrSurfaceLighting(
    PbrSurfaceLightingInput surface, float screenAo)
{
    PbrSurfaceLightingResult result;
    vec3 view = normalize(ubo.cameraPosWS.xyz - surface.positionWS);
    float effectiveScreenAo = surface.receivesScreenAo != 0u
        ? screenAo : 1.0;
    float occlusion = surface.materialOcclusion * effectiveScreenAo;
    vec3 direct = vklEvaluateDirectLighting(
        surface.normalWS, view, surface.positionWS, surface.baseColor,
        surface.roughness, surface.metallic);
    IndirectLightingComponents indirect =
        evaluateIndirectLightingComponents(
            surface.normalWS, view, surface.baseColor, surface.roughness,
            surface.metallic, occlusion, surface.positionWS);
    if (ddgiIsActive()) {
        indirect.diffuse = evaluateDdgiDiffuse(
            surface.positionWS, surface.normalWS, view, surface.baseColor,
            surface.metallic, occlusion);
    }

    result.baselineSpecular = indirect.specular;
    result.baselineDiffuse = indirect.diffuse;
    result.debugOverride =
        ddgiIsActive() && ddgiSampling.updateWindow.w != 0u ? 1u : 0u;
    result.color = result.debugOverride != 0u
        ? evaluateDdgiDebug(surface.positionWS, surface.normalWS)
        : indirect.diffuse + indirect.specular + direct + surface.emissive;
    if (result.debugOverride != 0u) {
        result.baselineSpecular = vec3(0.0);
        result.baselineDiffuse = vec3(0.0);
    }
    return result;
}

void vklApplyPbrAerialPerspective(
    inout PbrSurfaceLightingResult lighting, vec3 positionWS)
{
    if (lighting.debugOverride != 0u || !atmosphereIsActive())
        return;
    vec3 viewRay = positionWS - ubo.cameraPosWS.xyz;
    float distanceToSurface = length(viewRay);
    if (distanceToSurface <= 1.0e-6)
        return;
    vec4 aerial = sampleAerialPerspective(
        viewRay / distanceToSurface, distanceToSurface);
    lighting.color = lighting.color * aerial.a + aerial.rgb *
        atmosphere.sunColorIntensity.rgb *
        atmosphere.sunColorIntensity.a;
    lighting.baselineSpecular *= aerial.a;
    lighting.baselineDiffuse *= aerial.a;
}

#endif
