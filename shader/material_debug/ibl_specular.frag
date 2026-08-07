#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/global_frame.glsl"
#include "include/material_push.glsl"
#include "include/ibl.glsl"

layout(location = 0) in vec3 fragPositionWS;
layout(location = 1) in vec3 fragNormalWS;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 5) in vec4 fragColor;

layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 1, binding = 2) uniform sampler2D metallicRoughnessTexture;

layout(location = 0) out vec4 outColor;

void main()
{
    vec3 n = normalize(gl_FrontFacing ? fragNormalWS : -fragNormalWS);
    vec3 v = normalize(ubo.cameraPosWS.xyz - fragPositionWS);
    vec3 albedo =
        (texture(baseColorTexture, fragTexCoord) *
         push.baseColorFactor * fragColor).rgb;
    vec4 mr = texture(metallicRoughnessTexture, fragTexCoord);
    float roughness =
        clamp(mr.g * push.roughnessAlpha.x, 0.04, 1.0);
    float metallic =
        clamp(mr.b * push.emissiveMetallic.w, 0.0, 1.0);
    vec3 globalSpecular = ubo.environmentParams.x < 0.5
        ? vec3(0.0)
        : evaluateIblSpecular(n, v, albedo, roughness, metallic) *
          ubo.environmentParams.y;
    vec3 color = evaluateReflectionProbeSpecular(
        fragPositionWS, n, v, albedo, roughness, metallic,
        globalSpecular);
    outColor = vec4(color, 1.0);
}
