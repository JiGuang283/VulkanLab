#pragma once

#include <cstdint>
#include <glm/glm.hpp>

namespace vkr {

struct Bounds;
struct SceneLight;

inline constexpr uint32_t kDirectionalShadowMapSize = 2048;

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

} // namespace vkr
