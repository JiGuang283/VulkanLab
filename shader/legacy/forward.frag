#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/material_data.glsl"
#include "include/material_textures.glsl"

layout(location = 0) in vec3 fragNormalWS;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 5) in vec4 fragColor;


layout(location = 0) out vec4 outColor;

bool isMaskAlphaMode()
{
    return abs(float(materialData().textureIndices1.z) - 1.0) < 0.5;
}

void applyAlphaCutoff(float alpha)
{
    if (isMaskAlphaMode() && alpha < materialData().roughnessAlphaOcclusionNormal.y) discard;
}

void main()
{
    vec4 tex = sampleBaseColor(fragTexCoord);
    vec4 albedo = tex * materialData().baseColorFactor * fragColor;
    applyAlphaCutoff(albedo.a);

    vec3 n = normalize(fragNormalWS);
    if (!gl_FrontFacing)
        n = -n;
    vec3 L = normalize(vec3(0.3, 0.8, 0.5));
    float ndl = max(dot(n, L), 0.0);
    vec3 lit = albedo.rgb * (0.25 + 0.75 * ndl) + materialData().emissiveMetallic.rgb;
    outColor = vec4(lit, albedo.a);
}
