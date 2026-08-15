#pragma once

#include "render/DirectionalShadow.h"
#include "render/PunctualShadow.h"
#include "render/RenderSettings.h"
#include "render/SceneLight.h"
#include "scene_data/SceneTypes.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vkr {

struct ShadowLightBinding {
    LightType type = LightType::Directional;
    int32_t slot = -1;
    float farPlane = 0.0f;
};

using ShadowLightBindingMap =
    std::unordered_map<std::string, ShadowLightBinding>;

struct ShadowAllocation {
    LightType type = LightType::Directional;
    uint32_t slot = 0;
    std::optional<PersistentEntityId> entity;
    std::string stableKey;
    std::string name;
    ShadowCastingPolicy policy = ShadowCastingPolicy::Disabled;
    float score = 0.0f;
    float farPlane = 0.0f;
    uint32_t age = 0;
    bool retained = false;
    bool focused = false;
};

struct ShadowEviction {
    LightType type = LightType::Point;
    std::string stableKey;
    std::string reason;
};

struct ShadowFrameStatistics {
    uint32_t pointCandidates = 0;
    uint32_t spotCandidates = 0;
    uint32_t reactiveFramesRemaining = 0;
    std::vector<ShadowEviction> evictions;
};

struct ShadowFramePlan {
    std::vector<SceneLight> effectiveLights;
    CsmFrameData csm{};
    PunctualShadowFrameData punctual{};
    std::vector<ShadowAllocation> allocations;
    ShadowLightBindingMap lightBindings;
    ShadowFrameStatistics statistics{};
    std::optional<PersistentEntityId> directionalEntity;
    std::string directionalStableKey;
    std::string directionalName;
    uint64_t contentRevision = 0;
    bool temporalReactive = false;
};

struct ShadowBuildInput {
    const std::vector<SceneLight> *sceneLights = nullptr;
    Bounds sceneBounds{};
    glm::mat4 cameraView{1.0f};
    glm::mat4 cameraProjection{1.0f};
    glm::vec3 cameraPosition{0.0f};
    float cameraNearPlane = 0.05f;
    float cameraFarPlane = 1000.0f;
    glm::vec3 fallbackSunDirection{0.3f, 0.8f, 0.5f};
    glm::vec3 fallbackSunColor{1.0f};
    float fallbackSunIntensity = 3.0f;
    bool fallbackSunEnabled = false;
    RenderSettings settings{};
    std::optional<PersistentEntityId> focusedLightEntity;
};

struct ShadowSlotState {
    std::string stableKey;
    float score = 0.0f;
    uint32_t priority = 0;
    uint32_t age = 0;
};

bool isEffectiveSceneLight(const SceneLight &light);
std::string sceneLightStableKey(const SceneLight &light,
                                size_t fallbackIndex);

class ShadowSystem {
  public:
    ShadowFramePlan build(const ShadowBuildInput &input);
    void reset();

  private:
    std::array<ShadowSlotState, kMaxPointShadowLights> pointSlots_{};
    std::array<ShadowSlotState, kMaxSpotShadowLights> spotSlots_{};
    uint64_t previousContentHash_ = 0;
    uint64_t contentRevision_ = 0;
    uint32_t reactiveFramesRemaining_ = 0;
};

} // namespace vkr
