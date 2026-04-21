#include "SwapChain.h"
#include "Device.h"
#include "VulkanCheck.h"

#include <algorithm>
#include <limits>

namespace vkr {

SwapChain::SwapChain(Device &device, VkSurfaceKHR surface,
                     ExtentProvider getExtent)
    : device_(&device), surface_(surface), getExtent_(std::move(getExtent)) {
    createSwapChain();
    createImageViews();
}

SwapChain::~SwapChain() {
    cleanup();
}

void SwapChain::recreate() {
    cleanup();
    createSwapChain();
    createImageViews();
}

// ---- 交换链参数选择 ----

VkSurfaceFormatKHR SwapChain::chooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR> &availableFormats) {
    for (const auto &availableFormat : availableFormats) {
        if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
            availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return availableFormat;
        }
    }

    return availableFormats[0];
}

VkPresentModeKHR SwapChain::chooseSwapPresentMode(
    const std::vector<VkPresentModeKHR> &availablePresentModes) {
    for (const auto &availablePresentMode : availablePresentModes) {
        if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return availablePresentMode;
        }
    }

    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D
SwapChain::chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities) {
    if (capabilities.currentExtent.width !=
        std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    } else {
        VkExtent2D actualExtent = getExtent_();

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

void SwapChain::createSwapChain() {
    SwapChainSupportDetails swapChainSupport = device_->querySwapChainSupport();

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
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    QueueFamilyIndices indices = device_->queueFamilies();
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

    VK_CHECK(vkCreateSwapchainKHR(device_->logicalDevice(), &createInfo,
                                  nullptr, &swapChain_));

    vkGetSwapchainImagesKHR(device_->logicalDevice(), swapChain_, &imageCount,
                            nullptr);
    images_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_->logicalDevice(), swapChain_, &imageCount,
                            images_.data());

    imageFormat_ = surfaceFormat.format;
    extent_ = extent;
}

// ---- 图像视图创建 ----

void SwapChain::createImageViews() {
    imageViews_.resize(images_.size());

    for (size_t i = 0; i < images_.size(); i++) {
        imageViews_[i] = createImageView(images_[i], imageFormat_,
                                         VK_IMAGE_ASPECT_COLOR_BIT, 1);
    }
}

VkImageView SwapChain::createImageView(VkImage image, VkFormat format,
                                       VkImageAspectFlags aspectFlags,
                                       uint32_t           mipLevels) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    viewInfo.subresourceRange.levelCount = mipLevels;

    VkImageView imageView;
    VK_CHECK(vkCreateImageView(device_->logicalDevice(), &viewInfo, nullptr,
                               &imageView));

    return imageView;
}

// ---- 清理 ----

void SwapChain::cleanup() {
    VkDevice d = device_->logicalDevice();
    for (auto view : imageViews_) {
        vkDestroyImageView(d, view, nullptr);
    }
    imageViews_.clear();
    images_.clear();

    if (swapChain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(d, swapChain_, nullptr);
        swapChain_ = VK_NULL_HANDLE;
    }
}

} // namespace vkr
