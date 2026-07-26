#pragma once

#include "ValidationProfile.h"

#include <atomic>
#include <functional>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

using SurfaceCreator = std::function<VkSurfaceKHR(VkInstance)>;

struct VulkanContextOptions {
    ValidationProfile validationProfile = ValidationProfile::Core;
    bool validationAllowed = true;
    bool debugUtilsRequested = true;
};

class VulkanContext {
  public:
    VulkanContext(SurfaceCreator            createSurface,
                  std::vector<const char *> requiredExtensions,
                  VulkanContextOptions options = {});
    ~VulkanContext();

    VulkanContext(const VulkanContext &) = delete;
    VulkanContext &operator=(const VulkanContext &) = delete;

    VkInstance   instance() const { return instance_; }
    VkSurfaceKHR surface() const { return surface_; }
    bool validationEnabled() const {
        return validationStatus_.actual != ValidationProfile::Off;
    }
    bool debugUtilsEnabled() const {
        return validationStatus_.debugUtilsEnabled;
    }
    uint32_t instanceApiVersion() const { return instanceApiVersion_; }
    ValidationStatus validationStatus() const;

  private:
    void createInstance(std::vector<const char *> requiredExtensions);
    void setupDebugMessenger();

    void populateDebugMessengerCreateInfo(
        VkDebugUtilsMessengerCreateInfoEXT &createInfo);
    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT *callbackData,
        void *userData);

    VkInstance               instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR             surface_ = VK_NULL_HANDLE;
    VulkanContextOptions options_{};
    ValidationStatus validationStatus_{};
    uint32_t instanceApiVersion_ = VK_API_VERSION_1_0;
    std::atomic_uint64_t validationWarnings_{0};
    std::atomic_uint64_t validationErrors_{0};
};

} // namespace vkr
