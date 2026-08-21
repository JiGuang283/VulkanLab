#ifndef VULKAN_LAB_PBR_BRDF_GLSL
#define VULKAN_LAB_PBR_BRDF_GLSL

const float VKL_PI = 3.14159265359;

float vklDistributionGGX(vec3 normal, vec3 halfVector, float roughness)
{
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float ndh = max(dot(normal, halfVector), 0.0);
    float denominator = ndh * ndh * (alphaSquared - 1.0) + 1.0;
    return alphaSquared /
           max(VKL_PI * denominator * denominator, 0.0001);
}

float vklGeometrySchlickGGX(float ndv, float roughness)
{
    float value = roughness + 1.0;
    float k = value * value / 8.0;
    return ndv / max(ndv * (1.0 - k) + k, 0.0001);
}

float vklGeometrySmith(vec3 normal, vec3 view, vec3 light,
                       float roughness)
{
    return vklGeometrySchlickGGX(max(dot(normal, view), 0.0), roughness) *
           vklGeometrySchlickGGX(max(dot(normal, light), 0.0), roughness);
}

vec3 vklFresnelSchlick(float cosTheta, vec3 f0)
{
    return f0 + (1.0 - f0) *
           pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 vklEvaluatePbrLight(vec3 normal, vec3 view, vec3 light,
                         vec3 radiance, vec3 albedo, float roughness,
                         float metallic)
{
    vec3 halfVector = normalize(view + light);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    float ndl = max(dot(normal, light), 0.0);
    float ndv = max(dot(normal, view), 0.0);
    float distribution = vklDistributionGGX(normal, halfVector, roughness);
    float geometry = vklGeometrySmith(normal, view, light, roughness);
    vec3 fresnel = vklFresnelSchlick(
        max(dot(halfVector, view), 0.0), f0);
    vec3 specular = distribution * geometry * fresnel /
                    max(4.0 * ndv * ndl, 0.0001);
    vec3 diffuseWeight = (vec3(1.0) - fresnel) * (1.0 - metallic);
    return (diffuseWeight * albedo / VKL_PI + specular) * radiance * ndl;
}

#endif
