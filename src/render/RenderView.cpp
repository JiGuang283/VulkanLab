#include "render/RenderView.h"
#include "diagnostics/Profiling.h"

#include <algorithm>
#include <cmath>
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

    std::vector<const SceneLight *> acceptedLights;
    acceptedLights.reserve(
        std::min(effectiveLights.size(), size_t{kMaxSceneLights}));
    if (shadowLight)
        acceptedLights.push_back(shadowLight);
    for (const SceneLight *light : effectiveLights) {
        if (light == shadowLight)
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
            result.sceneLights.push_back(makeGpuLight(*light));
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
        result.lightStats.shadowCasterBufferIndex = 0;
    }

    result.globalUbo.lightCounts =
        glm::uvec4(result.lightStats.directionalLights,
                   result.lightStats.pointLights,
                   result.lightStats.spotLights,
                   result.lightStats.totalLights);
    result.directionalShadow = buildDirectionalShadowFrameData(
        input.sceneBounds, shadowLight, input.settings.shadowsEnabled);
    result.lightStats.shadowCasterActive = result.directionalShadow.enabled;
    result.globalUbo.directionalShadowViewProj =
        result.directionalShadow.lightViewProjection;
    result.globalUbo.shadowParams =
        glm::vec4(result.directionalShadow.enabled ? 1.0f : 0.0f,
                  input.settings.shadowReceiverBias,
                  result.directionalShadow.texelSize,
                  result.directionalShadow.enabled ? 0.0f : -1.0f);
    result.globalUbo.environmentParams =
        glm::vec4(input.settings.iblEnabled && input.environmentReady
                      ? 1.0f
                      : 0.0f,
                  std::max(input.settings.environmentIntensity, 0.0f),
                  input.settings.environmentRotationRadians,
                  std::max(input.maxSpecularLod, 0.0f));
    return result;
}

} // namespace vkr
