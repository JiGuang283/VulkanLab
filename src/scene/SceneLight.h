#pragma once

#include "scene_data/SceneIds.h"

#include <cstdint>
#include <optional>
#include <string>

#include <glm/glm.hpp>

namespace vkr {

enum class LightType : uint32_t {
    Directional = 0,
    Point = 1,
    Spot = 2,
};

enum class SceneLightSource : uint32_t {
    Legacy,
    ExplicitEntity,
    ImportedModel,
    Fallback,
};

struct SceneLight {
    std::string debugName;
    std::string stableKey;
    SceneLightSource source = SceneLightSource::Legacy;
    std::optional<PersistentEntityId> ownerEntity;
    bool castsShadow = false;
    std::optional<uint32_t> atmosphereSunIndex;
    float sourceAngularRadiusRadians = 0.004675f;
    LightType type = LightType::Directional;

    glm::vec3 positionWS{0.0f};
    float     range = 10.0f;

    glm::vec3 directionWS{0.3f, 0.8f, 0.5f};
    float     innerConeCos = 1.0f;

    glm::vec3 color{1.0f};
    float     intensity = 3.0f;

    float outerConeCos = 0.0f;
};

} // namespace vkr
