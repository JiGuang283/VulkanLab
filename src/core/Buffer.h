#pragma once
#include "vk_mem_alloc.h"
#include <vulkan/vulkan.h>

namespace vkr {

class Device;

class Buffer {
  public:
    Buffer(Device &device, VkDeviceSize size, VkBufferUsageFlags usage,
           VkMemoryPropertyFlags memProps,
           VmaAllocationCreateFlags allocationFlags = 0);
    ~Buffer();

    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;

    Buffer(Buffer &&other) noexcept;
    Buffer &operator=(Buffer &&other) noexcept;

    VkBuffer     handle() const { return buffer_; }
    VkDeviceSize size() const { return size_; }

    void *map();
    void  unmap();
    void invalidate(VkDeviceSize offset = 0,
                    VkDeviceSize size = VK_WHOLE_SIZE);
    void *mappedData() const { return mapped_; }

  private:
    void cleanup();

    Device       *device_ = nullptr;
    VkBuffer      buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkDeviceSize  size_ = 0;
    void         *mapped_ = nullptr;
};

} // namespace vkr
