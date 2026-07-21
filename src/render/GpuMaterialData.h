#pragma once

#include <glm/glm.hpp>

namespace vkr {

struct GpuPushBlock {
    glm::mat4 model{1.0f};
    glm::vec4 baseColorFactor{1.0f};
    glm::vec4 emissiveMetallic{0.0f};
    glm::vec4 roughnessAlpha{0.0f};
    glm::vec4 reserved{0.0f};
};

static_assert(sizeof(GpuPushBlock) == 128, "push block must be 128B");

} // namespace vkr
