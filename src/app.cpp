#include "app.h"

// ---- 应用入口 ----

void HelloTriangleApplication::run() {
    initWindow();
    initVulkan();
    mainLoop();
    cleanup();
}

// ---- 窗口初始化 ----

void HelloTriangleApplication::initWindow() {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GL_FALSE);

    window = glfwCreateWindow(WIDTH, HEIGHT, "Vulkan", nullptr, nullptr);

    glfwSetWindowUserPointer(window, this);
    glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);
}

// ---- Vulkan 初始化 ----

void HelloTriangleApplication::initVulkan() {
    // createInstance();
    // setupDebugMessenger();
    // createSurface();
    context = std::make_unique<vkr::VulkanContext>(window);
    // pickPhysicalDevice();
    // createLogicalDevice();
    device = std::make_unique<vkr::Device>(*context);
    createAllocator();
    createSwapChain();
    createImageViews();
    createRenderPass();
    createDescriptorSetLayout();
    createGraphicsPipeline();
    createCommandPool();
    createColorResources();
    createDepthResources();
    createFramebuffers();
    createTextureImage();
    createTextureImageView();
    createTextureSampler();
    loadModel();
    createVertexBuffer();
    createIndexBuffer();
    createUniformBuffers();
    createDescriptionPool();
    createDescriptorSets();
    createCommandBuffers();
    createSyncObjects();
}

// ---- 主循环 ----

void HelloTriangleApplication::mainLoop() {
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        drawFrame();
    }

    vkDeviceWaitIdle(device->logicalDevice());
}

// ---- 资源清理 ----

void HelloTriangleApplication::cleanup() {
    cleanupSwapChain();

    VkDevice d = device->logicalDevice();

    uniformBuffers_.clear();

    vkDestroyDescriptorPool(d, descriptorPool, nullptr);

    vkDestroySampler(d, textureSampler, nullptr);

    textureImage_.reset();

    vkDestroyDescriptorSetLayout(d, descriptorSetLayout, nullptr);

    indexBuffer_.reset();

    vertexBuffer_.reset();

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(d, imageAvailableSemaphores[i], nullptr);
        vkDestroyFence(d, inFlightFences[i], nullptr);
    }

    vkDestroyPipeline(d, graphicsPipeline, nullptr);
    vkDestroyPipelineLayout(d, pipelineLayout, nullptr);
    vkDestroyRenderPass(d, renderPass, nullptr);
    vkDestroyCommandPool(d, commandPool, nullptr);

    vmaDestroyAllocator(allocator);

    device.reset();  // ~Device() 销毁 VkDevice
    context.reset(); // ~VulkanContext() 销毁 Surface/Instance

    glfwDestroyWindow(window);

    glfwTerminate();
}

void HelloTriangleApplication::framebufferResizeCallback(GLFWwindow *window,
                                                         int         width,
                                                         int         height) {
    auto app = reinterpret_cast<HelloTriangleApplication *>(
        glfwGetWindowUserPointer(window));
    app->framebufferResized = true;
}
