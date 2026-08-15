#pragma once

#include "scene_data/SceneTypes.h"

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace vkr {

struct Frustum {
    std::array<glm::vec4, 6> planes{};
    bool valid = false;

    static Frustum fromVulkanClipMatrix(const glm::mat4 &matrix);
    bool intersects(const Bounds &bounds) const;
};

std::array<glm::vec3, 8> boundsCorners(const Bounds &bounds);
Bounds transformBounds(const Bounds &localBounds, const glm::mat4 &world);
void includePoint(Bounds &bounds, const glm::vec3 &point);
void includeTransformedBounds(Bounds &bounds, const Bounds &localBounds,
                              const glm::mat4 &world);
float distanceSquaredToBounds(const glm::vec3 &point, const Bounds &bounds);
bool projectedBoundsAreSmallerThan(const Bounds &bounds,
                                   const glm::mat4 &viewProjection,
                                   VkExtent2D viewportExtent,
                                   float thresholdPixels);
bool intersectRayBounds(const glm::vec3 &origin, const glm::vec3 &direction,
                        const Bounds &bounds, float &distance);

} // namespace vkr
