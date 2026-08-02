#pragma once

#include "render/DirectionalShadow.h"
#include "render/FrameGpuData.h"
#include "render/RenderSettings.h"
#include "scene/SceneLight.h"
#include "scene/SceneTypes.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

struct DefaultSunSettings {
    glm::vec3 direction{0.3f, 0.8f, 0.5f};
    glm::vec3 color{1.0f};
    float intensity = 3.0f;
};

struct RenderViewInput {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 cameraPosition{0.0f};
    Bounds sceneBounds{};
    const std::vector<SceneLight> *sceneLights = nullptr;
    glm::vec3 ambientColor{1.0f};
    float ambientIntensity = 0.08f;
    DefaultSunSettings defaultSun{};
    bool fallbackSunEnabled = true;
    RenderSettings settings{};
    bool environmentReady = false;
    float maxSpecularLod = 0.0f;
};

struct RenderViewLightStats {
    uint32_t effectiveLights = 0;
    uint32_t directionalLights = 0;
    uint32_t pointLights = 0;
    uint32_t spotLights = 0;
    uint32_t punctualLights = 0;
    uint32_t totalLights = 0;
    uint32_t ignoredLights = 0;
    std::vector<PersistentEntityId> ignoredEntityIds;
    std::vector<std::string> ignoredStableKeys;
    std::optional<PersistentEntityId> shadowCasterEntity;
    std::string shadowCasterKey;
    std::string shadowCasterName;
    int32_t shadowCasterBufferIndex = -1;
    bool shadowCasterActive = false;
};

struct RenderView {
    GlobalFrameUbo globalUbo{};
    std::vector<GpuLight> sceneLights;
    DirectionalShadowFrameData directionalShadow{};
    RenderSettings settings{};
    RenderViewLightStats lightStats{};
};

bool isEffectiveSceneLight(const SceneLight &light);
RenderView buildRenderView(const RenderViewInput &input);

} // namespace vkr
