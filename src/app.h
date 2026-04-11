#pragma once

#include "vk_mem_alloc.h"
#include "vulkan_utils.h"

#include "core/Buffer.h"
#include "core/Device.h"
#include "core/Image.h"
#include "core/SwapChain.h"
#include "core/VulkanContext.h"

class HelloTriangleApplication {
  public:
    void run();

  private:
    // ---- 窗口 ----
    GLFWwindow *window;

    // ---- Vulkan 核心对象 ----
    // VkInstance instance;
    // VkDebugUtilsMessengerEXT debugMessenger;
    // VkSurfaceKHR surface;
    std::unique_ptr<vkr::VulkanContext> context;

    // ---- 设备 ----
    // VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    // VkDevice device;
    // VkQueue graphicsQueue;
    // VkQueue presentQueue;
    std::unique_ptr<vkr::Device> device;

    // ---- 交换链 ----
    std::unique_ptr<vkr::SwapChain> swapChain_;

    // ---- 图形管线 ----
    VkDescriptorSetLayout descriptorSetLayout;
    VkPipelineLayout      pipelineLayout;
    VkRenderPass          renderPass;
    VkPipeline            graphicsPipeline;

    // ---- 帧缓冲 ----
    std::vector<VkFramebuffer>   swapChainFramebuffers;
    VkCommandPool                commandPool;
    std::vector<VkCommandBuffer> commandBuffers;

    // ---- 同步 ----
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence>     inFlightFences;

    bool framebufferResized = false;

    uint32_t currentFrame = 0;

    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;

    std::unique_ptr<vkr::Buffer> vertexBuffer_;
    std::unique_ptr<vkr::Buffer> indexBuffer_;

    std::vector<std::unique_ptr<vkr::Buffer>> uniformBuffers_;

    VkDescriptorPool             descriptorPool;
    std::vector<VkDescriptorSet> descriptorSets;

    std::unique_ptr<vkr::Image> textureImage_;
    VkSampler                   textureSampler;

    std::unique_ptr<vkr::Image> depthImage_;

    uint32_t mipLevels;

    // VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    std::unique_ptr<vkr::Image> colorImage_;

    VmaAllocator allocator;

    // ---- 应用主框架 (app.cpp) ----
    void        initWindow();
    void        initVulkan();
    void        mainLoop();
    void        cleanup();
    static void framebufferResizeCallback(GLFWwindow *window, int width,
                                          int height);

    // ---- 实例与调试 (vulkan_instance.cpp) ----
    // void createInstance();
    // void setupDebugMessenger();
    // void populateDebugMessengerCreateInfo(
    //     VkDebugUtilsMessengerCreateInfoEXT &createInfo);
    // bool checkValidationLayerSupport();
    // std::vector<const char *> getRequiredExtensions();
    // static VKAPI_ATTR VkBool32 VKAPI_CALL
    // debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    //               VkDebugUtilsMessageTypeFlagsEXT messageType,
    //               const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
    //               void *pUserData);

    // ---- 设备管理 (vulkan_device.cpp) ----
    // void pickPhysicalDevice();
    // void createLogicalDevice();
    // bool isDeviceSuitable(VkPhysicalDevice device);
    // bool checkDeviceExtensionSupport(VkPhysicalDevice device);
    // QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);

    // ---- 交换链 (vulkan_swapchain.cpp) ----

    // ---- 图形管线 (vulkan_pipeline.cpp) ----
    void           createGraphicsPipeline();
    VkShaderModule createShaderModule(const std::vector<char> code);
    void           createRenderPass();

    // ----帧缓冲----
    void createFramebuffers();
    void createCommandPool();
    void createCommandBuffers();
    void recordCommandBuffer(VkCommandBuffer commandBuffer,
                             uint32_t        imageIndex);

    void drawFrame();
    void createSyncObjects();
    void createSwapChainSemaphores();

    void recreateSwapChain();
    void cleanupSwapChain();

    void createVertexBuffer();
    void createIndexBuffer();
    // uint32_t findMemoryType(uint32_t typeFilter,
    //                         VkMemoryPropertyFlags properties);

    VkCommandBuffer beginSingleTimeCommands();
    void            endSingleTimeCommands(VkCommandBuffer commandBuffer);
    void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);

    void createDescriptorSetLayout();
    void createUniformBuffers();
    void updateUniformBuffer(uint32_t currentImage);

    // 创建描述符池和描述符集
    void createDescriptionPool();
    void createDescriptorSets();

    void createTextureImage();
    void createTextureImageView();

    void transitionImageLayout(VkImage image, VkFormat format,
                               VkImageLayout oldLayout, VkImageLayout newLayout,
                               uint32_t mipLevels);
    void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width,
                           uint32_t height);

    void createTextureSampler();
    void createDepthResources();

    VkFormat findSuportedFormat(const std::vector<VkFormat> &candidates,
                                VkImageTiling                tiling,
                                VkFormatFeatureFlags         features);
    VkFormat findDepthFormat();

    void loadModel();
    void generateMipmaps(VkImage image, VkFormat imageFormat, int32_t texWidth,
                         int32_t texHeight, uint32_t mipLevels);

    // VkSampleCountFlagBits getMaxUsableSampleCount();
    void createColorResources();

    void createAllocator();
};
