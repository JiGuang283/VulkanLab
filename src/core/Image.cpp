#include "Image.h"
#include "Device.h"
#include "GpuDebugUtils.h"
#include "VulkanCheck.h"

#include <utility>

namespace vkr {

Image::Image(Device &device, uint32_t width, uint32_t height,
             uint32_t mipLevels, VkSampleCountFlagBits samples, VkFormat format,
             VkImageTiling tiling, VkImageUsageFlags usage,
             VkMemoryPropertyFlags memProps, std::string debugName)
    : device_(&device), debugName_(std::move(debugName)) {
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

    VmaAllocationCreateInfo allocCI{};
    allocCI.requiredFlags = memProps;

    VK_CHECK(vmaCreateImage(device.allocator(), &imageInfo, &allocCI, &image_,
                            &allocation_, nullptr));
    if (!debugName_.empty()) {
        device.debugUtils().setObjectName(VK_OBJECT_TYPE_IMAGE, image_,
                                          debugName_);
        vmaSetAllocationName(device.allocator(), allocation_,
                             debugName_.c_str());
    }
}

Image::~Image() {
    cleanup();
}

Image::Image(Image &&other) noexcept
    : device_(other.device_), image_(other.image_),
      allocation_(other.allocation_), view_(other.view_),
      debugName_(std::move(other.debugName_)) {
    other.device_ = nullptr;
    other.image_ = VK_NULL_HANDLE;
    other.allocation_ = VK_NULL_HANDLE;
    other.view_ = VK_NULL_HANDLE;
}

Image &Image::operator=(Image &&other) noexcept {
    if (this != &other) {
        cleanup();
        device_ = other.device_;
        image_ = other.image_;
        allocation_ = other.allocation_;
        view_ = other.view_;
        debugName_ = std::move(other.debugName_);
        other.device_ = nullptr;
        other.image_ = VK_NULL_HANDLE;
        other.allocation_ = VK_NULL_HANDLE;
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

    VK_CHECK(vkCreateImageView(device_->logicalDevice(), &viewInfo, nullptr,
                               &view_));
    if (!debugName_.empty()) {
        device_->debugUtils().setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, view_,
                                            debugName_ + "/View");
    }
}

void Image::cleanup() {
    if (device_) {
        VkDevice d = device_->logicalDevice();
        if (view_ != VK_NULL_HANDLE)
            vkDestroyImageView(d, view_, nullptr);
        if (image_ != VK_NULL_HANDLE)
            vmaDestroyImage(device_->allocator(), image_, allocation_);
        view_ = VK_NULL_HANDLE;
        image_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
    }
}

} // namespace vkr
