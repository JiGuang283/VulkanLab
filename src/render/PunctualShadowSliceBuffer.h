#pragma once

#include "core/FrameSync.h"
#include "render/PunctualShadow.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Buffer;
class DescriptorAllocator;
class Device;

class PunctualShadowSliceBuffer {
  public:
    PunctualShadowSliceBuffer(Device &device,
                              DescriptorAllocator &descriptorAllocator,
                              uint32_t sliceCount,
                              VkShaderStageFlags stageFlags,
                              std::string debugName);
    ~PunctualShadowSliceBuffer();

    PunctualShadowSliceBuffer(const PunctualShadowSliceBuffer &) = delete;
    PunctualShadowSliceBuffer &
    operator=(const PunctualShadowSliceBuffer &) = delete;

    void write(uint32_t frameIndex, uint32_t sliceIndex,
               const PunctualShadowSlice &slice);
    uint32_t dynamicOffset(uint32_t sliceIndex) const;
    VkDescriptorSetLayout descriptorSetLayout() const { return layout_; }
    VkDescriptorSet descriptorSet(uint32_t frameIndex) const {
        return descriptorSets_.at(frameIndex);
    }

  private:
    Device *device_ = nullptr;
    uint32_t sliceCount_ = 0;
    VkDeviceSize stride_ = 0;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> buffers_{};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptorSets_{};
};

} // namespace vkr
