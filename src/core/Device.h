#pragma once

#include <optional>
#include <vector>
#include <vulkan/vulkan.h>

#include "VulkanContext.h"
#include "vk_mem_alloc.h"
#include "vulkan_utils.h"

namespace vkr {

class Device {
  public:
    Device(VulkanContext &ctx);
    ~Device();

    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    VkDevice              logicalDevice() const { return device_; }
    VkPhysicalDevice      physicalDevice() const { return physicalDevice_; }
    VkQueue               graphicsQueue() const { return graphicsQueue_; }
    VkQueue               presentQueue() const { return presentQueue_; }
    QueueFamilyIndices    queueFamilies() const;
    VkSampleCountFlagBits msaaSamples() const { return msaaSamples_; }

    uint32_t                findMemoryType(uint32_t              typeFilter,
                                           VkMemoryPropertyFlags props) const;
    SwapChainSupportDetails querySwapChainSupport() const;

    VmaAllocator allocator() const { return allocator_; }

  private:
    void pickPhysicalDevice();
    void createLogicalDevice();

    bool               isDeviceSuitable(VkPhysicalDevice device);
    bool               checkDeviceExtensionSupport(VkPhysicalDevice device);
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;
    SwapChainSupportDetails
                          querySwapChainSupport(VkPhysicalDevice device) const;
    VkSampleCountFlagBits getMaxUsableSampleCount();
    void                  createAllocator();

    VulkanContext        &ctx_;
    VkPhysicalDevice      physicalDevice_ = VK_NULL_HANDLE;
    VkDevice              device_ = VK_NULL_HANDLE;
    VkQueue               graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue               presentQueue_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    VmaAllocator          allocator_ = VK_NULL_HANDLE;
};

} // namespace vkr