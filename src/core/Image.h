#pragma once
#include <vulkan/vulkan.h>

namespace vkr {

class Device;

class Image {
  public:
    Image(Device &device, uint32_t width, uint32_t height, uint32_t mipLevels,
          VkSampleCountFlagBits samples, VkFormat format, VkImageTiling tiling,
          VkImageUsageFlags usage, VkMemoryPropertyFlags memProps);
    ~Image();

    Image(const Image &) = delete;
    Image &operator=(const Image &) = delete;

    Image(Image &&other) noexcept;
    Image &operator=(Image &&other) noexcept;

    VkImage        handle() const { return image_; }
    VkDeviceMemory memory() const { return memory_; }
    VkImageView    imageView() const { return view_; }

    void createView(VkFormat format, VkImageAspectFlags aspectFlags,
                    uint32_t mipLevels);

  private:
    void cleanup();

    Device        *device_ = nullptr;
    VkImage        image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView    view_ = VK_NULL_HANDLE;
};

} // namespace vkr
