#pragma once

#include <vulkan/vulkan.h>
#include <vector>

struct  GLFWwindow;


namespace vkr{

class VulkanContext{
public:
    VulkanContext(GLFWwindow* window);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext) = delete;

    VkInstance instance() const {return instance_;}
    VkSurfaceKHR surface() const {return surface_;}

private:
    void createInstance();
    void setupDebugMessenger();
    void createSurface();

    bool checkValidationLayerSupport();
    std::vector<const char*> getRequiredExtensions();
    void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo);

    GLFWwindow* window_;
    VkInstance instance_;
    VkDebugUtilsMessengerEXT debugMessenger_;
    VkSurfaceKHR surface_;
};
}