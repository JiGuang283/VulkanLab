#version 450

#extension GL_GOOGLE_include_directive : require
#include "include/global_frame.glsl"
#include "include/scene_lights.glsl"
#include "include/material_push.glsl"
#include "include/ibl.glsl"
#include "include/atmosphere.glsl"
#include "include/screen_space_lighting.glsl"

const float PI = 3.14159265359;

layout(location = 0) in vec3 fragPositionWS;
layout(location = 1) in vec3 fragNormalWS;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 4) in vec2 fragTexCoord1;
layout(location = 5) in vec4 fragColor;

layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 1, binding = 2) uniform sampler2D metallicRoughnessTexture;
layout(set = 1, binding = 3) uniform sampler2D occlusionTexture;
layout(set = 1, binding = 4) uniform sampler2D emissiveTexture;
layout(set = 2, binding = 0) uniform sampler2DShadow directionalShadowMap;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outBaselineSpecular;
layout(location = 2) out vec4 outBaselineDiffuse;

bool isMaskAlphaMode()
{
    return abs(push.reserved.x - 1.0) < 0.5;
}

bool isBlendAlphaMode()
{
    return abs(push.reserved.x - 2.0) < 0.5;
}

void applyAlphaCutoff(float alpha)
{
    if (isMaskAlphaMode() && alpha < push.roughnessAlpha.y) discard;
}

float transmissionFactor()
{
    return clamp(push.reserved.y, 0.0, 1.0);
}

float materialAlpha(float baseAlpha)
{
    float transmission = transmissionFactor();
    if (transmission > 0.0 && !isBlendAlphaMode())
        return mix(1.0, 0.28, transmission);
    return baseAlpha;
}

vec3 applyTransmissionApprox(vec3 color, vec3 n, vec3 v, float roughness)
{
    float transmission = transmissionFactor();
    if (transmission <= 0.0)
        return color;

    float ndv = max(dot(n, v), 0.0);
    float rim = pow(clamp(1.0 - ndv, 0.0, 1.0), 4.0);
    float smoothness = 1.0 - clamp(roughness, 0.0, 1.0);
    vec3 rimColor = vec3(0.35 + 0.45 * smoothness);
    vec3 tint = mix(color, vec3(1.0), 0.12 * transmission);
    return tint + rimColor * rim * transmission;
}

vec2 occlusionTexCoord()
{
    return abs(push.roughnessAlpha.w - 1.0) < 0.5 ? fragTexCoord1
                                                  : fragTexCoord;
}

float materialOcclusion()
{
    float sampled = texture(occlusionTexture, occlusionTexCoord()).r;
    float strength = clamp(push.roughnessAlpha.z, 0.0, 1.0);
    return mix(1.0, sampled, strength);
}

float distributionGGX(vec3 n, vec3 h, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float ndh = max(dot(n, h), 0.0);
    float ndh2 = ndh * ndh;
    float denom = (ndh2 * (a2 - 1.0) + 1.0);
    return a2 / max(PI * denom * denom, 0.0001);
}

float geometrySchlickGGX(float ndv, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return ndv / max(ndv * (1.0 - k) + k, 0.0001);
}

float geometrySmith(vec3 n, vec3 v, vec3 l, float roughness)
{
    float ndv = max(dot(n, v), 0.0);
    float ndl = max(dot(n, l), 0.0);
    return geometrySchlickGGX(ndv, roughness) *
           geometrySchlickGGX(ndl, roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0)
{
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 evaluatePbrLight(vec3 n, vec3 v, vec3 l, vec3 radiance, vec3 albedo,
                      float roughness, float metallic)
{
    vec3 h = normalize(v + l);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    float ndl = max(dot(n, l), 0.0);
    float ndv = max(dot(n, v), 0.0);

    float d = distributionGGX(n, h, roughness);
    float g = geometrySmith(n, v, l, roughness);
    vec3 f = fresnelSchlick(max(dot(h, v), 0.0), f0);

    vec3 numerator = d * g * f;
    float denominator = max(4.0 * ndv * ndl, 0.0001);
    vec3 specular = numerator / denominator;

    vec3 ks = f;
    vec3 kd = (vec3(1.0) - ks) * (1.0 - metallic);
    return (kd * albedo / PI + specular) * radiance * ndl;
}

float directionalShadowVisibility(vec3 positionWS)
{
    if (ubo.shadowParams.x < 0.5)
        return 1.0;
    vec4 clip = ubo.directionalShadowViewProj * vec4(positionWS, 1.0);
    vec3 coord = clip.xyz / clip.w;
    vec2 uv = coord.xy * 0.5 + 0.5;
    if (coord.z <= 0.0 || coord.z >= 1.0 ||
        any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 1.0;

    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(x, y) * ubo.shadowParams.z;
            visibility += texture(directionalShadowMap,
                                  vec3(uv + offset,
                                       coord.z - ubo.shadowParams.y));
        }
    }
    return visibility / 9.0;
}

float rangeAttenuation(float distanceToLight, float range)
{
    float attenuation = 1.0 / max(distanceToLight * distanceToLight, 0.01);
    if (range <= 0.0)
        return attenuation;

    float normalizedDistance = clamp(distanceToLight / range, 0.0, 1.0);
    float rangeFade = clamp(1.0 - pow(normalizedDistance, 4.0), 0.0, 1.0);
    return attenuation * rangeFade * rangeFade;
}

float spotAttenuation(vec3 l, vec3 spotDirection, float innerCos,
                      float outerCos)
{
    float cosAngle = dot(normalize(-l), normalize(spotDirection));
    float cone = clamp((cosAngle - outerCos) / max(innerCos - outerCos, 0.001),
                       0.0, 1.0);
    return cone * cone;
}

vec3 evaluateDirectLighting(vec3 n, vec3 v, vec3 positionWS, vec3 albedo,
                            float roughness, float metallic)
{
    vec3 direct = vec3(0.0);
    uint directionalCount = ubo.lightCounts.x;
    uint pointCount = ubo.lightCounts.y;
    uint spotCount = ubo.lightCounts.z;
    int shadowLightIndex = int(ubo.shadowParams.w);

    for (uint i = 0u; i < directionalCount; ++i) {
        GpuLight light = sceneLightBuffer.lights[i];
        vec3 l = normalize(light.directionInnerCos.xyz);
        vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.a;
        if (atmosphereIsActive() &&
            i == atmosphere.runtimeParams.y) {
            radiance *= sampleAtmosphereTransmittance(
                atmosphereWorldPositionKm(positionWS), l);
        }
        float visibility = int(i) == shadowLightIndex
                               ? directionalShadowVisibility(positionWS)
                               : 1.0;
        direct += visibility * evaluatePbrLight(
            n, v, l, radiance, albedo, roughness, metallic);
    }

    uint pointOffset = directionalCount;
    for (uint i = 0u; i < pointCount; ++i) {
        GpuLight light = sceneLightBuffer.lights[pointOffset + i];
        vec3 toLight = light.positionRange.xyz - positionWS;
        float distanceToLight = length(toLight);
        if (distanceToLight <= 0.0001 ||
            (light.positionRange.w > 0.0 &&
             distanceToLight >= light.positionRange.w))
            continue;

        vec3 l = toLight / distanceToLight;
        float attenuation = rangeAttenuation(distanceToLight,
                                             light.positionRange.w);
        vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.a *
                        attenuation;
        direct += evaluatePbrLight(n, v, l, radiance, albedo, roughness,
                                   metallic);
    }

    uint spotOffset = pointOffset + pointCount;
    for (uint i = 0u; i < spotCount; ++i) {
        GpuLight light = sceneLightBuffer.lights[spotOffset + i];
        vec3 toLight = light.positionRange.xyz - positionWS;
        float distanceToLight = length(toLight);
        if (distanceToLight <= 0.0001 ||
            (light.positionRange.w > 0.0 &&
             distanceToLight >= light.positionRange.w))
            continue;

        vec3 l = toLight / distanceToLight;
        float attenuation = rangeAttenuation(distanceToLight,
                                             light.positionRange.w);
        attenuation *= spotAttenuation(l, light.directionInnerCos.xyz,
                                       light.directionInnerCos.w,
                                       light.params.y);
        vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.a *
                        attenuation;
        direct += evaluatePbrLight(n, v, l, radiance, albedo, roughness,
                                   metallic);
    }

    return direct;
}

void main()
{
    vec4 baseSample = texture(baseColorTexture, fragTexCoord);
    vec4 baseColor = baseSample * push.baseColorFactor * fragColor;
    applyAlphaCutoff(baseColor.a);

    vec4 mr = texture(metallicRoughnessTexture, fragTexCoord);
    float roughness = clamp(mr.g * push.roughnessAlpha.x, 0.04, 1.0);
    float metallic = clamp(mr.b * push.emissiveMetallic.w, 0.0, 1.0);
    float materialAo = materialOcclusion();
    float screenAo =
        (!isBlendAlphaMode() && transmissionFactor() <= 0.0)
            ? screenSpaceAmbientOcclusion()
            : 1.0;
    float occlusion = materialAo * screenAo;
    vec3 emissive = texture(emissiveTexture, fragTexCoord).rgb *
                    push.emissiveMetallic.rgb;

    vec3 n = normalize(fragNormalWS);
    if (!gl_FrontFacing)
        n = -n;
    vec3 v = normalize(ubo.cameraPosWS.xyz - fragPositionWS);

    vec3 albedo = baseColor.rgb;
    vec3 direct = evaluateDirectLighting(n, v, fragPositionWS, albedo,
                                         roughness, metallic);

    IndirectLightingComponents indirect =
        evaluateIndirectLightingComponents(
            n, v, albedo, roughness, metallic, occlusion);
    vec3 baselineSpecular = indirect.specular;
    vec3 baselineDiffuse = indirect.diffuse;
    vec3 color = applyTransmissionApprox(
        indirect.diffuse + indirect.specular + direct + emissive, n, v,
                                         roughness);
    if (atmosphereIsActive()) {
        vec3 viewRay = fragPositionWS - ubo.cameraPosWS.xyz;
        vec4 aerial = sampleAerialPerspective(
            normalize(viewRay), length(viewRay));
        color = color * aerial.a + aerial.rgb *
                atmosphere.sunColorIntensity.rgb *
                atmosphere.sunColorIntensity.a;
        baselineSpecular *= aerial.a;
        baselineDiffuse *= aerial.a;
    }
    outColor = vec4(color, materialAlpha(baseColor.a));
    outBaselineSpecular = vec4(baselineSpecular, 1.0);
    outBaselineDiffuse = vec4(baselineDiffuse, materialAo);
}
