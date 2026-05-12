#version 450

layout(set = 0, binding = 0) uniform UniformBufferObject{
    mat4 view;
    mat4 proj;
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

layout(location = 0) out vec3 fragNormalWS;
layout(location = 1) out vec2 fragTexCoord;

void main(){
    gl_Position = ubo.proj * ubo.view * push.model * vec4(inPosition, 1.0);
    // Correct normal transformation for any affine model matrix including
    // non-uniform scale: use the transpose of the inverse (normal matrix).
    fragNormalWS = normalize(transpose(inverse(mat3(push.model))) * inNormal);
    fragTexCoord = inTexCoord;
}
