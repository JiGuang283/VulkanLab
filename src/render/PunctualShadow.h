#pragma once

#include "RenderSettings.h"

#include <array>
#include <cstdint>
#include <glm/glm.hpp>

namespace vkr {

struct Bounds;
struct SceneLight;

inline constexpr uint32_t kPointShadowMapSize = 1024;
inline constexpr uint32_t kSpotShadowMapSize = 1024;
inline constexpr uint32_t kPointShadowFaceCount = 6;
inline constexpr uint32_t kPointShadowLayers =
    kMaxPointShadowLights * kPointShadowFaceCount;

struct alignas(16) PunctualShadowSlice {
    glm::mat4 viewProjection{1.0f};
    glm::vec4 lightPositionFar{0.0f};
};

static_assert(sizeof(PunctualShadowSlice) == 80);

// Per-cubemap-face view matrix directions (right-handed, looking out from
// the light position along each axis).
std::array<glm::mat4, kPointShadowFaceCount> pointShadowFaceViews(
    const glm::vec3 &lightPosition);

// Point light shadow data for one light.
struct PointShadowData {
    std::array<glm::mat4, kPointShadowFaceCount> faceViewProjections{};
    glm::vec3 position{0.0f};
    float farPlane = 50.0f;
    bool enabled = false;
    uint32_t layerBase = 0;
};

// Spot light shadow data for one light.
struct SpotShadowData {
    glm::mat4 viewProjection{1.0f};
    glm::mat4 view{1.0f};
    float fovY = glm::radians(60.0f);
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
    bool enabled = false;
    uint32_t layerBase = 0;
};

// Collection of all punctual shadow data for a frame.
struct PunctualShadowFrameData {
    std::array<PointShadowData, kMaxPointShadowLights> points{};
    std::array<SpotShadowData, kMaxSpotShadowLights> spots{};
    uint32_t activePointCount = 0;
    uint32_t activeSpotCount = 0;
};

struct PointShadowLightInput {
    glm::vec3 position{0.0f};
    float farPlane = 50.0f;
};

struct SpotShadowLightInput {
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f};
    float fovY = glm::radians(60.0f);
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
};

// Compute shadow projection for one point light.
PointShadowData buildPointShadowData(const PointShadowLightInput &input,
                                     uint32_t layerBase,
                                     uint32_t shadowMapSize);

// Compute shadow projection for one spot light.
SpotShadowData buildSpotShadowData(const SpotShadowLightInput &input,
                                   uint32_t layerBase,
                                   uint32_t shadowMapSize);

} // namespace vkr
