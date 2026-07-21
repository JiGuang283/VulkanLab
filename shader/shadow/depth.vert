#version 450

const int MAX_DIRECTIONAL_LIGHTS = 1;
const int MAX_PUNCTUAL_LIGHTS = 8;

struct GpuLight {
    vec4 positionRange;
    vec4 directionInnerCos;
    vec4 colorIntensity;
    vec4 params;
};

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec4 cameraPosWS;
    vec4 ambientColorIntensity;
    vec4 lightCounts;
    GpuLight directionalLights[MAX_DIRECTIONAL_LIGHTS];
    GpuLight punctualLights[MAX_PUNCTUAL_LIGHTS];
    mat4 directionalShadowViewProj;
    vec4 shadowParams;
} ubo;

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

void main()
{
    gl_Position = ubo.directionalShadowViewProj * push.model *
                  vec4(inPosition, 1.0);
    fragTexCoord = inTexCoord;
    fragColor = inColor;
}
