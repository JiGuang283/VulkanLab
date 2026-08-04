#ifndef VULKAN_LAB_SURFACE_FRAME_GLSL
#define VULKAN_LAB_SURFACE_FRAME_GLSL

layout(std140, set = 2, binding = 0) uniform SurfaceFrameBuffer {
    mat4 previousViewProjection;
    vec4 viewportSizeInvSize;
    uvec4 params;
} surfaceFrame;

struct RenderItemHistory {
    mat4 previousWorld;
    uvec4 params;
};

layout(std430, set = 2, binding = 1) readonly buffer RenderItemHistoryBuffer {
    RenderItemHistory items[];
} surfaceHistory;

#endif
