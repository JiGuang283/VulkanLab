#ifndef VULKAN_LAB_GLOBAL_FRAME_GLSL
#define VULKAN_LAB_GLOBAL_FRAME_GLSL

#define CSM_CASCADE_COUNT 4

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    mat4 inverseViewProjection;
    vec4 cameraPosWS;
    vec4 ambientColorIntensity;
    uvec4 lightCounts;
    mat4 cascadeViewProj[CSM_CASCADE_COUNT];
    vec4 cascadeSplits;
    vec4 cascadeBlendStarts;
    vec4 shadowParams;
    ivec4 punctualShadowCounts;
    vec4 punctualShadowParams;
    mat4 spotShadowViewProj[4];
    vec4 environmentParams;
    uvec4 clusterGrid;
    uvec4 clusterViewport;
    vec4 clusterDepthParams;
} ubo;

#endif
