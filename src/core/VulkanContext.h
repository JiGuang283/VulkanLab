#pragma once

#include <functional>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

using SurfaceCreator = std::function<VkSurfaceKHR(VkInstance)>;

class VulkanContext {
  public:
    VulkanContext(SurfaceCreator            createSurface,
                  std::vector<const char *> requiredExtensions,
                  bool enableValidation);
    ~VulkanContext();

    VulkanContext(const VulkanContext &) = delete;
    VulkanContext &operator=(const VulkanContext &) = delete;

    VkInstance   instance() const { return instance_; }
    VkSurfaceKHR surface() const { return surface_; }
    bool validationEnabled() const { return validationEnabled_; }

  private:
    void createInstance(std::vector<const char *> requiredExtensions);
    void setupDebugMessenger();

    bool checkValidationLayerSupport();
    void populateDebugMessengerCreateInfo(
        VkDebugUtilsMessengerCreateInfoEXT &createInfo);

    VkInstance               instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR             surface_ = VK_NULL_HANDLE;
    bool validationEnabled_ = false;
};

} // namespace vkr
