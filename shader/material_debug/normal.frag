#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/material_push.glsl"

layout(location = 0) in vec3 fragNormalWS;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec4 fragTangentWS;
layout(location = 5) in vec4 fragColor;

layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 1, binding = 1) uniform sampler2D normalTexture;

layout(location = 0) out vec4 outColor;

bool isMaskAlphaMode()
{
    return abs(push.reserved.x - 1.0) < 0.5;
}

void applyAlphaCutoff(float alpha)
{
    if (isMaskAlphaMode() && alpha < push.roughnessAlpha.y) discard;
}

void main()
{
    float baseAlpha = texture(baseColorTexture, fragTexCoord).a *
                      push.baseColorFactor.a * fragColor.a;
    applyAlphaCutoff(baseAlpha);

    vec3 n = normalize(fragNormalWS);
    vec3 t = normalize(fragTangentWS.xyz);
    t = normalize(t - n * dot(n, t));
    vec3 b = normalize(cross(n, t) * fragTangentWS.w);
    vec3 tangentNormal = texture(normalTexture, fragTexCoord).xyz * 2.0 - 1.0;
    tangentNormal.xy *= max(push.reserved.z, 0.0);
    vec3 normalWS = normalize(mat3(t, b, n) * tangentNormal);
    if (!gl_FrontFacing)
        normalWS = -normalWS;
    outColor = vec4(normalWS * 0.5 + 0.5, 1.0);
}
