#ifndef VULKAN_LAB_GLOBAL_FRAME_GLSL
#define VULKAN_LAB_GLOBAL_FRAME_GLSL

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    mat4 inverseViewProjection;
    vec4 cameraPosWS;
    vec4 ambientColorIntensity;
    uvec4 lightCounts;
    mat4 directionalShadowViewProj;
    vec4 shadowParams;
    vec4 environmentParams;
} ubo;

#endif
