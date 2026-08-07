#ifndef VULKAN_LAB_IBL_GLSL
#define VULKAN_LAB_IBL_GLSL

layout(set = 2, binding = 1) uniform samplerCube irradianceMap;
layout(set = 2, binding = 2) uniform samplerCube prefilteredSpecularMap;
layout(set = 2, binding = 3) uniform sampler2D brdfLut;
layout(set = 2, binding = 4) uniform samplerCube radianceMap;

const uint MAX_REFLECTION_PROBES = 8u;

struct GpuReflectionProbe {
    mat4 worldToLocal;
    vec4 capturePositionRadius;
    vec4 boxExtentsBlend;
    vec4 params;
};

layout(set = 2, binding = 5) uniform samplerCube
    reflectionProbeSpecularMaps[MAX_REFLECTION_PROBES];
layout(std430, set = 2, binding = 6) readonly buffer ReflectionProbeBuffer {
    uvec4 counts;
    GpuReflectionProbe probes[];
} reflectionProbeBuffer;

vec3 sampleReflectionProbe(uint index, vec3 direction, float lod)
{
    switch (index) {
    case 0u: return textureLod(reflectionProbeSpecularMaps[0], direction, lod).rgb;
    case 1u: return textureLod(reflectionProbeSpecularMaps[1], direction, lod).rgb;
    case 2u: return textureLod(reflectionProbeSpecularMaps[2], direction, lod).rgb;
    case 3u: return textureLod(reflectionProbeSpecularMaps[3], direction, lod).rgb;
    case 4u: return textureLod(reflectionProbeSpecularMaps[4], direction, lod).rgb;
    case 5u: return textureLod(reflectionProbeSpecularMaps[5], direction, lod).rgb;
    case 6u: return textureLod(reflectionProbeSpecularMaps[6], direction, lod).rgb;
    default: return textureLod(reflectionProbeSpecularMaps[7], direction, lod).rgb;
    }
}

vec3 rotateEnvironmentDirection(vec3 direction)
{
    float angle = -ubo.environmentParams.z;
    float c = cos(angle);
    float s = sin(angle);
    return vec3(c * direction.x - s * direction.y,
                s * direction.x + c * direction.y,
                direction.z);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 f0, float roughness)
{
    return f0 + (max(vec3(1.0 - roughness), f0) - f0) *
                pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 evaluateIblDiffuse(vec3 n, vec3 v, vec3 albedo,
                        float roughness, float metallic)
{
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 irradiance = texture(
        irradianceMap, rotateEnvironmentDirection(n)).rgb;
    vec3 f = fresnelSchlickRoughness(
        max(dot(n, v), 0.0), f0, roughness);
    vec3 kd = (vec3(1.0) - f) * (1.0 - metallic);
    return kd * irradiance * albedo;
}

vec3 evaluateIblSpecular(vec3 n, vec3 v, vec3 albedo,
                         float roughness, float metallic)
{
    float ndv = max(dot(n, v), 0.0);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 f = fresnelSchlickRoughness(ndv, f0, roughness);
    vec3 reflection = reflect(-v, n);
    vec3 prefiltered = textureLod(
        prefilteredSpecularMap,
        rotateEnvironmentDirection(reflection),
        roughness * ubo.environmentParams.w).rgb;
    vec2 brdf = texture(brdfLut, vec2(ndv, roughness)).rg;
    return prefiltered * (f * brdf.x + brdf.y);
}

float reflectionProbeWeight(GpuReflectionProbe probe, vec3 positionWS,
                            out vec3 positionLocal)
{
    positionLocal = (probe.worldToLocal * vec4(positionWS, 1.0)).xyz;
    float blendDistance = max(probe.boxExtentsBlend.w, 0.0);
    float margin;
    if (probe.params.z > 0.5) {
        margin = probe.capturePositionRadius.w - length(positionLocal);
    } else {
        margin = min(min(probe.boxExtentsBlend.x - abs(positionLocal.x),
                         probe.boxExtentsBlend.y - abs(positionLocal.y)),
                     probe.boxExtentsBlend.z - abs(positionLocal.z));
    }
    if (margin < 0.0)
        return 0.0;
    return blendDistance > 1.0e-5
        ? clamp(margin / blendDistance, 0.0, 1.0)
        : 1.0;
}

vec3 reflectionProbeDirection(GpuReflectionProbe probe,
                              vec3 positionLocal,
                              vec3 reflectionWS)
{
    if (probe.params.z > 0.5 || probe.params.w < 0.5)
        return reflectionWS;

    vec3 directionLocal = normalize(
        mat3(probe.worldToLocal) * reflectionWS);
    vec3 extent = max(probe.boxExtentsBlend.xyz, vec3(1.0e-4));
    vec3 safeDirection = vec3(
        abs(directionLocal.x) < 1.0e-5
            ? (directionLocal.x < 0.0 ? -1.0e-5 : 1.0e-5)
            : directionLocal.x,
        abs(directionLocal.y) < 1.0e-5
            ? (directionLocal.y < 0.0 ? -1.0e-5 : 1.0e-5)
            : directionLocal.y,
        abs(directionLocal.z) < 1.0e-5
            ? (directionLocal.z < 0.0 ? -1.0e-5 : 1.0e-5)
            : directionLocal.z);
    vec3 targetPlane = mix(-extent, extent,
                           greaterThanEqual(safeDirection, vec3(0.0)));
    vec3 distances = (targetPlane - positionLocal) / safeDirection;
    float distanceToWall = min(distances.x,
                               min(distances.y, distances.z));
    vec3 hitLocal = positionLocal + directionLocal * distanceToWall;
    vec3 captureLocal =
        (probe.worldToLocal *
         vec4(probe.capturePositionRadius.xyz, 1.0)).xyz;
    return normalize(inverse(mat3(probe.worldToLocal)) *
                     (hitLocal - captureLocal));
}

vec3 evaluateReflectionProbeSpecular(vec3 positionWS, vec3 n, vec3 v,
                                     vec3 albedo, float roughness,
                                     float metallic, vec3 globalSpecular)
{
    if (reflectionProbeBuffer.counts.y == 0u)
        return globalSpecular;
    float ndv = max(dot(n, v), 0.0);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 f = fresnelSchlickRoughness(ndv, f0, roughness);
    vec2 brdf = texture(brdfLut, vec2(ndv, roughness)).rg;
    vec3 reflectionWS = reflect(-v, n);
    vec3 localSpecular = vec3(0.0);
    float remaining = 1.0;
    uint count = min(reflectionProbeBuffer.counts.x,
                     MAX_REFLECTION_PROBES);
    for (uint index = 0u; index < count && remaining > 1.0e-4;
         ++index) {
        GpuReflectionProbe probe = reflectionProbeBuffer.probes[index];
        vec3 positionLocal;
        float weight = reflectionProbeWeight(
            probe, positionWS, positionLocal) * remaining;
        if (weight <= 0.0)
            continue;
        vec3 direction = reflectionProbeDirection(
            probe, positionLocal, reflectionWS);
        vec3 radiance = sampleReflectionProbe(
            index, direction, roughness * max(probe.params.y, 0.0));
        localSpecular += radiance * (f * brdf.x + brdf.y) *
                         max(probe.params.x, 0.0) * weight;
        remaining -= weight;
    }
    return localSpecular + globalSpecular * remaining;
}

struct IndirectLightingComponents {
    vec3 diffuse;
    vec3 specular;
};

IndirectLightingComponents evaluateIndirectLightingComponents(
    vec3 n, vec3 v, vec3 albedo, float roughness, float metallic,
    float occlusion, vec3 positionWS)
{
    IndirectLightingComponents result;
    if (ubo.environmentParams.x < 0.5) {
        result.diffuse = ubo.ambientColorIntensity.rgb *
                         ubo.ambientColorIntensity.a * albedo * occlusion;
        result.specular = evaluateReflectionProbeSpecular(
            positionWS, n, v, albedo, roughness, metallic, vec3(0.0)) *
            occlusion;
        return result;
    }
    float scale = ubo.environmentParams.y;
    result.diffuse = evaluateIblDiffuse(
        n, v, albedo, roughness, metallic) * scale * occlusion;
    vec3 globalSpecular = evaluateIblSpecular(
        n, v, albedo, roughness, metallic) * scale;
    result.specular = evaluateReflectionProbeSpecular(
        positionWS, n, v, albedo, roughness, metallic,
        globalSpecular) * occlusion;
    return result;
}

vec3 evaluateIndirectLighting(vec3 n, vec3 v, vec3 albedo,
                              float roughness, float metallic,
                              float occlusion, vec3 positionWS)
{
    IndirectLightingComponents components =
        evaluateIndirectLightingComponents(
            n, v, albedo, roughness, metallic, occlusion, positionWS);
    return components.diffuse + components.specular;
}

#endif
