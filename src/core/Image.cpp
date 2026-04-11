#include "Image.h"
#include "Device.h"
#include <stdexcept>

namespace vkr {

Image::Image(Device &device, uint32_t width, uint32_t height,
             uint32_t mipLevels, VkSampleCountFlagBits samples, VkFormat format,
             VkImageTiling tiling, VkImageUsageFlags usage,
             VkMemoryPropertyFlags memProps)
    : device_(&device) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = samples;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device.logicalDevice(), &imageInfo, nullptr, &image_) !=
        VK_SUCCESS)
        throw std::runtime_error("failed to create image!");

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device.logicalDevice(), image_, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex =
        device.findMemoryType(memReq.memoryTypeBits, memProps);

    if (vkAllocateMemory(device.logicalDevice(), &allocInfo, nullptr,
                         &memory_) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate image memory!");

    vkBindImageMemory(device.logicalDevice(), image_, memory_, 0);
}

Image::~Image() {
    cleanup();
}

Image::Image(Image &&other) noexcept
    : device_(other.device_), image_(other.image_), memory_(other.memory_),
      view_(other.view_) {
    other.device_ = nullptr;
    other.image_ = VK_NULL_HANDLE;
    other.memory_ = VK_NULL_HANDLE;
    other.view_ = VK_NULL_HANDLE;
}

Image &Image::operator=(Image &&other) noexcept {
    if (this != &other) {
        cleanup();
        device_ = other.device_;
        image_ = other.image_;
        memory_ = other.memory_;
        view_ = other.view_;
        other.device_ = nullptr;
        other.image_ = VK_NULL_HANDLE;
        other.memory_ = VK_NULL_HANDLE;
        other.view_ = VK_NULL_HANDLE;
    }
    return *this;
}

void Image::createView(VkFormat format, VkImageAspectFlags aspectFlags,
                       uint32_t mipLevels) {
    if (view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_->logicalDevice(), view_, nullptr);
        view_ = VK_NULL_HANDLE;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_->logicalDevice(), &viewInfo, nullptr,
                          &view_) != VK_SUCCESS)
        throw std::runtime_error("failed to create image view!");
}

void Image::cleanup() {
    if (device_) {
        VkDevice d = device_->logicalDevice();
        if (view_ != VK_NULL_HANDLE)
            vkDestroyImageView(d, view_, nullptr);
        if (image_ != VK_NULL_HANDLE)
            vkDestroyImage(d, image_, nullptr);
        if (memory_ != VK_NULL_HANDLE)
            vkFreeMemory(d, memory_, nullptr);
        view_ = VK_NULL_HANDLE;
        image_ = VK_NULL_HANDLE;
        memory_ = VK_NULL_HANDLE;
    }
}

} // namespace vkr
