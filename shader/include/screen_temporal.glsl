#ifndef VULKAN_LAB_SCREEN_TEMPORAL_GLSL
#define VULKAN_LAB_SCREEN_TEMPORAL_GLSL

bool screenUvInBounds(vec2 uv)
{
    return all(greaterThanEqual(uv, vec2(0.0))) &&
           all(lessThanEqual(uv, vec2(1.0)));
}

vec2 reprojectScreenUv(vec2 uv, vec2 motion)
{
    return uv + motion;
}

bool temporalSurfaceAccepted(float currentDepth, float historyDepth,
                             vec3 currentNormal, vec3 historyNormal,
                             float depthThreshold, float normalThreshold)
{
    return abs(currentDepth - historyDepth) <= depthThreshold &&
           dot(currentNormal, historyNormal) >= normalThreshold;
}

#endif
