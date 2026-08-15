#pragma once

#include <array>
#include <cstdint>
#include <glm/glm.hpp>

namespace vkr {

struct Bounds;
struct SceneLight;

inline constexpr uint32_t kDirectionalShadowMapSize = 2048;
inline constexpr uint32_t kCsmCascadeCount = 4;

struct DirectionalShadowFrameData {
    glm::mat4 lightViewProjection{1.0f};
    glm::mat4 lightView{1.0f};
    glm::vec3 lightSpaceMin{0.0f};
    glm::vec3 lightSpaceMax{0.0f};
    bool      enabled = false;
    bool      receiverFitted = false;
    float     texelSize = 1.0f / static_cast<float>(kDirectionalShadowMapSize);
    float     receiverDistance = 0.0f;
};

struct CsmCascadeData {
    glm::mat4 lightViewProjection{1.0f};
    float splitDistance = 0.0f;
    float texelSize = 0.0f;
};

struct CsmFrameData {
    std::array<CsmCascadeData, kCsmCascadeCount> cascades{};
    std::array<float, kCsmCascadeCount> splitDepths{};
    glm::vec3 lightDirection{0.0f};
    bool enabled = false;
    uint32_t cascadeCount = kCsmCascadeCount;
};

struct DirectionalShadowCameraData {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    float nearPlane = 0.05f;
    float farPlane = 1000.0f;
    float shadowDistance = 200.0f;
};

DirectionalShadowFrameData buildDirectionalShadowFrameData(
    const Bounds &bounds, const SceneLight *directionalLight,
    bool shadowsEnabled,
    const DirectionalShadowCameraData *camera,
    uint32_t shadowMapSize = kDirectionalShadowMapSize);

DirectionalShadowFrameData buildDirectionalShadowFrameData(
    const Bounds &bounds, const SceneLight *directionalLight,
    bool shadowsEnabled,
    uint32_t shadowMapSize = kDirectionalShadowMapSize);

// CSM: compute cascade split distances in view-space Z (positive distance).
std::array<float, kCsmCascadeCount> computeCsmSplits(
    float nearPlane, float farPlane, float shadowDistance,
    float lambda = 0.75f);

// CSM: build per-cascade shadow matrices fitted to each sub-frustum.
CsmFrameData buildCsmFrameData(
    const Bounds &bounds, const SceneLight *directionalLight,
    bool shadowsEnabled, const DirectionalShadowCameraData &camera,
    uint32_t shadowMapSize = kDirectionalShadowMapSize);

} // namespace vkr
