#include "render/RenderView.h"

#include <algorithm>
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

} // namespace

RenderView buildRenderView(const RenderViewInput &input) {
    RenderView result{};
    result.settings = input.settings;
    result.globalUbo.view = input.view;
    result.globalUbo.proj = input.projection;
    result.globalUbo.cameraPosWS = glm::vec4(input.cameraPosition, 1.0f);
    result.globalUbo.ambientColorIntensity =
        glm::vec4(glm::max(input.ambientColor, glm::vec3(0.0f)),
                  std::max(input.ambientIntensity, 0.0f));

    SceneLight fallbackSun{};
    const SceneLight *shadowLight = nullptr;
    const bool hasSceneLights =
        input.sceneLights != nullptr && !input.sceneLights->empty();

    const auto uploadLight = [&](const SceneLight &light) {
        switch (light.type) {
        case LightType::Directional:
            if (result.lightStats.directionalLights <
                kMaxDirectionalLights) {
                result.globalUbo.directionalLights
                    [result.lightStats.directionalLights++] =
                        makeGpuLight(light);
                if (!shadowLight)
                    shadowLight = &light;
            } else {
                ++result.lightStats.ignoredLights;
            }
            break;
        case LightType::Point:
        case LightType::Spot:
            if (result.lightStats.punctualLights < kMaxPunctualLights) {
                result.globalUbo.punctualLights
                    [result.lightStats.punctualLights++] =
                        makeGpuLight(light);
            } else {
                ++result.lightStats.ignoredLights;
            }
            break;
        }
    };

    if (hasSceneLights) {
        for (const SceneLight &light : *input.sceneLights)
            uploadLight(light);
    } else {
        fallbackSun = makeDefaultSun(input.defaultSun);
        uploadLight(fallbackSun);
    }

    result.globalUbo.lightCounts =
        glm::vec4(static_cast<float>(result.lightStats.directionalLights),
                  static_cast<float>(result.lightStats.punctualLights), 0.0f,
                  0.0f);
    result.directionalShadow = buildDirectionalShadowFrameData(
        input.sceneBounds, shadowLight, input.settings.shadowsEnabled);
    result.globalUbo.directionalShadowViewProj =
        result.directionalShadow.lightViewProjection;
    result.globalUbo.shadowParams =
        glm::vec4(result.directionalShadow.enabled ? 1.0f : 0.0f,
                  input.settings.shadowReceiverBias,
                  result.directionalShadow.texelSize, 0.0f);
    return result;
}

} // namespace vkr
