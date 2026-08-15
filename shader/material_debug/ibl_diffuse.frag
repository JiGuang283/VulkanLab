#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/global_frame.glsl"
#include "include/material_data.glsl"
#include "include/material_textures.glsl"
#include "include/ibl.glsl"

layout(location = 0) in vec3 fragPositionWS;
layout(location = 1) in vec3 fragNormalWS;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 5) in vec4 fragColor;


layout(location = 0) out vec4 outColor;

void main()
{
    vec3 n = normalize(gl_FrontFacing ? fragNormalWS : -fragNormalWS);
    vec3 v = normalize(ubo.cameraPosWS.xyz - fragPositionWS);
    vec3 albedo =
        (sampleBaseColor( fragTexCoord) *
         materialData().baseColorFactor * fragColor).rgb;
    vec4 mr = sampleMetallicRoughness( fragTexCoord);
    float roughness =
        clamp(mr.g * materialData().roughnessAlphaOcclusionNormal.x, 0.04, 1.0);
    float metallic =
        clamp(mr.b * materialData().emissiveMetallic.w, 0.0, 1.0);
    vec3 color = ubo.environmentParams.x < 0.5
        ? vec3(0.0)
        : evaluateIblDiffuse(n, v, albedo, roughness, metallic) *
          ubo.environmentParams.y;
    outColor = vec4(color, 1.0);
}
