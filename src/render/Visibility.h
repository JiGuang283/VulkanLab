#pragma once

#include "render/RenderCommand.h"
#include "scene/BoundsMath.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

struct RenderView;

struct VisibilityCpuStatistics {
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
    uint32_t gpuUncullable = 0;
};

struct CompletedGpuVisibilityStatistics {
    uint64_t frameSerial = 0;
    uint32_t candidates = 0;
    uint32_t visible = 0;
    uint32_t occluded = 0;
};

struct GpuVisibilityDrawStream {
    VkBuffer indirectBuffer = VK_NULL_HANDLE;
    uint32_t candidateCount = 0;
    uint32_t frameIndex = 0;
    uint64_t visibilityGeneration = 0;
    bool active = false;
};

struct TemporalFrameHistoryData {
    glm::mat4 previousViewProjection{1.0f};
    glm::mat4 currentViewProjection{1.0f};
    glm::mat4 currentProjection{1.0f};
    glm::vec3 cameraPosition{0.0f};
    glm::vec3 cameraForward{0.0f, 0.0f, -1.0f};
    VkExtent2D viewportExtent{};
    uint64_t sceneGeneration = 0;
    uint64_t historyGeneration = 0;
    uint32_t historyValidItems = 0;
    bool globalValid = false;
    std::string cameraIdentity;
    std::string invalidationReason;
};

struct VisibilityFrame {
    uint64_t generation = 0;
    std::vector<RenderItem> items;
    std::vector<RenderItemIndex> cameraOpaque;
    std::vector<RenderItemIndex> cameraTransparent;
    std::vector<RenderItemIndex> shadowCasters;
    VisibilityCpuStatistics cpuStats{};
    TemporalFrameHistoryData history{};

    size_t cameraDrawCount() const {
        return cameraOpaque.size() + cameraTransparent.size();
    }
};

struct VisibilityBuildInput {
    uint64_t sceneGeneration = 0;
    std::string cameraIdentity = "editor";
    Bounds sceneBounds{};
};

class VisibilitySystem {
  public:
    VisibilityFrame build(std::vector<RenderItem> source,
                          const RenderView &view,
                          VkExtent2D viewportExtent,
                          VisibilityBuildInput input);
    void commit(const VisibilityFrame &frame);
    void invalidate(std::string reason);

    uint64_t historyGeneration() const { return historyGeneration_; }
    const std::string &lastInvalidationReason() const {
        return lastInvalidationReason_;
    }

  private:
    std::unordered_map<RenderItemKey, glm::mat4, RenderItemKeyHash>
        previousWorld_;
    glm::mat4 previousViewProjection_{1.0f};
    glm::mat4 previousProjection_{1.0f};
    glm::vec3 previousCameraPosition_{0.0f};
    glm::vec3 previousCameraForward_{0.0f, 0.0f, -1.0f};
    VkExtent2D previousViewportExtent_{};
    uint64_t previousSceneGeneration_ = 0;
    uint64_t historyGeneration_ = 0;
    uint64_t nextVisibilityGeneration_ = 1;
    std::string previousCameraIdentity_;
    std::string forcedInvalidationReason_;
    std::string lastInvalidationReason_ = "initial frame";
    bool committed_ = false;
};

} // namespace vkr
