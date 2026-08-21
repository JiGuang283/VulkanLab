#ifndef VULKAN_LAB_CLUSTERED_LIGHTING_GLSL
#define VULKAN_LAB_CLUSTERED_LIGHTING_GLSL

#define VKL_CLUSTER_OVERFLOW_BIT 0x80000000u

layout(std430, set = 6, binding = 0) readonly buffer ClusterLightCounts {
    uint counts[];
} clusterLightCounts;

layout(std430, set = 6, binding = 1) readonly buffer ClusterLightIndices {
    uint indices[];
} clusterLightIndices;

bool vklClusterIndexForWorldPosition(vec3 positionWS, out uint clusterIndex,
                                     out uint storedCount,
                                     out bool overflowed)
{
    clusterIndex = 0u;
    storedCount = 0u;
    overflowed = false;
    if (ubo.clusterViewport.w == 0u || ubo.clusterGrid.x == 0u ||
        ubo.clusterGrid.y == 0u || ubo.clusterGrid.z == 0u)
        return false;

    vec4 positionVS4 = ubo.view * vec4(positionWS, 1.0);
    float viewDepth = -positionVS4.z;
    if (isnan(viewDepth) || isinf(viewDepth) || viewDepth <= 0.0)
        return false;

    vec4 clip = ubo.proj * positionVS4;
    if (isnan(clip.w) || isinf(clip.w) || abs(clip.w) <= 1.0e-6)
        return false;
    vec2 uv = (clip.xy / clip.w) * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) ||
        any(greaterThanEqual(uv, vec2(1.0))))
        return false;

    uvec2 pixel = min(uvec2(uv * vec2(ubo.clusterViewport.xy)),
                      ubo.clusterViewport.xy - uvec2(1u));
    uvec2 tile = min(pixel / max(ubo.clusterViewport.z, 1u),
                     ubo.clusterGrid.xy - uvec2(1u));
    float sliceValue = log2(max(viewDepth, ubo.clusterDepthParams.x)) *
                           ubo.clusterDepthParams.z +
                       ubo.clusterDepthParams.w;
    uint slice = uint(clamp(floor(sliceValue), 0.0,
                            float(ubo.clusterGrid.z - 1u)));
    clusterIndex = tile.x + tile.y * ubo.clusterGrid.x +
                   slice * ubo.clusterGrid.x * ubo.clusterGrid.y;
    uint packedCount = clusterLightCounts.counts[clusterIndex];
    overflowed = (packedCount & VKL_CLUSTER_OVERFLOW_BIT) != 0u;
    storedCount = packedCount & ~VKL_CLUSTER_OVERFLOW_BIT;
    return true;
}

uint vklClusterLightIndex(uint clusterIndex, uint localIndex)
{
    return clusterLightIndices.indices[
        clusterIndex * ubo.clusterGrid.w + localIndex];
}

#endif
