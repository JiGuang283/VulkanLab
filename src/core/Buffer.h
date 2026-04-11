#pragma once
#include <vulkan/vulkan.h>

namespace vkr {

class Device;

class Buffer {
  public:
    Buffer(Device &device, VkDeviceSize size, VkBufferUsageFlags usage,
           VkMemoryPropertyFlags memProps);
    ~Buffer();

    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;

    Buffer(Buffer &&other) noexcept;
    Buffer &operator=(Buffer &&other) noexcept;

    VkBuffer       handle() const { return buffer_; }
    VkDeviceMemory memory() const { return memory_; }
    VkDeviceSize   size() const { return size_; }

    void *map();
    void  unmap();
    void *mappedData() const { return mapped_; }

  private:
    void cleanup();

    Device        *device_ = nullptr;
    VkBuffer       buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize   size_ = 0;
    void          *mapped_ = nullptr;
};

} // namespace vkr
