#pragma once

#include <cstdint>
#include <glm/glm.hpp>

namespace vkr {

struct Bounds;
struct SceneLight;

inline constexpr uint32_t kDirectionalShadowMapSize = 2048;

struct DirectionalShadowFrameData {
    glm::mat4 lightViewProjection{1.0f};
    bool      enabled = false;
    float     texelSize = 1.0f / static_cast<float>(kDirectionalShadowMapSize);
};

DirectionalShadowFrameData buildDirectionalShadowFrameData(
    const Bounds &bounds, const SceneLight *directionalLight,
    bool shadowsEnabled,
    uint32_t shadowMapSize = kDirectionalShadowMapSize);

} // namespace vkr
