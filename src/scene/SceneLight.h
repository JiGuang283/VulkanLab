#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace vkr {

enum class LightType : uint32_t {
    Directional = 0,
    Point = 1,
    Spot = 2,
};

struct SceneLight {
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
