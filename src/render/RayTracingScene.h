#pragma once

#include "core/Buffer.h"
#include "core/AccelerationStructure.h"
#include "core/FrameSync.h"
#include "render/FrameGpuData.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
struct VisibilityFrame;

struct RayTracingSceneStatus {
    bool supported = false;
    bool active = false;
    std::string unavailableReason;
    uint32_t instanceCount = 0;
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> frameCapacities{};
    uint64_t allocatedBytes = 0;
};

class RayTracingScene {
  public:
    explicit RayTracingScene(Device &device);
    ~RayTracingScene();

    RayTracingScene(const RayTracingScene &) = delete;
    RayTracingScene &operator=(const RayTracingScene &) = delete;

    void build(VkCommandBuffer commandBuffer, uint32_t frameIndex,
               const VisibilityFrame &visibility);
    VkAccelerationStructureKHR handle(uint32_t frameIndex) const;
    VkBuffer metadataBuffer(uint32_t frameIndex) const;
    uint32_t instanceCount(uint32_t frameIndex) const;
    const RayTracingSceneStatus &status() const { return status_; }

  private:
    struct FrameStorage {
        std::unique_ptr<Buffer> instances;
        std::unique_ptr<Buffer> metadata;
        std::unique_ptr<Buffer> scratch;
        std::unique_ptr<AccelerationStructure> topLevel;
        uint32_t capacity = 0;
        uint32_t count = 0;
    };

    void ensureCapacity(uint32_t frameIndex, uint32_t required);
    void refreshStatus();

    Device *device_ = nullptr;
    std::array<FrameStorage, MAX_FRAMES_IN_FLIGHT> frames_{};
    RayTracingSceneStatus status_{};
};

} // namespace vkr
