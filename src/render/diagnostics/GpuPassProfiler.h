#pragma once

#include "core/FrameSync.h"
#include "render/graph/RenderGraphTypes.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;

struct GpuPassTiming {
    std::string name;
    double milliseconds = 0.0;
};

struct GpuPassTimings {
    bool available = false;
    uint64_t frameSerial = 0;
    std::vector<GpuPassTiming> passes;
    double totalMs = 0.0;
};

struct GpuPassProfile {
    RenderGraphPassId id = 0;
    std::string name;
};

uint64_t gpuTimestampDeltaTicks(uint64_t begin, uint64_t end,
                                uint32_t validBits);
double gpuTimestampTicksToMilliseconds(uint64_t ticks,
                                       double timestampPeriodNanoseconds);

class GpuPassProfiler {
  public:
    GpuPassProfiler(Device &device, std::vector<GpuPassProfile> passes);
    ~GpuPassProfiler();

    GpuPassProfiler(const GpuPassProfiler &) = delete;
    GpuPassProfiler &operator=(const GpuPassProfiler &) = delete;

    void collect(uint32_t frameIndex);
    void beginFrame(VkCommandBuffer commandBuffer, uint32_t frameIndex,
                    uint64_t frameSerial);
    void beginPass(VkCommandBuffer commandBuffer, uint32_t frameIndex,
                   RenderGraphPassId passId);
    void endPass(VkCommandBuffer commandBuffer, uint32_t frameIndex,
                 RenderGraphPassId passId);

    bool supported() const { return queryPool_ != VK_NULL_HANDLE; }
    const GpuPassTimings &latest() const { return latest_; }

  private:
    struct FrameSlot {
        bool recorded = false;
        uint64_t frameSerial = 0;
        std::vector<uint8_t> activePasses;
        uint32_t firstStartedPass = UINT32_MAX;
        uint32_t lastCompletedPass = UINT32_MAX;
    };

    uint32_t frameQueryBase(uint32_t frameIndex) const;
    uint32_t passQuery(uint32_t frameIndex, uint32_t passIndex,
                       bool end) const;
    uint32_t passIndex(RenderGraphPassId passId) const;

    Device *device_ = nullptr;
    std::vector<GpuPassProfile> passes_;
    std::unordered_map<RenderGraphPassId, uint32_t> passSlots_;
    VkQueryPool queryPool_ = VK_NULL_HANDLE;
    uint32_t timestampValidBits_ = 0;
    double timestampPeriodNanoseconds_ = 0.0;
    uint32_t queriesPerFrame_ = 0;
    std::array<FrameSlot, MAX_FRAMES_IN_FLIGHT> frameSlots_{};
    GpuPassTimings latest_{};
};

} // namespace vkr
