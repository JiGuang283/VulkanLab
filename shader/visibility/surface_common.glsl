#extension GL_GOOGLE_include_directive : require
#include "include/material_data.glsl"
#include "include/material_textures.glsl"

#ifndef VKL_SURFACE_NORMAL_OUTPUT
#define VKL_SURFACE_NORMAL_OUTPUT 1
#endif
#ifndef VKL_SURFACE_MOTION_OUTPUT
#define VKL_SURFACE_MOTION_OUTPUT 1
#endif
#ifndef VKL_SURFACE_ALBEDO_OUTPUT
#define VKL_SURFACE_ALBEDO_OUTPUT 1
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

vec2 octEncode(vec3 normal)
{
    normal /= abs(normal.x) + abs(normal.y) + abs(normal.z);
    vec2 encoded = normal.xy;
    if (normal.z < 0.0)
        encoded = (1.0 - abs(encoded.yx)) * sign(encoded.xy);
    return encoded * 0.5 + 0.5;
}

vec3 materialNormal()
{
    vec3 n = normalize(fragNormalWS);
    vec3 t = normalize(fragTangentWS.xyz);
    t = normalize(t - n * dot(n, t));
    vec3 b = normalize(cross(n, t) * fragTangentWS.w);
    vec3 tangentNormal = sampleNormal( fragTexCoord).xyz * 2.0 - 1.0;
    tangentNormal.xy *= max(materialData().roughnessAlphaOcclusionNormal.w, 0.0);
    vec3 result = normalize(mat3(t, b, n) * tangentNormal);
    return gl_FrontFacing ? result : -result;
}

void main()
{
#if SURFACE_ALPHA_MASKED || VKL_SURFACE_ALBEDO_OUTPUT
    vec4 baseColor = sampleBaseColor( fragTexCoord) *
                     materialData().baseColorFactor * fragColor;
#endif
#if SURFACE_ALPHA_MASKED
    if (baseColor.a < materialData().roughnessAlphaOcclusionNormal.y)
        discard;
#endif

#if VKL_SURFACE_NORMAL_OUTPUT
    vec3 normalWS = materialNormal();
    float roughness = clamp(sampleMetallicRoughness(
                                    fragTexCoord).g *
                                materialData().roughnessAlphaOcclusionNormal.x,
                            0.04, 1.0);
    outNormalRoughness = vec4(octEncode(normalWS), roughness,
                              fragHistoryValid != 0u ? 1.0 : 0.0);
#endif
#if VKL_SURFACE_ALBEDO_OUTPUT
    float metallic = clamp(sampleMetallicRoughness(
                                   fragTexCoord).b *
                               materialData().emissiveMetallic.w,
                           0.0, 1.0);
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
