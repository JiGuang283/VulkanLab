#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/material_push.glsl"

// Minimal vertex shader for point/spot shadow rendering.
// Uses a dedicated UBO (set=0, binding=0) with a single mat4 viewProjection.

layout(set = 0, binding = 0) uniform PunctualShadowUniform {
    mat4 viewProjection;
    vec4 lightPositionFar;
} punctualShadow;

layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inTexCoord;
layout(location = 5) in vec4 inColor;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out vec3 fragWorldPosition;

void main()
{
    vec4 worldPosition = push.model * vec4(inPosition, 1.0);
    gl_Position = punctualShadow.viewProjection * worldPosition;
    fragTexCoord = inTexCoord;
    fragColor = inColor;
    fragWorldPosition = worldPosition.xyz;
}
