#ifndef VULKAN_LAB_SCENE_LIGHTS_GLSL
#define VULKAN_LAB_SCENE_LIGHTS_GLSL

const uint LIGHT_TYPE_DIRECTIONAL = 0u;
const uint LIGHT_TYPE_POINT = 1u;
const uint LIGHT_TYPE_SPOT = 2u;

struct GpuLight {
    vec4 positionRange;
    vec4 directionInnerCos;
    vec4 colorIntensity;
    vec4 params;
};

layout(std430, set = 0, binding = 1) readonly buffer SceneLightBuffer {
    GpuLight lights[];
} sceneLightBuffer;

#endif
