#pragma once

#include "render/RenderSettings.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace vkr {

struct TemporalJitter {
    glm::vec2 pixels{0.0f};
    glm::vec2 ndc{0.0f};
    uint32_t phase = 0;
};

bool isTaaDebugView(ScreenSpaceDebugView view);
bool taaPassRequested(const RenderSettings &settings);
TemporalJitter temporalJitter(uint64_t frameSerial, VkExtent2D extent,
                              bool enabled);
glm::mat4 applyProjectionJitter(const glm::mat4 &projection,
                                const glm::vec2 &jitterNdc);

} // namespace vkr
