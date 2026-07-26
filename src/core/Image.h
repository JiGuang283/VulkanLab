#pragma once
#include "vk_mem_alloc.h"
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;

struct ImageCreateInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    VkImageUsageFlags usage = 0;
    VkMemoryPropertyFlags memoryProperties =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkImageCreateFlags flags = 0;
    std::string debugName;
};

class Image {
  public:
    Image(Device &device, uint32_t width, uint32_t height, uint32_t mipLevels,
          VkSampleCountFlagBits samples, VkFormat format, VkImageTiling tiling,
          VkImageUsageFlags usage, VkMemoryPropertyFlags memProps,
          std::string debugName = {});
    Image(Device &device, const ImageCreateInfo &info);
    ~Image();

    Image(const Image &) = delete;
    Image &operator=(const Image &) = delete;

    Image(Image &&other) noexcept;
    Image &operator=(Image &&other) noexcept;

    VkImage     handle() const { return image_; }
    VkImageView imageView() const { return view_; }

    void createView(VkFormat format, VkImageAspectFlags aspectFlags,
                    uint32_t mipLevels,
                    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D,
                    uint32_t arrayLayers = 1);

  private:
    void cleanup();

    Device       *device_ = nullptr;
    VkImage       image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView   view_ = VK_NULL_HANDLE;
    std::string   debugName_;
};

} // namespace vkr
