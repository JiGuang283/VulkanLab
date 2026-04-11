#include "Buffer.h"
#include "Device.h"
#include <stdexcept>

namespace vkr {

Buffer::Buffer(Device &device, VkDeviceSize size, VkBufferUsageFlags usage,
               VkMemoryPropertyFlags memProps)
    : device_(&device), size_(size) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device.logicalDevice(), &bufferInfo, nullptr,
                       &buffer_) != VK_SUCCESS)
        throw std::runtime_error("failed to create buffer!");

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device.logicalDevice(), buffer_, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex =
        device.findMemoryType(memReq.memoryTypeBits, memProps);

    if (vkAllocateMemory(device.logicalDevice(), &allocInfo, nullptr,
                         &memory_) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate buffer memory!");

    vkBindBufferMemory(device.logicalDevice(), buffer_, memory_, 0);
}

Buffer::~Buffer() {
    cleanup();
}

Buffer::Buffer(Buffer &&other) noexcept
    : device_(other.device_), buffer_(other.buffer_), memory_(other.memory_),
      size_(other.size_), mapped_(other.mapped_) {
    other.device_ = nullptr;
    other.buffer_ = VK_NULL_HANDLE;
    other.memory_ = VK_NULL_HANDLE;
    other.size_ = 0;
    other.mapped_ = nullptr;
}

Buffer &Buffer::operator=(Buffer &&other) noexcept {
    if (this != &other) {
        cleanup();
        device_ = other.device_;
        buffer_ = other.buffer_;
        memory_ = other.memory_;
        size_ = other.size_;
        mapped_ = other.mapped_;
        other.device_ = nullptr;
        other.buffer_ = VK_NULL_HANDLE;
        other.memory_ = VK_NULL_HANDLE;
        other.size_ = 0;
        other.mapped_ = nullptr;
    }
    return *this;
}

void *Buffer::map() {
    if (!mapped_) {
        vkMapMemory(device_->logicalDevice(), memory_, 0, size_, 0, &mapped_);
    }
    return mapped_;
}

void Buffer::unmap() {
    if (mapped_) {
        vkUnmapMemory(device_->logicalDevice(), memory_);
        mapped_ = nullptr;
    }
}

void Buffer::cleanup() {
    if (device_ && buffer_ != VK_NULL_HANDLE) {
        if (mapped_)
            unmap();
        vkDestroyBuffer(device_->logicalDevice(), buffer_, nullptr);
        vkFreeMemory(device_->logicalDevice(), memory_, nullptr);
        buffer_ = VK_NULL_HANDLE;
        memory_ = VK_NULL_HANDLE;
    }
}

} // namespace vkr
