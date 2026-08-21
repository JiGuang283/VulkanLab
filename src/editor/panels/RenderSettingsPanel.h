#pragma once

#include "render/Renderer.h"
#include "render/features/global_illumination/DdgiPass.h"
#include "render/features/shadows_visibility/ShadowSystem.h"
#include "render/frame/RenderView.h"
#include "render/frame/RenderSettingsController.h"
#include "render/shader/ShaderRegistry.h"
#include "scene/EnvironmentAssetRepository.h"
#include "render/frame/SceneLight.h"

#include <glm/glm.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

struct RenderEnvironmentOption {
    std::string id;
    std::string displayName;
};

struct RenderEnvironmentLoadSnapshot {
    uint64_t taskId = 0;
    std::string state;
    uint32_t uploadedImages = 0;
    uint32_t totalImages = 0;
    bool active = false;
};

struct RenderCameraPanelSnapshot {
    glm::vec3 position{0.0f};
    float moveSpeed = 2.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    Bounds sceneBounds{};
};

struct RenderSettingsPanelSnapshot {
    bool advanced = false;
    RenderSettingsSnapshot features{};
    const std::vector<ViewMode> *viewModes = nullptr;
    std::string currentViewModeId;
    std::string currentViewModeDisplayName;
    bool viewModeSupportsBloom = false;
    bool viewModeSupportsScreenSpace = false;
    bool viewModeSupportsDdgi = false;

    uint32_t textureLimit = 2048;
    bool textureLimitLocked = false;
    std::string textureLimitHelp;

    ScreenSpaceEffectsStatus screenSpace{};
    DdgiRuntimeStatus ddgi{};
    SurfaceDataStatus surfaceData{};
    GBufferRuntimeStatus gBuffer{};
    DeferredLightingRuntimeStatus deferredLighting{};
    OcclusionCullingStatus occlusion{};
    VisibilityCpuStatistics visibilityStats{};
    TemporalFrameHistoryData temporalHistory{};
    uint32_t renderItemCount = 0;

    RenderViewLightStats lightStats{};
    AtmosphereRuntimeStatus atmosphere{};
    ReflectionProbeRuntimeStatus reflectionProbes{};
    EnvironmentAssetRepositorySnapshot environmentRepository{};
    SceneLightBufferStatus lightBuffer{};
    ClusteredLightingStatus clusteredLighting{};
    std::vector<SceneLight> sceneLights;
    std::vector<RenderEnvironmentOption> environments;
    std::string selectedEnvironmentId;
    std::string selectedEnvironmentName = "None";
    std::string environmentError;
    std::string reflectionProbeCaptureStatus;
    std::optional<RenderEnvironmentLoadSnapshot> environmentLoad;
    bool environmentIblSupported = false;
    bool environmentReady = false;
    glm::vec3 ambientColor{0.03f};
    float ambientIntensity = 1.0f;
    glm::vec3 fallbackSunDirection{0.3f, 0.8f, 0.5f};
    glm::vec3 fallbackSunColor{1.0f};
    float fallbackSunIntensity = 1.0f;

    RenderCameraPanelSnapshot camera{};
};

struct RenderSettingsPanelActions {
    std::function<void(bool)> setAdvanced;
    std::function<void(const std::string &)> setViewMode;
    std::function<void(uint32_t)> setTextureLimit;
    std::function<void(const RenderSettingsPatch &)> applySettings;
    std::function<void(const std::string &)> setEnvironment;
    std::function<void(uint64_t)> cancelEnvironmentLoad;
    std::function<void(glm::vec3)> setAmbientColor;
    std::function<void(float)> setAmbientIntensity;
    std::function<void(glm::vec3)> setFallbackSunDirection;
    std::function<void(glm::vec3)> setFallbackSunColor;
    std::function<void(float)> setFallbackSunIntensity;
    std::function<void()> createDdgiProbeVolume;
    std::function<void(float)> setCameraMoveSpeed;
    std::function<void(float, float)> setCameraClipPlanes;
};

class RenderSettingsPanel {
  public:
    void draw(const RenderSettingsPanelSnapshot &snapshot,
              const RenderSettingsPanelActions &actions) const;

  private:
    void drawOutput(const RenderSettingsPanelSnapshot &snapshot,
                    const RenderSettingsPanelActions &actions) const;
    void drawLighting(const RenderSettingsPanelSnapshot &snapshot,
                      const RenderSettingsPanelActions &actions) const;
    void drawEffects(const RenderSettingsPanelSnapshot &snapshot,
                     const RenderSettingsPanelActions &actions) const;
    void drawSurfaceData(const RenderSettingsPanelSnapshot &snapshot,
                         const RenderSettingsPanelActions &actions) const;
    void drawVisibility(const RenderSettingsPanelSnapshot &snapshot,
                        const RenderSettingsPanelActions &actions) const;
    void drawCamera(const RenderSettingsPanelSnapshot &snapshot,
                    const RenderSettingsPanelActions &actions) const;
};

} // namespace vkr
