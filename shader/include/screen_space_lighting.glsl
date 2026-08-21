#ifndef VULKAN_LAB_SCREEN_SPACE_LIGHTING_GLSL
#define VULKAN_LAB_SCREEN_SPACE_LIGHTING_GLSL

layout(std140, set = 4, binding = 0) uniform ScreenSpaceLightingBuffer {
    vec4 viewportSizeInvSize;
    uvec4 modes;
} screenSpaceLighting;

layout(set = 4, binding = 1) uniform sampler2D filteredScreenSpaceAo;

float screenSpaceAmbientOcclusionAtUv(vec2 uv)
{
    if (screenSpaceLighting.modes.y == 0u)
        return 1.0;
    return clamp(texture(filteredScreenSpaceAo, uv).r, 0.0, 1.0);
}

#ifndef VKL_SCREEN_SPACE_COMPUTE
float screenSpaceAmbientOcclusion()
{
    vec2 uv = gl_FragCoord.xy * screenSpaceLighting.viewportSizeInvSize.zw;
    return screenSpaceAmbientOcclusionAtUv(uv);
}
#endif

#endif
