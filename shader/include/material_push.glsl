#ifndef VULKAN_LAB_MATERIAL_PUSH_GLSL
#define VULKAN_LAB_MATERIAL_PUSH_GLSL

layout(push_constant) uniform PushConstants {
    mat4 model;
    uvec4 indices;
    vec4 reserved[3];
} push;

#endif
