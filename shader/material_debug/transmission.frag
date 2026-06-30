#version 450

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissiveMetallic;
    vec4 roughnessAlpha;
    vec4 reserved;
} push;

layout(location = 0) out vec4 outColor;

void main()
{
    float transmission = clamp(push.reserved.y, 0.0, 1.0);
    outColor = vec4(vec3(transmission), 1.0);
}
