#ifndef VULKAN_LAB_ATMOSPHERE_SCATTERING_GLSL
#define VULKAN_LAB_ATMOSPHERE_SCATTERING_GLSL

#include "include/atmosphere.glsl"

vec3 atmosphereDensity(vec3 positionKm)
{
    float height = max(length(positionKm - atmospherePlanetCenter()) -
                       atmosphereBottomRadius(), 0.0);
    float rayleigh = exp(-height /
                         max(atmosphere.topRadiusDensityHeights.y, 1e-4));
    float mie = exp(-height /
                    max(atmosphere.topRadiusDensityHeights.z, 1e-4));
    float ozone = max(1.0 - abs(height -
                               atmosphere.topRadiusDensityHeights.w) /
                              max(atmosphere.rayleighScatteringOzoneHalfWidth.w,
                                  1e-4),
                      0.0);
    return vec3(rayleigh, mie, ozone);
}

vec3 atmosphereExtinction(vec3 density)
{
    return atmosphere.rayleighScatteringOzoneHalfWidth.rgb * density.x +
           vec3(atmosphere.mieScatteringExtinction.y * density.y) +
           atmosphere.ozoneAbsorptionAerialStart.rgb * density.z;
}

float rayleighPhase(float cosine)
{
    return 3.0 * (1.0 + cosine * cosine) / (16.0 * ATM_PI);
}

float miePhase(float cosine)
{
    float g = atmosphere.mieScatteringExtinction.z;
    float g2 = g * g;
    float denominator = max(pow(1.0 + g2 - 2.0 * g * cosine, 1.5), 1e-4);
    return 3.0 * (1.0 - g2) * (1.0 + cosine * cosine) /
           (8.0 * ATM_PI * (2.0 + g2) * denominator);
}

vec3 integrateAtmosphere(vec3 originKm, vec3 direction, float distanceKm,
                         int sampleCount, out vec3 transmittance)
{
    direction = normalize(direction);
    float topDistance = raySphereNearest(originKm - atmospherePlanetCenter(),
                                         direction, atmosphereTopRadius());
    if (topDistance < 0.0) {
        transmittance = vec3(1.0);
        return vec3(0.0);
    }
    float groundDistance = raySphereNearest(
        originKm - atmospherePlanetCenter(), direction,
        atmosphereBottomRadius());
    float pathDistance = min(distanceKm, topDistance);
    bool hitsGround = groundDistance >= 0.0 &&
                      groundDistance <= pathDistance;
    if (hitsGround && groundDistance <= 1e-4) {
        vec3 radial = normalize(originKm - atmospherePlanetCenter());
        hitsGround = dot(direction, radial) < 0.0;
    }
    float endDistance = hitsGround ? max(groundDistance, 0.0)
                                   : pathDistance;
    if (endDistance <= 0.0 && !hitsGround) {
        transmittance = vec3(1.0);
        return vec3(0.0);
    }

    vec3 sunDirection = normalize(atmosphere.sunDirectionAngularRadius.xyz);
    float phaseCosine = dot(direction, sunDirection);
    float phaseR = rayleighPhase(phaseCosine);
    float phaseM = miePhase(phaseCosine);
    vec3 radiance = vec3(0.0);
    transmittance = vec3(1.0);
    if (endDistance > 1e-6) {
        float stepLength = endDistance / float(max(sampleCount, 1));
        for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
            float sampleDistance = (float(sampleIndex) + 0.5) * stepLength;
            vec3 positionKm = originKm + direction * sampleDistance;
            vec3 density = atmosphereDensity(positionKm);
            vec3 extinction = atmosphereExtinction(density);
            vec3 segmentTransmittance = exp(-extinction * stepLength);
            vec3 sunTransmittance =
                sampleAtmosphereTransmittance(positionKm, sunDirection);
            vec3 scattering =
                atmosphere.rayleighScatteringOzoneHalfWidth.rgb *
                    density.x * phaseR +
                vec3(atmosphere.mieScatteringExtinction.x * density.y *
                     phaseM);
            vec3 multiple = sampleAtmosphereMultipleScattering(
                positionKm, sunDirection) *
                atmosphere.mieScatteringExtinction.w;
            radiance += transmittance *
                        (sunTransmittance * scattering +
                         multiple * extinction) *
                        stepLength;
            transmittance *= segmentTransmittance;
        }
    }

    if (hitsGround) {
        vec3 groundPosition = originKm + direction * endDistance;
        vec3 groundNormal = normalize(groundPosition -
                                      atmospherePlanetCenter());
        float sunCosine = max(dot(groundNormal, sunDirection), 0.0);
        vec3 directIrradiance = sampleAtmosphereTransmittance(
            groundPosition, sunDirection) * sunCosine;
        vec3 indirectRadiance = sampleAtmosphereMultipleScattering(
            groundPosition, sunDirection) *
            atmosphere.mieScatteringExtinction.w;
        vec3 groundRadiance = atmosphere.groundAlbedoDistanceScale.rgb *
            (directIrradiance / ATM_PI + indirectRadiance);
        radiance += transmittance * groundRadiance;
    }
    return max(radiance, vec3(0.0));
}

#endif
