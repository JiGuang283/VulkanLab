#ifndef VULKAN_LAB_SHADOW_SAMPLING_GLSL
#define VULKAN_LAB_SHADOW_SAMPLING_GLSL

// Requires:
//   #include "include/global_frame.glsl"

layout(set = 2, binding = 0) uniform sampler2DArrayShadow directionalShadowMap;
layout(set = 2, binding = 7) uniform samplerCubeArrayShadow pointShadowMap;
layout(set = 2, binding = 8) uniform sampler2DArrayShadow spotShadowMap;

float csmShadowVisibility(vec3 positionWS) {
    if (ubo.shadowParams.x < 0.5)
        return 1.0;

    float viewZ = -(ubo.view * vec4(positionWS, 1.0)).z;

    uint cascadeIndex = 0u;
    for (uint i = 0u; i < CSM_CASCADE_COUNT - 1u; ++i) {
        if (viewZ > ubo.cascadeSplits[i])
            cascadeIndex = i + 1u;
    }

    vec4 clip = ubo.cascadeViewProj[cascadeIndex] * vec4(positionWS, 1.0);
    vec3 coord = clip.xyz / clip.w;
    vec2 uv = coord.xy * 0.5 + 0.5;

    if (coord.z <= 0.0 || coord.z >= 1.0 ||
        any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 1.0;

    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(x, y) * ubo.shadowParams.z;
            visibility += texture(
                directionalShadowMap,
                vec4(uv + offset,
                     float(cascadeIndex),
                     coord.z - ubo.shadowParams.y));
        }
    }
    return visibility / 9.0;
}

float pointShadowVisibility(vec3 positionWS, vec3 lightPos,
                            int shadowSlot, float shadowFar) {
    int pointCount = ubo.punctualShadowCounts.x;
    if (pointCount <= 0 || shadowSlot < 0 || shadowSlot >= pointCount ||
        shadowFar <= 0.0)
        return 1.0;

    vec3 dir = positionWS - lightPos;
    float dist = length(dir);
    if (dist <= 0.0001 || dist >= shadowFar)
        return 1.0;
    dir /= dist;

    float texelSize = 1.0 / float(ubo.punctualShadowCounts.z);
    float pointBiasWorld = max(ubo.punctualShadowParams.x, 0.0);
    float depthNorm = max(dist - pointBiasWorld, 0.0) / shadowFar;
    vec3 helper = abs(dir.z) < 0.999 ? vec3(0.0, 0.0, 1.0)
                                    : vec3(0.0, 1.0, 0.0);
    vec3 tangent = normalize(cross(helper, dir));
    vec3 bitangent = cross(dir, tangent);
    float angularTexel = texelSize * 2.0;

    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec3 sampleDir = normalize(
                dir + (float(x) * tangent + float(y) * bitangent) *
                          angularTexel);
            visibility += texture(
                pointShadowMap, vec4(sampleDir, float(shadowSlot)),
                depthNorm);
        }
    }
    return visibility / 9.0;
}

float spotShadowVisibility(vec3 positionWS, int shadowSlot) {
    int spotCount = ubo.punctualShadowCounts.y;
    if (spotCount <= 0 || shadowSlot < 0 || shadowSlot >= spotCount)
        return 1.0;

    vec4 clip = ubo.spotShadowViewProj[shadowSlot] *
                vec4(positionWS, 1.0);
    vec3 coord = clip.xyz / clip.w;
    vec2 uv = coord.xy * 0.5 + 0.5;

    if (coord.z <= 0.0 || coord.z >= 1.0 ||
        any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 1.0;

    float texelSize = 1.0 / float(ubo.punctualShadowCounts.w);
    float bias = ubo.shadowParams.y;

    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(x, y) * texelSize;
            visibility += texture(
                spotShadowMap,
                vec4(uv + offset, float(shadowSlot),
                     coord.z - bias));
        }
    }
    return visibility / 9.0;
}

#endif
