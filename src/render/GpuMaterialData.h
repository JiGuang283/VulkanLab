#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <type_traits>

namespace vkr {

struct GpuPushBlock {
    glm::mat4 model{1.0f};
    glm::vec4 baseColorFactor{1.0f};
    glm::vec4 emissiveMetallic{0.0f};
    glm::vec4 roughnessAlpha{0.0f};
    glm::vec4 reserved{0.0f};
};

static_assert(std::is_standard_layout_v<GpuPushBlock>);
static_assert(sizeof(GpuPushBlock) == 128, "push block must be 128B");
static_assert(offsetof(GpuPushBlock, model) == 0);
static_assert(offsetof(GpuPushBlock, baseColorFactor) == 64);
static_assert(offsetof(GpuPushBlock, emissiveMetallic) == 80);
static_assert(offsetof(GpuPushBlock, roughnessAlpha) == 96);
static_assert(offsetof(GpuPushBlock, reserved) == 112);

} // namespace vkr
