#extension GL_GOOGLE_include_directive : require
#include "include/material_surface.glsl"
#include "include/surface_encoding.glsl"

#ifndef VKL_SURFACE_NORMAL_OUTPUT
#define VKL_SURFACE_NORMAL_OUTPUT 1
#endif
#ifndef VKL_SURFACE_MOTION_OUTPUT
#define VKL_SURFACE_MOTION_OUTPUT 1
#endif
#ifndef VKL_SURFACE_ALBEDO_OUTPUT
#define VKL_SURFACE_ALBEDO_OUTPUT 1
#endif
#ifndef VKL_SURFACE_SHADING_MODEL
#define VKL_SURFACE_SHADING_MODEL VKL_SHADING_MODEL_DEFAULT_LIT
#endif
#ifndef VKL_SURFACE_RECEIVES_SCREEN_AO
#define VKL_SURFACE_RECEIVES_SCREEN_AO 1
#endif
#ifndef VKL_SURFACE_NORMAL_MAPPING
#define VKL_SURFACE_NORMAL_MAPPING 1
#endif

layout(location = 0) in vec3 fragNormalWS;
layout(location = 1) in vec4 fragTangentWS;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec4 fragColor;
layout(location = 4) in vec4 fragCurrentClip;
layout(location = 5) in vec4 fragPreviousClip;
layout(location = 6) flat in uint fragHistoryValid;


#if VKL_SURFACE_NORMAL_OUTPUT
layout(location = 0) out vec4 outNormalRoughness;
#endif
#if VKL_SURFACE_MOTION_OUTPUT
layout(location = 1) out vec2 outMotion;
#endif
#if VKL_SURFACE_ALBEDO_OUTPUT
layout(location = 2) out vec4 outAlbedoMetallic;
#endif

void main()
{
#if SURFACE_ALPHA_MASKED || VKL_SURFACE_ALBEDO_OUTPUT
    vec4 baseColor = vklEvaluateBaseColor(fragTexCoord, fragColor);
#endif
#if SURFACE_ALPHA_MASKED
    if (materialIsMask() &&
        baseColor.a < materialData().roughnessAlphaOcclusionNormal.y)
        discard;
#endif

#if VKL_SURFACE_NORMAL_OUTPUT
    vec3 normalWS;
#if VKL_SURFACE_NORMAL_MAPPING
    normalWS = vklMappedNormal(fragNormalWS, fragTangentWS,
                               fragTexCoord, gl_FrontFacing);
#else
    normalWS = vklGeometricNormal(fragNormalWS, gl_FrontFacing);
#endif
    vec2 roughnessMetallic = vklEvaluateRoughnessMetallic(fragTexCoord);
    uint surfaceFlags = vklPackSurfaceFlags(
        VKL_SURFACE_SHADING_MODEL, fragHistoryValid != 0u,
        VKL_SURFACE_RECEIVES_SCREEN_AO != 0);
    outNormalRoughness = vec4(vklOctEncode(normalWS),
                              roughnessMetallic.x,
                              float(surfaceFlags));
#endif
#if VKL_SURFACE_ALBEDO_OUTPUT
    float metallic = vklEvaluateRoughnessMetallic(fragTexCoord).y;
    outAlbedoMetallic = vec4(baseColor.rgb, metallic);
#endif

#if VKL_SURFACE_MOTION_OUTPUT
    outMotion = vec2(0.0);
    if (fragHistoryValid != 0u && fragCurrentClip.w > 1e-6 &&
        fragPreviousClip.w > 1e-6) {
        vec2 currentUv = fragCurrentClip.xy / fragCurrentClip.w * 0.5 + 0.5;
        vec2 previousUv = fragPreviousClip.xy / fragPreviousClip.w * 0.5 + 0.5;
        outMotion = previousUv - currentUv;
    }
#endif
}
