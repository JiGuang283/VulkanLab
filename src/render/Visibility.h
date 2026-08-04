#pragma once

#include "render/RenderQueue.h"
#include "scene/SceneTypes.h"

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace vkr {

struct RenderView;

struct Frustum {
    std::array<glm::vec4, 6> planes{};
    bool valid = false;

    static Frustum fromVulkanClipMatrix(const glm::mat4 &matrix);
    bool intersects(const Bounds &bounds) const;
};

struct VisibilityStatistics {
    uint32_t sourceDraws = 0;
    uint32_t invalidBounds = 0;
    uint32_t frustumCulled = 0;
    uint32_t distanceCulled = 0;
    uint32_t smallObjectCulled = 0;
    uint32_t cameraVisible = 0;
    uint32_t cameraOpaque = 0;
    uint32_t cameraTransparent = 0;
    uint32_t shadowCandidates = 0;
    uint32_t shadowCulled = 0;
    uint32_t shadowVisible = 0;
    uint32_t depthPrepassDraws = 0;
    uint32_t occlusionCandidates = 0;
    uint32_t gpuOccluded = 0;
    uint64_t gpuStatsFrameSerial = 0;
};

struct VisibilityFrame {
    RenderQueue camera;
    RenderQueue shadowCasters;
    RenderQueue depthPrepass;
    VisibilityStatistics stats{};
};

Bounds transformBounds(const Bounds &localBounds, const glm::mat4 &world);

class VisibilitySystem {
  public:
    VisibilityFrame build(const RenderQueue &source, const RenderView &view,
                          VkExtent2D viewportExtent) const;
};

} // namespace vkr
