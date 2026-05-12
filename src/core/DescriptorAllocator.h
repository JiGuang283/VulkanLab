#pragma once

#include <vector>

#include <vulkan/vulkan.h>

namespace vkr {

class Device;

class DescriptorAllocator {
  public:
    explicit DescriptorAllocator(Device &device);
    ~DescriptorAllocator();

    DescriptorAllocator(const DescriptorAllocator &) = delete;
    DescriptorAllocator &operator=(const DescriptorAllocator &) = delete;

    VkDescriptorSet allocate(VkDescriptorSetLayout layout);
    void            resetPools();

  private:
    VkDescriptorPool createPool();
    VkDescriptorPool currentPool();

    Device                       *device_ = nullptr;
    std::vector<VkDescriptorPool> usedPools_;
    std::vector<VkDescriptorPool> freePools_;
};

} // namespace vkr