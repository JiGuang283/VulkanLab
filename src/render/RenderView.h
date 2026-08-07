#pragma once

#include "render/DirectionalShadow.h"
#include "render/FrameGpuData.h"
#include "render/RenderSettings.h"
#include "scene/SceneLight.h"
#include "scene/IRenderWorld.h"
#include "scene/SceneTypes.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

struct DefaultSunSettings {
    glm::vec3 direction{0.3f, 0.8f, 0.5f};
    glm::vec3 color{1.0f};
    float intensity = 3.0f;
};

struct RenderViewInput {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec2 projectionJitterNdc{0.0f};
    glm::vec2 projectionJitterPixels{0.0f};
    glm::vec3 cameraPosition{0.0f};
    Bounds sceneBounds{};
    const std::vector<SceneLight> *sceneLights = nullptr;
    const std::vector<RenderWorldReflectionProbe> *reflectionProbes = nullptr;
    glm::vec3 ambientColor{1.0f};
    float ambientIntensity = 0.08f;
    DefaultSunSettings defaultSun{};
    bool fallbackSunEnabled = true;
    RenderSettings settings{};
    bool environmentReady = false;
    float maxSpecularLod = 0.0f;
    float cameraNearPlane = 0.05f;
    float cameraFarPlane = 1000.0f;
    VkExtent2D viewportExtent{};
    std::optional<RenderWorldAtmosphere> atmosphere;
    bool atmosphereSupported = false;
    std::optional<RenderWorldDdgiVolume> ddgiProbeVolume;
    bool ddgiSupported = false;
};

struct AtmosphereFrameData {
    bool componentPresent = false;
    bool active = false;
    PersistentEntityId componentEntity;
    std::optional<PersistentEntityId> sunEntity;
    std::string sunStableKey;
    int32_t sunBufferIndex = -1;
    float cameraAltitudeKm = 0.0f;
    uint64_t staticLutKey = 0;
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

struct RenderViewReflectionProbe {
    PersistentEntityId entityId;
    GpuReflectionProbe gpu{};
    std::shared_ptr<EnvironmentGpuResources> environment;
    uint64_t environmentGeneration = 0;
};

struct RenderViewReflectionProbeStats {
    uint32_t sourceCount = 0;
    uint32_t activeCount = 0;
    uint32_t ignoredCount = 0;
    std::vector<PersistentEntityId> ignoredEntityIds;
};

struct DdgiFrameData {
    bool componentPresent = false;
    bool active = false;
    PersistentEntityId componentEntity;
    glm::mat4 localToWorld{1.0f};
    glm::mat4 worldToLocal{1.0f};
    DdgiProbeVolumeComponentDocument parameters{};
    uint32_t probeCount = 0;
};

struct RenderView {
    GlobalFrameUbo globalUbo{};
    glm::mat4 stableProjection{1.0f};
    glm::mat4 stableViewProjection{1.0f};
    glm::vec2 projectionJitterNdc{0.0f};
    glm::vec2 projectionJitterPixels{0.0f};
    std::vector<GpuLight> sceneLights;
    std::vector<RenderViewReflectionProbe> reflectionProbes;
    DirectionalShadowFrameData directionalShadow{};
    RenderSettings settings{};
    RenderViewLightStats lightStats{};
    RenderViewReflectionProbeStats reflectionProbeStats{};
    AtmosphereFrameData atmosphere{};
    AtmosphereGpuParams atmosphereGpuParams{};
    DdgiFrameData ddgi{};
    float cameraNearPlane = 0.05f;
    float cameraFarPlane = 1000.0f;
};

bool isEffectiveSceneLight(const SceneLight &light);
RenderView buildRenderView(const RenderViewInput &input);

} // namespace vkr
