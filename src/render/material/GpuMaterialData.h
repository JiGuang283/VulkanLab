#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <type_traits>

namespace vkr {

struct alignas(16) GpuMaterial {
    glm::vec4 baseColorFactor{1.0f};
    glm::vec4 emissiveMetallic{0.0f};
    glm::vec4 roughnessAlphaOcclusionNormal{1.0f, 0.5f, 1.0f, 1.0f};
    glm::vec4 transmissionVolume{0.0f};
    glm::vec4 attenuationColor{1.0f};
    glm::uvec4 textureIndices0{0u};
    glm::uvec4 textureIndices1{0u};
    glm::uvec4 reserved{0u};
};

struct alignas(16) GpuDrawPushBlock {
    glm::mat4 model{1.0f};
    glm::uvec4 indices{0u};
    glm::vec4 reserved[3]{};
};

using GpuPushBlock = GpuDrawPushBlock;

static_assert(std::is_standard_layout_v<GpuMaterial>);
static_assert(std::is_standard_layout_v<GpuDrawPushBlock>);
static_assert(sizeof(GpuMaterial) == 128, "GPU material must be 128B");
static_assert(sizeof(GpuDrawPushBlock) == 128, "draw push block must be 128B");
static_assert(offsetof(GpuDrawPushBlock, model) == 0);
static_assert(offsetof(GpuDrawPushBlock, indices) == 64);
static_assert(offsetof(GpuDrawPushBlock, reserved) == 80);

} // namespace vkr
