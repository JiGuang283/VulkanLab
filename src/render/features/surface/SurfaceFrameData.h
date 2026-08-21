#pragma once

#include "core/FrameSync.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vulkan/vulkan.h>

namespace vkr {

class Buffer;
class DescriptorAllocator;
class Device;
struct VisibilityFrame;

class SurfaceFrameData {
  public:
    SurfaceFrameData(Device &device,
                     DescriptorAllocator &descriptorAllocator);
    ~SurfaceFrameData();

    SurfaceFrameData(const SurfaceFrameData &) = delete;
    SurfaceFrameData &operator=(const SurfaceFrameData &) = delete;

    void prepare(uint32_t frameIndex, const VisibilityFrame &visibility,
                 VkExtent2D extent);

    VkDescriptorSetLayout descriptorSetLayout() const {
        return descriptorSetLayout_;
    }
    VkDescriptorSet descriptorSet(uint32_t frameIndex) const;
    uint32_t historyCapacity(uint32_t frameIndex) const;
    uint64_t allocatedBytes() const;

  private:
    struct FrameStorage;

    void createDescriptorSetLayout();
    void createFrameStorage();
    void updateDescriptor(uint32_t frameIndex);
    void ensureHistoryCapacity(uint32_t frameIndex, uint32_t required);

    Device *device_ = nullptr;
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<std::unique_ptr<FrameStorage>, MAX_FRAMES_IN_FLIGHT> frames_{};
};

} // namespace vkr
