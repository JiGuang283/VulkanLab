#ifndef VULKAN_LAB_ATMOSPHERE_GLSL
#define VULKAN_LAB_ATMOSPHERE_GLSL

#ifndef ATMOSPHERE_SET
#define ATMOSPHERE_SET 3
#endif

// Parameterization follows Hillaire 2020 and the MIT-licensed reference
// implementation. Distances in this block are expressed in kilometers.
layout(std140, set = ATMOSPHERE_SET, binding = 0) uniform AtmosphereParameters {
    vec4 planetCenterBottomRadius;
    vec4 topRadiusDensityHeights;
    vec4 rayleighScatteringOzoneHalfWidth;
    vec4 mieScatteringExtinction;
    vec4 ozoneAbsorptionAerialStart;
    vec4 groundAlbedoDistanceScale;
    vec4 sunDirectionAngularRadius;
    vec4 sunColorIntensity;
    vec4 cameraDistanceParams;
    vec4 viewportParams;
    uvec4 runtimeParams;
    vec4 reserved;
} atmosphere;

layout(set = ATMOSPHERE_SET, binding = 1) uniform sampler2D atmosphereTransmittance;
layout(set = ATMOSPHERE_SET, binding = 2) uniform sampler2D atmosphereMultipleScattering;
layout(set = ATMOSPHERE_SET, binding = 3) uniform sampler2D atmosphereSkyView;
layout(set = ATMOSPHERE_SET, binding = 4) uniform sampler2DArray atmosphereAerialPerspective;

const float ATM_PI = 3.14159265359;

bool atmosphereIsActive()
{
    return atmosphere.runtimeParams.x != 0u;
}

float atmosphereBottomRadius()
{
    return atmosphere.planetCenterBottomRadius.w;
}

float atmosphereTopRadius()
{
    return atmosphere.topRadiusDensityHeights.x;
}

vec3 atmospherePlanetCenter()
{
    return atmosphere.planetCenterBottomRadius.xyz;
}

float raySphereNearest(vec3 origin, vec3 direction, float radius)
{
    float b = dot(origin, direction);
    float c = dot(origin, origin) - radius * radius;
    float discriminant = b * b - c;
    if (discriminant < 0.0)
        return -1.0;
    float root = sqrt(discriminant);
    float nearDistance = -b - root;
    float farDistance = -b + root;
    return nearDistance >= 0.0 ? nearDistance : farDistance;
}

vec2 atmosphereDirectionUv(vec3 direction)
{
    direction = normalize(direction);
    float azimuth = atan(direction.y, direction.x);
    float elevation = asin(clamp(direction.z, -1.0, 1.0));
    return vec2(fract(azimuth / (2.0 * ATM_PI) + 0.5),
                clamp(elevation / ATM_PI + 0.5, 0.0, 1.0));
}

vec3 atmosphereUvDirection(vec2 uv)
{
    float azimuth = (uv.x - 0.5) * 2.0 * ATM_PI;
    float elevation = (uv.y - 0.5) * ATM_PI;
    float cosElevation = cos(elevation);
    return vec3(cosElevation * cos(azimuth),
                cosElevation * sin(azimuth), sin(elevation));
}

vec2 atmosphereTransmittanceUv(vec3 positionKm, vec3 direction)
{
    vec3 radial = positionKm - atmospherePlanetCenter();
    float radius = max(length(radial), atmosphereBottomRadius());
    float height = clamp((radius - atmosphereBottomRadius()) /
                         max(atmosphereTopRadius() - atmosphereBottomRadius(),
                             1e-4), 0.0, 1.0);
    float mu = dot(radial / max(radius, 1e-4), normalize(direction));
    return vec2(mu * 0.5 + 0.5, height);
}

vec3 sampleAtmosphereTransmittance(vec3 positionKm, vec3 direction)
{
    return texture(atmosphereTransmittance,
                   atmosphereTransmittanceUv(positionKm, direction)).rgb;
}

#ifndef ATMOSPHERE_NO_GLOBAL_FRAME
vec3 atmosphereWorldPositionKm(vec3 positionWS)
{
    return (positionWS - ubo.cameraPosWS.xyz) * 0.001;
}
#endif

vec3 sampleAtmosphereMultipleScattering(vec3 positionKm, vec3 sunDirection)
{
    return texture(atmosphereMultipleScattering,
                   atmosphereTransmittanceUv(positionKm, sunDirection)).rgb;
}

vec4 sampleAerialPerspective(vec3 direction, float distanceMeters)
{
    if (!atmosphereIsActive())
        return vec4(0.0, 0.0, 0.0, 1.0);
    float startKm = atmosphere.ozoneAbsorptionAerialStart.w;
    float distanceKm = max(distanceMeters * 0.001 - startKm, 0.0) *
                       atmosphere.groundAlbedoDistanceScale.w;
    float maxDistanceKm = max(atmosphere.cameraDistanceParams.w - startKm,
                              1e-4);
    if (distanceKm <= 0.0)
        return vec4(0.0, 0.0, 0.0, 1.0);
    float depth = sqrt(clamp(distanceKm / maxDistanceKm, 0.0, 1.0));
    return texture(atmosphereAerialPerspective,
                   vec3(atmosphereDirectionUv(direction), depth));
}

#endif
