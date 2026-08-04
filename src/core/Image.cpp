#include "Image.h"
#include "Device.h"
#include "GpuDebugUtils.h"
#include "VulkanCheck.h"

#include <stdexcept>
#include <utility>

namespace vkr {

Image::Image(Device &device, uint32_t width, uint32_t height,
             uint32_t mipLevels, VkSampleCountFlagBits samples, VkFormat format,
             VkImageTiling tiling, VkImageUsageFlags usage,
             VkMemoryPropertyFlags memProps, std::string debugName)
    : Image(device,
            ImageCreateInfo{width, height, mipLevels, 1, samples, format,
                            tiling, usage, memProps, 0,
                            std::move(debugName)}) {}

Image::Image(Device &device, const ImageCreateInfo &info)
    : device_(&device), debugName_(info.debugName) {
    if (info.width == 0 || info.height == 0 || info.mipLevels == 0 ||
        info.arrayLayers == 0 || info.format == VK_FORMAT_UNDEFINED ||
        info.usage == 0) {
        throw std::invalid_argument("invalid image create info");
    }
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = info.flags;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {info.width, info.height, 1};
    imageInfo.mipLevels = info.mipLevels;
    imageInfo.arrayLayers = info.arrayLayers;
    imageInfo.format = info.format;
    imageInfo.tiling = info.tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = info.usage;
    imageInfo.samples = info.samples;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocCI{};
    allocCI.requiredFlags = info.memoryProperties;

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
      mipViews_(std::move(other.mipViews_)),
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
        mipViews_ = std::move(other.mipViews_);
        debugName_ = std::move(other.debugName_);
        other.device_ = nullptr;
        other.image_ = VK_NULL_HANDLE;
        other.allocation_ = VK_NULL_HANDLE;
        other.view_ = VK_NULL_HANDLE;
    }
    return *this;
}

void Image::createView(VkFormat format, VkImageAspectFlags aspectFlags,
                       uint32_t mipLevels, VkImageViewType viewType,
                       uint32_t arrayLayers) {
    if (view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_->logicalDevice(), view_, nullptr);
        view_ = VK_NULL_HANDLE;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image_;
    viewInfo.viewType = viewType;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = arrayLayers;

    VK_CHECK(vkCreateImageView(device_->logicalDevice(), &viewInfo, nullptr,
                               &view_));
    if (!debugName_.empty()) {
        device_->debugUtils().setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, view_,
                                            debugName_ + "/View");
    }
}

void Image::createMipViews(VkFormat format,
                           VkImageAspectFlags aspectFlags,
                           uint32_t mipLevels, VkImageViewType viewType,
                           uint32_t arrayLayers) {
    for (VkImageView mipView : mipViews_)
        vkDestroyImageView(device_->logicalDevice(), mipView, nullptr);
    mipViews_.clear();
    mipViews_.reserve(mipLevels);
    for (uint32_t mip = 0; mip < mipLevels; ++mip) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image_;
        viewInfo.viewType = viewType;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = aspectFlags;
        viewInfo.subresourceRange.baseMipLevel = mip;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = arrayLayers;
        VkImageView mipView = VK_NULL_HANDLE;
        VK_CHECK(vkCreateImageView(device_->logicalDevice(), &viewInfo,
                                   nullptr, &mipView));
        if (!debugName_.empty()) {
            device_->debugUtils().setObjectName(
                VK_OBJECT_TYPE_IMAGE_VIEW, mipView,
                debugName_ + "/Mip" + std::to_string(mip));
        }
        mipViews_.push_back(mipView);
    }
}

VkImageView Image::mipView(uint32_t mipLevel) const {
    if (mipLevel >= mipViews_.size())
        throw std::out_of_range("image mip view index out of range");
    return mipViews_[mipLevel];
}

void Image::cleanup() {
    if (device_) {
        VkDevice d = device_->logicalDevice();
        for (VkImageView mipView : mipViews_)
            vkDestroyImageView(d, mipView, nullptr);
        mipViews_.clear();
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
