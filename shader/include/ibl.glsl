#ifndef VULKAN_LAB_IBL_GLSL
#define VULKAN_LAB_IBL_GLSL

layout(set = 2, binding = 1) uniform samplerCube irradianceMap;
layout(set = 2, binding = 2) uniform samplerCube prefilteredSpecularMap;
layout(set = 2, binding = 3) uniform sampler2D brdfLut;
layout(set = 2, binding = 4) uniform samplerCube radianceMap;

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

struct IndirectLightingComponents {
    vec3 diffuse;
    vec3 specular;
};

IndirectLightingComponents evaluateIndirectLightingComponents(
    vec3 n, vec3 v, vec3 albedo, float roughness, float metallic,
    float occlusion)
{
    IndirectLightingComponents result;
    if (ubo.environmentParams.x < 0.5) {
        result.diffuse = ubo.ambientColorIntensity.rgb *
                         ubo.ambientColorIntensity.a * albedo * occlusion;
        result.specular = vec3(0.0);
        return result;
    }
    float scale = ubo.environmentParams.y * occlusion;
    result.diffuse = evaluateIblDiffuse(
        n, v, albedo, roughness, metallic) * scale;
    result.specular = evaluateIblSpecular(
        n, v, albedo, roughness, metallic) * scale;
    return result;
}

vec3 evaluateIndirectLighting(vec3 n, vec3 v, vec3 albedo,
                              float roughness, float metallic,
                              float occlusion)
{
    IndirectLightingComponents components =
        evaluateIndirectLightingComponents(
            n, v, albedo, roughness, metallic, occlusion);
    return components.diffuse + components.specular;
}

#endif
