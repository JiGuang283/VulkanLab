#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/material_data.glsl"
#include "include/material_textures.glsl"

layout(set = 0, binding = 0) uniform PunctualShadowUniform {
    mat4 viewProjection;
    vec4 lightPositionFar;
} punctualShadow;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in vec3 fragWorldPosition;


void main()
{
    float alpha = sampleBaseColor( fragTexCoord).a *
                  materialData().baseColorFactor.a * fragColor.a;
    if (alpha < materialData().roughnessAlphaOcclusionNormal.y)
        discard;

    float farPlane = max(punctualShadow.lightPositionFar.w, 0.0001);
    gl_FragDepth = clamp(
        length(fragWorldPosition - punctualShadow.lightPositionFar.xyz) /
            farPlane,
        0.0, 1.0);
}
