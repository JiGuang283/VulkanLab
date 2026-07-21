#ifndef VULKAN_LAB_MATERIAL_PUSH_GLSL
#define VULKAN_LAB_MATERIAL_PUSH_GLSL

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissiveMetallic;
    vec4 roughnessAlpha;
    vec4 reserved;
} push;

#endif
