#extension GL_GOOGLE_include_directive : require
#include "include/material_push.glsl"

layout(location = 0) in vec3 fragNormalWS;
layout(location = 1) in vec4 fragTangentWS;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec4 fragColor;
layout(location = 4) in vec4 fragCurrentClip;
layout(location = 5) in vec4 fragPreviousClip;
layout(location = 6) flat in uint fragHistoryValid;

layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 1, binding = 1) uniform sampler2D normalTexture;
layout(set = 1, binding = 2) uniform sampler2D metallicRoughnessTexture;

layout(location = 0) out vec4 outNormalRoughness;
layout(location = 1) out vec2 outMotion;
layout(location = 2) out vec4 outAlbedoMetallic;

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
    vec3 tangentNormal = texture(normalTexture, fragTexCoord).xyz * 2.0 - 1.0;
    tangentNormal.xy *= max(push.reserved.z, 0.0);
    vec3 result = normalize(mat3(t, b, n) * tangentNormal);
    return gl_FrontFacing ? result : -result;
}

void main()
{
    vec4 baseColor = texture(baseColorTexture, fragTexCoord) *
                     push.baseColorFactor * fragColor;
#if SURFACE_ALPHA_MASKED
    if (baseColor.a < push.roughnessAlpha.y)
        discard;
#endif

    vec3 normalWS = materialNormal();
    float roughness = clamp(texture(metallicRoughnessTexture,
                                    fragTexCoord).g *
                                push.roughnessAlpha.x,
                            0.04, 1.0);
    outNormalRoughness = vec4(octEncode(normalWS), roughness,
                              fragHistoryValid != 0u ? 1.0 : 0.0);
    float metallic = clamp(texture(metallicRoughnessTexture,
                                   fragTexCoord).b *
                               push.emissiveMetallic.w,
                           0.0, 1.0);
    outAlbedoMetallic = vec4(baseColor.rgb, metallic);

    outMotion = vec2(0.0);
    if (fragHistoryValid != 0u && fragCurrentClip.w > 1e-6 &&
        fragPreviousClip.w > 1e-6) {
        vec2 currentUv = fragCurrentClip.xy / fragCurrentClip.w * 0.5 + 0.5;
        vec2 previousUv = fragPreviousClip.xy / fragPreviousClip.w * 0.5 + 0.5;
        outMotion = previousUv - currentUv;
    }
}
