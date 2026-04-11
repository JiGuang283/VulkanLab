#include "app.h"

// ---- 交换链方法已搬入 src/core/SwapChain.h/cpp ----

void HelloTriangleApplication::recreateSwapChain() {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(window, &width, &height);
        glfwWaitEvents();
    }

    vkDeviceWaitIdle(device->logicalDevice());

    cleanupSwapChain();

    swapChain_->recreate();
    createSwapChainSemaphores();
    createColorResources();
    createDepthResources();
    createFramebuffers();
}

void HelloTriangleApplication::cleanupSwapChain() {
    for (auto semaphore : renderFinishedSemaphores) {
        vkDestroySemaphore(device->logicalDevice(), semaphore, nullptr);
    }
    renderFinishedSemaphores.clear();

    for (auto framebuffer : swapChainFramebuffers) {
        vkDestroyFramebuffer(device->logicalDevice(), framebuffer, nullptr);
    }

    colorImage_.reset();

    depthImage_.reset();

    // ImageViews 和 SwapchainKHR 由 SwapChain::recreate() 内部清理
}
