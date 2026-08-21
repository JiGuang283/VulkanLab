#ifndef VULKAN_LAB_DEPTH_HIERARCHY_GLSL
#define VULKAN_LAB_DEPTH_HIERARCHY_GLSL

float vklSampleNearestDepth(sampler2D hierarchy, vec2 uv, float mip)
{
    return textureLod(hierarchy, uv, mip).r;
}

float vklFetchNearestDepth(sampler2D hierarchy, ivec2 pixel, int mip)
{
    return texelFetch(hierarchy, pixel, mip).r;
}

float vklSampleFarthestDepth(sampler2D hierarchy, vec2 uv, float mip,
                             bool combinedMinMax)
{
    vec2 depthRange = textureLod(hierarchy, uv, mip).rg;
    return combinedMinMax ? depthRange.g : depthRange.r;
}

#endif
