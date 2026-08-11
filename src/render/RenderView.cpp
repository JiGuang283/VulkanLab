#include "render/RenderView.h"
#include "diagnostics/Profiling.h"
#include "render/TemporalAA.h"
#include "render/EnvironmentGpuResources.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>
#include <utility>

namespace vkr {

namespace {

glm::vec3 normalizeOrFallback(const glm::vec3 &value,
                              const glm::vec3 &fallback) {
    if (glm::dot(value, value) <= 1.0e-6f)
        return glm::normalize(fallback);
    return glm::normalize(value);
}

GpuLight makeGpuLight(const SceneLight &light, int32_t shadowSlot = -1,
                      float shadowFar = 0.0f) {
    GpuLight gpu{};
    const glm::vec3 fallbackDirection =
        light.type == LightType::Directional
            ? glm::vec3(0.3f, 0.8f, 0.5f)
            : glm::vec3(0.0f, -1.0f, 0.0f);
    const glm::vec3 direction =
        normalizeOrFallback(light.directionWS, fallbackDirection);

    float innerConeCos = glm::clamp(light.innerConeCos, -1.0f, 1.0f);
    float outerConeCos = glm::clamp(light.outerConeCos, -1.0f, 1.0f);
    if (light.type == LightType::Spot && innerConeCos < outerConeCos)
        std::swap(innerConeCos, outerConeCos);

    gpu.positionRange =
        glm::vec4(light.positionWS, std::max(light.range, 0.0f));
    gpu.directionInnerCos = glm::vec4(direction, innerConeCos);
    gpu.colorIntensity =
        glm::vec4(glm::max(light.color, glm::vec3(0.0f)),
                  std::max(light.intensity, 0.0f));
    gpu.params =
        glm::vec4(static_cast<float>(static_cast<uint32_t>(light.type)),
                  outerConeCos, static_cast<float>(shadowSlot),
                  std::max(shadowFar, 0.0f));
    return gpu;
}

uint64_t atmosphereStaticLutKey(
    const AtmosphereComponentDocument &atmosphere) {
    uint64_t hash = 1469598103934665603ull;
    const auto append = [&hash](float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        hash ^= bits;
        hash *= 1099511628211ull;
    };
    append(atmosphere.bottomRadiusKm);
    append(atmosphere.atmosphereHeightKm);
    append(atmosphere.rayleighScatteringPerKm.x);
    append(atmosphere.rayleighScatteringPerKm.y);
    append(atmosphere.rayleighScatteringPerKm.z);
    append(atmosphere.rayleighScaleHeightKm);
    append(atmosphere.mieScatteringPerKm);
    append(atmosphere.mieExtinctionPerKm);
    append(atmosphere.mieScaleHeightKm);
    append(atmosphere.mieAnisotropy);
    append(atmosphere.ozoneAbsorptionPerKm.x);
    append(atmosphere.ozoneAbsorptionPerKm.y);
    append(atmosphere.ozoneAbsorptionPerKm.z);
    append(atmosphere.ozoneCenterHeightKm);
    append(atmosphere.ozoneHalfWidthKm);
    append(atmosphere.groundAlbedo.x);
    append(atmosphere.groundAlbedo.y);
    append(atmosphere.groundAlbedo.z);
    append(atmosphere.multipleScatteringFactor);
    return hash;
}

} // namespace

RenderView buildRenderView(const RenderViewInput &input,
                           const ShadowFramePlan &shadowPlan) {
    VKL_PROFILE_ZONE("Build RenderView");
    RenderView result{};
    result.shadow = shadowPlan;
    result.settings = input.settings;
    result.settings.maxPointShadowLights = std::min(
        result.settings.maxPointShadowLights, kMaxPointShadowLights);
    result.settings.maxSpotShadowLights = std::min(
        result.settings.maxSpotShadowLights, kMaxSpotShadowLights);
    result.settings.pointShadowDistance = glm::clamp(
        result.settings.pointShadowDistance, kMinPunctualShadowDistance,
        kMaxPunctualShadowDistance);
    result.settings.spotShadowDistance = glm::clamp(
        result.settings.spotShadowDistance, kMinPunctualShadowDistance,
        kMaxPunctualShadowDistance);
    result.settings.culling.shadowDistance = glm::clamp(
        result.settings.culling.shadowDistance,
        kMinDirectionalShadowDistance, kMaxDirectionalShadowDistance);
    result.cameraNearPlane = input.cameraNearPlane;
    result.cameraFarPlane = input.cameraFarPlane;
    result.stableProjection = input.projection;
    result.stableViewProjection = input.projection * input.view;
    result.projectionJitterNdc = input.projectionJitterNdc;
    result.projectionJitterPixels = input.projectionJitterPixels;
    result.globalUbo.view = input.view;
    result.globalUbo.proj = applyProjectionJitter(
        input.projection, input.projectionJitterNdc);
    result.globalUbo.inverseViewProjection =
        glm::inverse(result.globalUbo.proj * input.view);
    result.globalUbo.cameraPosWS = glm::vec4(input.cameraPosition, 1.0f);
    result.globalUbo.ambientColorIntensity =
        glm::vec4(glm::max(input.ambientColor, glm::vec3(0.0f)),
                  std::max(input.ambientIntensity, 0.0f));

    if (input.ddgiProbeVolume) {
        const RenderWorldDdgiVolume &source = *input.ddgiProbeVolume;
        result.ddgi.componentPresent = true;
        result.ddgi.componentEntity = source.entityId;
        result.ddgi.localToWorld = source.localToWorld;
        result.ddgi.worldToLocal = source.worldToLocal;
        result.ddgi.parameters = source.parameters;
        result.ddgi.probeCount = source.parameters.probeCounts.x *
                                 source.parameters.probeCounts.y *
                                 source.parameters.probeCounts.z;
        const GlobalIlluminationMode mode =
            result.settings.globalIlluminationMode;
        result.ddgi.active = input.ddgiSupported &&
                             (mode == GlobalIlluminationMode::Ddgi ||
                              mode == GlobalIlluminationMode::SsgiDdgi);
    }

    if (input.reflectionProbes) {
        std::vector<const RenderWorldReflectionProbe *> candidates;
        candidates.reserve(input.reflectionProbes->size());
        for (const RenderWorldReflectionProbe &probe :
             *input.reflectionProbes) {
            ++result.reflectionProbeStats.sourceCount;
            if (!probe.environment ||
                !probe.environment->prefilteredSpecular)
                continue;
            candidates.push_back(&probe);
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto *left, const auto *right) {
                      if (left->parameters.priority !=
                          right->parameters.priority) {
                          return left->parameters.priority >
                                 right->parameters.priority;
                      }
                      return left->entityId.toString() <
                             right->entityId.toString();
                  });
        result.reflectionProbes.reserve(
            std::min(candidates.size(),
                     size_t{kMaxReflectionProbes}));
        for (size_t index = 0; index < candidates.size(); ++index) {
            const RenderWorldReflectionProbe &source = *candidates[index];
            if (result.reflectionProbes.size() >=
                kMaxReflectionProbes) {
                ++result.reflectionProbeStats.ignoredCount;
                result.reflectionProbeStats.ignoredEntityIds.push_back(
                    source.entityId);
                continue;
            }
            RenderViewReflectionProbe probe;
            probe.entityId = source.entityId;
            probe.environment = source.environment;
            probe.environmentGeneration =
                source.environmentGeneration;
            probe.gpu.worldToLocal = source.worldToLocal;
            probe.gpu.capturePositionRadius = glm::vec4(
                source.capturePositionWS,
                source.parameters.sphereRadius);
            probe.gpu.boxExtentsBlend = glm::vec4(
                source.parameters.boxExtents,
                source.parameters.blendDistance);
            probe.gpu.params = glm::vec4(
                source.parameters.intensity,
                std::max(source.environment->maxSpecularLod, 0.0f),
                source.parameters.shape == ReflectionProbeShape::Sphere
                    ? 1.0f
                    : 0.0f,
                source.parameters.boxProjection ? 1.0f : 0.0f);
            result.reflectionProbes.push_back(std::move(probe));
        }
        result.reflectionProbeStats.activeCount =
            static_cast<uint32_t>(result.reflectionProbes.size());
    }

    std::vector<const SceneLight *> effectiveLights;
    effectiveLights.reserve(result.shadow.effectiveLights.size());
    for (const SceneLight &light : result.shadow.effectiveLights)
        effectiveLights.push_back(&light);
    result.lightStats.effectiveLights =
        static_cast<uint32_t>(effectiveLights.size());
    result.lightStats.shadowContentRevision =
        result.shadow.contentRevision;
    result.lightStats.shadowReactiveFramesRemaining =
        result.shadow.statistics.reactiveFramesRemaining;
    result.lightStats.shadowTemporalReactive =
        result.shadow.temporalReactive;
    result.lightStats.shadowEvictions = result.shadow.statistics.evictions;
    for (const ShadowAllocation &allocation : result.shadow.allocations) {
        PunctualShadowSelection selection{
            allocation.slot, allocation.entity, allocation.stableKey,
            allocation.name, allocation.farPlane, allocation.policy,
            allocation.score, allocation.age, allocation.retained,
            allocation.focused};
        if (allocation.type == LightType::Point)
            result.lightStats.pointShadowSelections.push_back(selection);
        else if (allocation.type == LightType::Spot)
            result.lightStats.spotShadowSelections.push_back(selection);
    }
    result.lightStats.pointShadowLights = static_cast<uint32_t>(
        result.lightStats.pointShadowSelections.size());
    result.lightStats.spotShadowLights = static_cast<uint32_t>(
        result.lightStats.spotShadowSelections.size());

    const SceneLight *atmosphereSun = nullptr;
    size_t atmosphereSunWorldIndex = 0;
    if (input.atmosphere) {
        for (size_t index = 0; index < effectiveLights.size(); ++index) {
            const SceneLight &candidate = *effectiveLights[index];
            if (candidate.type != LightType::Directional ||
                candidate.atmosphereSunIndex != 0u) {
                continue;
            }
            if (!atmosphereSun ||
                std::tuple{sceneLightStableKey(candidate, index), index} <
                    std::tuple{sceneLightStableKey(*atmosphereSun,
                                                   atmosphereSunWorldIndex),
                               atmosphereSunWorldIndex}) {
                atmosphereSun = &candidate;
                atmosphereSunWorldIndex = index;
            }
        }
    }

    std::vector<const SceneLight *> acceptedLights;
    acceptedLights.reserve(
        std::min(effectiveLights.size(), size_t{kMaxSceneLights}));
    const auto keyFor = [&result](const SceneLight *light) {
        const size_t index = static_cast<size_t>(
            light - result.shadow.effectiveLights.data());
        return sceneLightStableKey(*light, index);
    };
    const auto findLightByKey = [&effectiveLights, &keyFor](
                                    const std::string &key) {
        const auto found = std::find_if(
            effectiveLights.begin(), effectiveLights.end(),
            [&key, &keyFor](const SceneLight *light) {
                return keyFor(light) == key;
            });
        return found != effectiveLights.end() ? *found : nullptr;
    };
    const auto appendUnique = [&acceptedLights](const SceneLight *light) {
        if (light && std::find(acceptedLights.begin(),
                               acceptedLights.end(), light) ==
                         acceptedLights.end()) {
            acceptedLights.push_back(light);
        }
    };

    // Preserve every shadow-bound light before applying the scene light limit.
    for (const ShadowAllocation &allocation : result.shadow.allocations)
        appendUnique(findLightByKey(allocation.stableKey));
    appendUnique(atmosphereSun);
    for (const SceneLight *light : effectiveLights) {
        if (std::find(acceptedLights.begin(), acceptedLights.end(), light) !=
            acceptedLights.end())
            continue;
        if (acceptedLights.size() < kMaxSceneLights) {
            acceptedLights.push_back(light);
            continue;
        }
        ++result.lightStats.ignoredLights;
        if (light->ownerEntity)
            result.lightStats.ignoredEntityIds.push_back(
                *light->ownerEntity);
        result.lightStats.ignoredStableKeys.push_back(
            keyFor(light));
    }

    const auto appendType = [&](LightType type) {
        for (const SceneLight *light : acceptedLights) {
            if (light->type != type)
                continue;
            const int32_t bufferIndex =
                static_cast<int32_t>(result.sceneLights.size());
            int32_t shadowSlot = -1;
            float shadowFar = 0.0f;
            const std::string key = keyFor(light);
            const auto binding = result.shadow.lightBindings.find(key);
            if (binding != result.shadow.lightBindings.end()) {
                shadowSlot = binding->second.slot;
                shadowFar = binding->second.farPlane;
            }
            result.sceneLights.push_back(
                makeGpuLight(*light, shadowSlot, shadowFar));
            if (key == result.shadow.directionalStableKey)
                result.lightStats.shadowCasterBufferIndex =
                    bufferIndex;
            if (light == atmosphereSun)
                result.atmosphere.sunBufferIndex = bufferIndex;
            switch (type) {
            case LightType::Directional:
                ++result.lightStats.directionalLights;
                break;
            case LightType::Point:
                ++result.lightStats.pointLights;
                break;
            case LightType::Spot:
                ++result.lightStats.spotLights;
                break;
            }
        }
    };
    result.sceneLights.reserve(acceptedLights.size());
    appendType(LightType::Directional);
    appendType(LightType::Point);
    appendType(LightType::Spot);
    result.lightStats.punctualLights = result.lightStats.pointLights +
                                      result.lightStats.spotLights;
    result.lightStats.totalLights =
        static_cast<uint32_t>(result.sceneLights.size());

    result.lightStats.shadowCasterEntity =
        result.shadow.directionalEntity;
    result.lightStats.shadowCasterKey =
        result.shadow.directionalStableKey;
    result.lightStats.shadowCasterName = result.shadow.directionalName;

    result.globalUbo.lightCounts =
        glm::uvec4(result.lightStats.directionalLights,
                   result.lightStats.pointLights,
                   result.lightStats.spotLights,
                   result.lightStats.totalLights);
    result.lightStats.shadowCasterActive = result.shadow.csm.enabled;

    // Fill cascade view-projection matrices into UBO
    for (uint32_t c = 0; c < kCsmCascadeCount; ++c) {
        result.globalUbo.cascadeViewProj[c] =
            result.shadow.csm.cascades[c].lightViewProjection;
    }
    // Pack cascade split depths (view-space Z) into vec4
    result.globalUbo.cascadeSplits =
        glm::vec4(result.shadow.csm.splitDepths[0],
                  result.shadow.csm.splitDepths[1],
                  result.shadow.csm.splitDepths[2],
                  result.shadow.csm.splitDepths[3]);
    result.globalUbo.shadowParams =
        glm::vec4(result.shadow.csm.enabled ? 1.0f : 0.0f,
                   result.settings.shadowReceiverBias,
                   result.shadow.csm.cascades[0].texelSize,
                   result.shadow.csm.enabled
                       ? static_cast<float>(
                             result.lightStats.shadowCasterBufferIndex)
                       : -1.0f);

    result.globalUbo.punctualShadowCounts =
        glm::ivec4(result.shadow.punctual.activePointCount,
                   result.shadow.punctual.activeSpotCount,
                   static_cast<int>(kPointShadowMapSize),
                   static_cast<int>(kSpotShadowMapSize));
    result.globalUbo.punctualShadowParams =
        glm::vec4(result.settings.pointShadowReceiverBiasWorld,
                  0.0f, 0.0f, 0.0f);
    for (uint32_t s = 0;
         s < result.shadow.punctual.activeSpotCount; ++s)
        result.globalUbo.spotShadowViewProj[s] =
            result.shadow.punctual.spots[s].viewProjection;
    result.globalUbo.environmentParams =
        glm::vec4(result.settings.iblEnabled && input.environmentReady
                      ? 1.0f
                      : 0.0f,
                  std::max(result.settings.environmentIntensity, 0.0f),
                   result.settings.environmentRotationRadians,
                   std::max(input.maxSpecularLod, 0.0f));

    if (input.atmosphere) {
        const RenderWorldAtmosphere &worldAtmosphere = *input.atmosphere;
        const AtmosphereComponentDocument &a = worldAtmosphere.parameters;
        result.atmosphere.componentPresent = true;
        result.atmosphere.componentEntity = worldAtmosphere.entityId;
        result.atmosphere.staticLutKey = atmosphereStaticLutKey(a);
        if (atmosphereSun) {
            result.atmosphere.sunEntity = atmosphereSun->ownerEntity;
            result.atmosphere.sunStableKey = sceneLightStableKey(
                *atmosphereSun, atmosphereSunWorldIndex);
        }

        AtmosphereGpuParams &gpu = result.atmosphereGpuParams;
        const glm::vec3 groundRelativeKm =
            (worldAtmosphere.groundOriginWS - input.cameraPosition) * 0.001f;
        const glm::vec3 planetCenterRelativeKm =
            groundRelativeKm - glm::vec3(0.0f, 0.0f, a.bottomRadiusKm);
        const float topRadiusKm =
            a.bottomRadiusKm + a.atmosphereHeightKm;
        const float cameraRadiusKm = glm::length(planetCenterRelativeKm);
        result.atmosphere.cameraAltitudeKm =
            std::max(cameraRadiusKm - a.bottomRadiusKm, 0.0f);
        gpu.planetCenterBottomRadius =
            glm::vec4(planetCenterRelativeKm, a.bottomRadiusKm);
        gpu.topRadiusDensityHeights =
            glm::vec4(topRadiusKm, a.rayleighScaleHeightKm,
                      a.mieScaleHeightKm, a.ozoneCenterHeightKm);
        gpu.rayleighScatteringOzoneHalfWidth =
            glm::vec4(a.rayleighScatteringPerKm, a.ozoneHalfWidthKm);
        gpu.mieScatteringExtinction =
            glm::vec4(a.mieScatteringPerKm, a.mieExtinctionPerKm,
                      a.mieAnisotropy, a.multipleScatteringFactor);
        gpu.ozoneAbsorptionAerialStart =
            glm::vec4(a.ozoneAbsorptionPerKm,
                      a.aerialPerspectiveStartMeters * 0.001f);
        gpu.groundAlbedoDistanceScale =
            glm::vec4(a.groundAlbedo, a.aerialPerspectiveDistanceScale);
        if (atmosphereSun) {
            gpu.sunDirectionAngularRadius =
                glm::vec4(normalizeOrFallback(
                              atmosphereSun->directionWS,
                              glm::vec3(0.3f, 0.8f, 0.5f)),
                          atmosphereSun->sourceAngularRadiusRadians);
            gpu.sunColorIntensity =
                glm::vec4(glm::max(atmosphereSun->color, glm::vec3(0.0f)),
                          std::max(atmosphereSun->intensity, 0.0f));
        }
        const float nearKm = std::max(input.cameraNearPlane, 0.001f) * 0.001f;
        const float farKm = std::max(input.cameraFarPlane,
                                     input.cameraNearPlane + 0.001f) *
                            0.001f;
        gpu.cameraDistanceParams =
            glm::vec4(result.atmosphere.cameraAltitudeKm, nearKm, farKm,
                      std::min(farKm, 96.0f));
        const float width = static_cast<float>(input.viewportExtent.width);
        const float height = static_cast<float>(input.viewportExtent.height);
        gpu.viewportParams =
            glm::vec4(width, height, width > 0.0f ? 1.0f / width : 0.0f,
                      height > 0.0f ? 1.0f / height : 0.0f);
        result.atmosphere.active = input.atmosphereSupported &&
                                   atmosphereSun != nullptr &&
                                   result.atmosphere.sunBufferIndex >= 0;
        gpu.runtimeParams =
            glm::uvec4(result.atmosphere.active ? 1u : 0u,
                       result.atmosphere.sunBufferIndex >= 0
                           ? static_cast<uint32_t>(
                                 result.atmosphere.sunBufferIndex)
                           : std::numeric_limits<uint32_t>::max(),
                       0u, 0u);
    }
    return result;
}

} // namespace vkr
