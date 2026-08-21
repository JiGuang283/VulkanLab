#extension GL_GOOGLE_include_directive : require
#include "include/material_surface.glsl"
#include "include/surface_encoding.glsl"

#ifndef VKL_GBUFFER_ALPHA_MASKED
#define VKL_GBUFFER_ALPHA_MASKED 0
#endif
#ifndef VKL_GBUFFER_SHADING_MODEL
#define VKL_GBUFFER_SHADING_MODEL VKL_SHADING_MODEL_DEFAULT_LIT
#endif
#ifndef VKL_GBUFFER_RECEIVES_SCREEN_AO
#define VKL_GBUFFER_RECEIVES_SCREEN_AO 1
#endif
#ifndef VKL_GBUFFER_NORMAL_MAPPING
#define VKL_GBUFFER_NORMAL_MAPPING 1
#endif

layout(location = 0) in vec3 fragNormalWS;
layout(location = 1) in vec4 fragTangentWS;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec4 fragColor;
layout(location = 4) in vec4 fragCurrentClip;
layout(location = 5) in vec4 fragPreviousClip;
layout(location = 6) flat in uint fragHistoryValid;
layout(location = 7) in vec2 fragTexCoord1;

layout(location = 0) out vec4 outBaseColorMetallic;
layout(location = 1) out vec4 outNormalRoughnessOcclusion;
layout(location = 2) out vec4 outEmissiveSurfaceFlags;
layout(location = 3) out vec2 outMotion;

void main()
{
    EvaluatedMaterialSurface surface = vklEvaluateMaterialSurface(
        fragTexCoord, fragTexCoord1, fragColor);
#if VKL_GBUFFER_ALPHA_MASKED
    if (vklMaterialAlphaRejected(surface))
        discard;
#endif

    vec3 normalWS;
#if VKL_GBUFFER_NORMAL_MAPPING
    normalWS = vklMappedNormal(fragNormalWS, fragTangentWS,
                               fragTexCoord, gl_FrontFacing);
#else
    normalWS = vklGeometricNormal(fragNormalWS, gl_FrontFacing);
#endif
    uint surfaceFlags = vklPackSurfaceFlags(
        VKL_GBUFFER_SHADING_MODEL, fragHistoryValid != 0u,
        VKL_GBUFFER_RECEIVES_SCREEN_AO != 0);

    outBaseColorMetallic = vec4(surface.baseColor.rgb, surface.metallic);
    outNormalRoughnessOcclusion = vec4(
        vklOctEncode(normalWS), surface.roughness,
        surface.materialOcclusion);
    outEmissiveSurfaceFlags = vec4(surface.emissive,
                                   float(surfaceFlags));

    outMotion = vec2(0.0);
    if (fragHistoryValid != 0u && fragCurrentClip.w > 1e-6 &&
        fragPreviousClip.w > 1e-6) {
        vec2 currentUv = fragCurrentClip.xy / fragCurrentClip.w * 0.5 + 0.5;
        vec2 previousUv = fragPreviousClip.xy / fragPreviousClip.w * 0.5 + 0.5;
        outMotion = previousUv - currentUv;
    }
}
