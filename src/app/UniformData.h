#pragma once

#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <glm/glm.hpp>

namespace vkr {

inline constexpr uint32_t kMaxDirectionalLights = 1;
inline constexpr uint32_t kMaxPunctualLights = 8;

struct GpuLight {
    alignas(16) glm::vec4 positionRange;
    alignas(16) glm::vec4 directionInnerCos;
    alignas(16) glm::vec4 colorIntensity;
    alignas(16) glm::vec4 params;
};

struct GlobalUBO {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
    alignas(16) glm::vec4 cameraPosWS;
    alignas(16) glm::vec4 ambientColorIntensity;
    alignas(16) glm::vec4 lightCounts;
    alignas(16) GpuLight directionalLights[kMaxDirectionalLights];
    alignas(16) GpuLight punctualLights[kMaxPunctualLights];
};

} // namespace vkr
