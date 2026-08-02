#pragma once

#include <optional>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

#include "VulkanContext.h"
#include "VulkanTypes.h"
#include "diagnostics/SceneLoadStats.h"
#include "render/TextureTranscodeTarget.h"
#include "vk_mem_alloc.h"

namespace vkr {

class GpuDebugUtils;
class TracyProfiler;

struct ComputeBloomSupport {
    bool available = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::string reason;
};

struct AtmosphereSupport {
    bool available = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::string reason;
};

class Device {
  public:
    Device(VulkanContext &ctx);
    ~Device();

    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    VkDevice              logicalDevice() const { return device_; }
    VkPhysicalDevice      physicalDevice() const { return physicalDevice_; }
    VkPhysicalDeviceProperties physicalDeviceProperties() const;
    VkQueue               graphicsQueue() const { return graphicsQueue_; }
    VkQueue               presentQueue() const { return presentQueue_; }
    QueueFamilyIndices    queueFamilies() const;
    VkSampleCountFlagBits msaaSamples() const { return msaaSamples_; }
    TextureTranscodeTarget textureTranscodeTarget() const {
        return textureTranscodeTarget_;
    }
    bool environmentIblSupported() const {
        return environmentIblSupported_;
    }
    const ComputeBloomSupport &computeBloomSupport() const {
        return computeBloomSupport_;
    }
    const AtmosphereSupport &atmosphereSupport() const {
        return atmosphereSupport_;
    }

    SwapChainSupportDetails querySwapChainSupport() const;

    VmaAllocator allocator() const { return allocator_; }
    AllocatorMemorySnapshot allocatorMemorySnapshot() const;
    GpuDebugUtils &debugUtils() { return *debugUtils_; }
    const GpuDebugUtils &debugUtils() const { return *debugUtils_; }
    TracyProfiler &tracyProfiler() { return *tracyProfiler_; }
    const TracyProfiler &tracyProfiler() const { return *tracyProfiler_; }

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
    TextureTranscodeTarget textureTranscodeTarget_ =
        TextureTranscodeTarget::Rgba8;
    bool environmentIblSupported_ = false;
    ComputeBloomSupport computeBloomSupport_{};
    AtmosphereSupport atmosphereSupport_{};
    VmaAllocator          allocator_ = VK_NULL_HANDLE;
    std::unique_ptr<GpuDebugUtils> debugUtils_;
    std::unique_ptr<TracyProfiler> tracyProfiler_;
};

} // namespace vkr
