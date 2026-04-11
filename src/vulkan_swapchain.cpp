#include "app.h"

// ---- Surface 创建 ----

// void HelloTriangleApplication::createSurface() {
//     if (glfwCreateWindowSurface(instance, window, nullptr, &surface) !=
//         VK_SUCCESS) {
//         throw std::runtime_error("failed to create window surface!");
//     }
// }

// ---- 交换链支持查询 ----

// SwapChainSupportDetails
// HelloTriangleApplication::querySwapChainSupport(VkPhysicalDevice device) {
//     SwapChainSupportDetails details;

//     vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, context->surface(),
//                                               &details.capabilities);

//     uint32_t formatCount;
//     vkGetPhysicalDeviceSurfaceFormatsKHR(device, context->surface(),
//     &formatCount,
//                                          nullptr);
//     if (formatCount != 0) {
//         details.formats.resize(formatCount);
//         vkGetPhysicalDeviceSurfaceFormatsKHR(device, context->surface(),
//         &formatCount,
//                                              details.formats.data());
//     }

//     uint32_t presentModeCount;
//     vkGetPhysicalDeviceSurfacePresentModesKHR(device, context->surface(),
//                                               &presentModeCount, nullptr);
//     if (presentModeCount != 0) {
//         details.presentModes.resize(presentModeCount);
//         vkGetPhysicalDeviceSurfacePresentModesKHR(
//             device, context->surface(), &presentModeCount,
//             details.presentModes.data());
//     }

//     return details;
// }

// ---- 交换链参数选择 ----

VkSurfaceFormatKHR HelloTriangleApplication::chooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR> &availableFormats) {
    for (const auto &availableFormat : availableFormats) {
        if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
            availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return availableFormat;
        }
    }

    return availableFormats[0];
}

VkPresentModeKHR HelloTriangleApplication::chooseSwapPresentMode(
    const std::vector<VkPresentModeKHR> &availablePresentModes) {
    for (const auto &availablePresentMode : availablePresentModes) {
        if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return availablePresentMode;
        }
    }

    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D HelloTriangleApplication::chooseSwapExtent(
    const VkSurfaceCapabilitiesKHR &capabilities) {
    if (capabilities.currentExtent.width !=
        std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    } else {
        int width, height;
        glfwGetFramebufferSize(window, &width, &height);

        VkExtent2D actualExtent = {static_cast<uint32_t>(width),
                                   static_cast<uint32_t>(height)};

        actualExtent.width =
            std::clamp(actualExtent.width, capabilities.minImageExtent.width,
                       capabilities.maxImageExtent.width);
        actualExtent.height =
            std::clamp(actualExtent.height, capabilities.minImageExtent.height,
                       capabilities.maxImageExtent.height);

        return actualExtent;
    }
}

// ---- 交换链创建 ----

void HelloTriangleApplication::createSwapChain() {
    SwapChainSupportDetails swapChainSupport = device->querySwapChainSupport();

    VkSurfaceFormatKHR surfaceFormat =
        chooseSwapSurfaceFormat(swapChainSupport.formats);
    VkPresentModeKHR presentMode =
        chooseSwapPresentMode(swapChainSupport.presentModes);
    VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities);

    uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
    if (swapChainSupport.capabilities.maxImageCount > 0 &&
        imageCount > swapChainSupport.capabilities.maxImageCount) {
        imageCount = swapChainSupport.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = context->surface();
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    QueueFamilyIndices indices = device->queueFamilies();
    uint32_t           queueFamilyIndices[] = {indices.graphicsFamily.value(),
                                               indices.presentFamily.value()};

    if (indices.graphicsFamily != indices.presentFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    if (vkCreateSwapchainKHR(device->logicalDevice(), &createInfo, nullptr,
                             &swapChain) != VK_SUCCESS) {
        throw std::runtime_error("failed to create swap chain!");
    }

    vkGetSwapchainImagesKHR(device->logicalDevice(), swapChain, &imageCount,
                            nullptr);
    swapChainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(device->logicalDevice(), swapChain, &imageCount,
                            swapChainImages.data());

    swapChainImageFormat = surfaceFormat.format;
    swapChainExtent = extent;
}

// ---- 图像视图创建 ----

void HelloTriangleApplication::createImageViews() {
    swapChainImageViews.resize(swapChainImages.size());

    for (size_t i = 0; i < swapChainImages.size(); i++) {
        swapChainImageViews[i] =
            createImageView(swapChainImages[i], swapChainImageFormat,
                            VK_IMAGE_ASPECT_COLOR_BIT, 1);
    }
}

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

    createSwapChain();
    createSwapChainSemaphores();
    createImageViews();
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

    for (auto imageView : swapChainImageViews) {
        vkDestroyImageView(device->logicalDevice(), imageView, nullptr);
    }

    vkDestroySwapchainKHR(device->logicalDevice(), swapChain, nullptr);
}
