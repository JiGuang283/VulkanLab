#pragma once

#include <glm/glm.hpp>

namespace vkr {

struct CameraPose {
    glm::vec3 position{2.0f, 2.0f, 2.0f};
    float     yaw = -135.0f;
    float     pitch = -30.0f;
};

struct Bounds {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    glm::vec3 center{0.0f};
    float     radius = 0.0f;
    bool      valid = false;
};

} // namespace vkr
