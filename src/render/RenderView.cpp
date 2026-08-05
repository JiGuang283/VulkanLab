#include "render/RenderView.h"
#include "diagnostics/Profiling.h"

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

SceneLight makeDefaultSun(const DefaultSunSettings &settings) {
    SceneLight light{};
    light.debugName = "Fallback Sun";
    light.stableKey = "fallback/sun";
    light.source = SceneLightSource::Fallback;
    light.castsShadow = true;
    light.type = LightType::Directional;
    light.directionWS = normalizeOrFallback(
        settings.direction, glm::vec3(0.3f, 0.8f, 0.5f));
    light.color = glm::max(settings.color, glm::vec3(0.0f));
    light.intensity = std::max(settings.intensity, 0.0f);
    return light;
}

GpuLight makeGpuLight(const SceneLight &light) {
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
                  outerConeCos, 0.0f, 0.0f);
    return gpu;
}

uint32_t shadowPriority(SceneLightSource source) {
    switch (source) {
    case SceneLightSource::ExplicitEntity:
        return 0;
    case SceneLightSource::ImportedModel:
        return 1;
    case SceneLightSource::Legacy:
        return 2;
    case SceneLightSource::Fallback:
        return 3;
    }
    return std::numeric_limits<uint32_t>::max();
}

std::string stableLightKey(const SceneLight &light, size_t worldIndex) {
    if (!light.stableKey.empty())
        return light.stableKey;
    if (light.ownerEntity)
        return "entity/" + light.ownerEntity->toString();
    if (!light.debugName.empty())
        return "legacy/" + light.debugName;
    return "legacy/" + std::to_string(worldIndex);
}

bool isShadowCandidate(const SceneLight &light) {
    if (light.type != LightType::Directional || !light.castsShadow)
        return false;
    return std::isfinite(light.directionWS.x) &&
           std::isfinite(light.directionWS.y) &&
           std::isfinite(light.directionWS.z) &&
           glm::dot(light.directionWS, light.directionWS) > 1.0e-8f;
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

bool isEffectiveSceneLight(const SceneLight &light) {
    constexpr float kMinimumContribution = 1.0e-5f;
    if (!std::isfinite(light.intensity) ||
        light.intensity <= kMinimumContribution) {
        return false;
    }
    return std::isfinite(light.color.r) && std::isfinite(light.color.g) &&
           std::isfinite(light.color.b) &&
           (light.color.r > kMinimumContribution ||
            light.color.g > kMinimumContribution ||
            light.color.b > kMinimumContribution);
}

RenderView buildRenderView(const RenderViewInput &input) {
    VKL_PROFILE_ZONE("Build RenderView");
    RenderView result{};
    result.settings = input.settings;
    result.cameraNearPlane = input.cameraNearPlane;
    result.cameraFarPlane = input.cameraFarPlane;
    result.globalUbo.view = input.view;
    result.globalUbo.proj = input.projection;
    result.globalUbo.inverseViewProjection =
        glm::inverse(input.projection * input.view);
    result.globalUbo.cameraPosWS = glm::vec4(input.cameraPosition, 1.0f);
    result.globalUbo.ambientColorIntensity =
        glm::vec4(glm::max(input.ambientColor, glm::vec3(0.0f)),
                  std::max(input.ambientIntensity, 0.0f));

    SceneLight fallbackSun{};
    std::vector<const SceneLight *> effectiveLights;
    if (input.sceneLights) {
        for (const SceneLight &light : *input.sceneLights) {
            if (!isEffectiveSceneLight(light))
                continue;
            effectiveLights.push_back(&light);
        }
    }
    if (effectiveLights.empty() && input.fallbackSunEnabled) {
        fallbackSun = makeDefaultSun(input.defaultSun);
        effectiveLights.push_back(&fallbackSun);
    }
    result.lightStats.effectiveLights =
        static_cast<uint32_t>(effectiveLights.size());

    const SceneLight *shadowLight = nullptr;
    size_t shadowWorldIndex = 0;
    for (size_t index = 0; index < effectiveLights.size(); ++index) {
        const SceneLight &candidate = *effectiveLights[index];
        if (!isShadowCandidate(candidate))
            continue;
        if (!shadowLight ||
            std::tuple{shadowPriority(candidate.source),
                       stableLightKey(candidate, index), index} <
                std::tuple{shadowPriority(shadowLight->source),
                           stableLightKey(*shadowLight, shadowWorldIndex),
                           shadowWorldIndex}) {
            shadowLight = &candidate;
            shadowWorldIndex = index;
        }
    }

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
                std::tuple{stableLightKey(candidate, index), index} <
                    std::tuple{stableLightKey(*atmosphereSun,
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
    if (shadowLight)
        acceptedLights.push_back(shadowLight);
    if (atmosphereSun && atmosphereSun != shadowLight)
        acceptedLights.push_back(atmosphereSun);
    for (const SceneLight *light : effectiveLights) {
        if (light == shadowLight || light == atmosphereSun)
            continue;
        if (acceptedLights.size() < kMaxSceneLights) {
            acceptedLights.push_back(light);
            continue;
        }
        ++result.lightStats.ignoredLights;
        if (light->ownerEntity)
            result.lightStats.ignoredEntityIds.push_back(*light->ownerEntity);
        result.lightStats.ignoredStableKeys.push_back(
            stableLightKey(*light, result.lightStats.ignoredLights - 1));
    }

    const auto appendType = [&](LightType type) {
        for (const SceneLight *light : acceptedLights) {
            if (light->type != type)
                continue;
            const int32_t bufferIndex =
                static_cast<int32_t>(result.sceneLights.size());
            result.sceneLights.push_back(makeGpuLight(*light));
            if (light == shadowLight)
                result.lightStats.shadowCasterBufferIndex = bufferIndex;
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

    if (shadowLight) {
        result.lightStats.shadowCasterEntity = shadowLight->ownerEntity;
        result.lightStats.shadowCasterKey =
            stableLightKey(*shadowLight, shadowWorldIndex);
        result.lightStats.shadowCasterName = shadowLight->debugName;
    }

    result.globalUbo.lightCounts =
        glm::uvec4(result.lightStats.directionalLights,
                   result.lightStats.pointLights,
                   result.lightStats.spotLights,
                   result.lightStats.totalLights);
    const DirectionalShadowCameraData shadowCamera{
        input.view, input.projection, input.cameraNearPlane,
        input.cameraFarPlane, input.settings.culling.shadowDistance};
    result.directionalShadow = buildDirectionalShadowFrameData(
        input.sceneBounds, shadowLight, input.settings.shadowsEnabled,
        &shadowCamera);
    result.lightStats.shadowCasterActive = result.directionalShadow.enabled;
    result.globalUbo.directionalShadowViewProj =
        result.directionalShadow.lightViewProjection;
    result.globalUbo.shadowParams =
        glm::vec4(result.directionalShadow.enabled ? 1.0f : 0.0f,
                   input.settings.shadowReceiverBias,
                   result.directionalShadow.texelSize,
                   result.directionalShadow.enabled
                       ? static_cast<float>(
                             result.lightStats.shadowCasterBufferIndex)
                       : -1.0f);
    result.globalUbo.environmentParams =
        glm::vec4(input.settings.iblEnabled && input.environmentReady
                      ? 1.0f
                      : 0.0f,
                  std::max(input.settings.environmentIntensity, 0.0f),
                   input.settings.environmentRotationRadians,
                   std::max(input.maxSpecularLod, 0.0f));

    if (input.atmosphere) {
        const RenderWorldAtmosphere &worldAtmosphere = *input.atmosphere;
        const AtmosphereComponentDocument &a = worldAtmosphere.parameters;
        result.atmosphere.componentPresent = true;
        result.atmosphere.componentEntity = worldAtmosphere.entityId;
        result.atmosphere.staticLutKey = atmosphereStaticLutKey(a);
        if (atmosphereSun) {
            result.atmosphere.sunEntity = atmosphereSun->ownerEntity;
            result.atmosphere.sunStableKey = stableLightKey(
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
