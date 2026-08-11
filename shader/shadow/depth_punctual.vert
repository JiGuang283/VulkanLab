#version 450

// Minimal vertex shader for point/spot shadow rendering.
// Uses a dedicated UBO (set=0, binding=0) with a single mat4 viewProjection.

layout(set = 0, binding = 0) uniform PunctualShadowUniform {
    mat4 viewProjection;
    vec4 lightPositionFar;
} punctualShadow;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissiveMetallic;
    vec4 roughnessAlpha;
    vec4 reserved;
} push;

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
