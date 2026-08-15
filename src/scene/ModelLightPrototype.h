#pragma once

#include "render/frame/SceneLight.h"

#include <glm/glm.hpp>

#include <string>

namespace vkr {

struct ModelLightPrototype {
    std::string debugName;
    LightType type = LightType::Directional;
    bool castsShadow = false;

    glm::vec3 positionAS{0.0f};
    float range = 10.0f;

    glm::vec3 directionAS{0.3f, 0.8f, 0.5f};
    float innerConeCos = 1.0f;

    glm::vec3 color{1.0f};
    float intensity = 3.0f;

    float outerConeCos = 0.0f;
};

} // namespace vkr
