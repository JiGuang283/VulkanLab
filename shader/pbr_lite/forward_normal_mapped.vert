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
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissiveMetallic;
    vec4 roughnessAlpha;
    vec4 reserved;
} push;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec2 inTexCoord1;
layout(location = 5) in vec4 inColor;

layout(location = 0) out vec3 fragPositionWS;
layout(location = 1) out vec3 fragNormalWS;
layout(location = 2) out vec2 fragTexCoord;
layout(location = 3) out vec4 fragTangentWS;
layout(location = 4) out vec2 fragTexCoord1;
layout(location = 5) out vec4 fragColor;

void main()
{
    vec4 positionWS = push.model * vec4(inPosition, 1.0);
    mat3 normalMatrix = transpose(inverse(mat3(push.model)));
    vec3 normalWS = normalize(normalMatrix * inNormal);
    vec3 tangentWS = normalize(mat3(push.model) * inTangent.xyz);
    tangentWS = normalize(tangentWS - normalWS * dot(normalWS, tangentWS));

    gl_Position = ubo.proj * ubo.view * positionWS;
    fragPositionWS = positionWS.xyz;
    fragNormalWS = normalWS;
    fragTexCoord = inTexCoord;
    fragTangentWS = vec4(tangentWS, inTangent.w);
    fragTexCoord1 = inTexCoord1;
    fragColor = inColor;
}
