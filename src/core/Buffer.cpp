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

    VmaAllocationCreateInfo allocCI{};
    allocCI.requiredFlags = memProps;

    if (vmaCreateBuffer(device.allocator(), &bufferInfo, &allocCI, &buffer_,
                        &allocation_, nullptr) != VK_SUCCESS)
        throw std::runtime_error("failed to create buffer with VMA!");
}

Buffer::~Buffer() {
    cleanup();
}

Buffer::Buffer(Buffer &&other) noexcept
    : device_(other.device_), buffer_(other.buffer_),
      allocation_(other.allocation_), size_(other.size_),
      mapped_(other.mapped_) {
    other.device_ = nullptr;
    other.buffer_ = VK_NULL_HANDLE;
    other.allocation_ = VK_NULL_HANDLE;
    other.size_ = 0;
    other.mapped_ = nullptr;
}

Buffer &Buffer::operator=(Buffer &&other) noexcept {
    if (this != &other) {
        cleanup();
        device_ = other.device_;
        buffer_ = other.buffer_;
        allocation_ = other.allocation_;
        size_ = other.size_;
        mapped_ = other.mapped_;
        other.device_ = nullptr;
        other.buffer_ = VK_NULL_HANDLE;
        other.allocation_ = VK_NULL_HANDLE;
        other.size_ = 0;
        other.mapped_ = nullptr;
    }
    return *this;
}

void *Buffer::map() {
    if (!mapped_) {
        vmaMapMemory(device_->allocator(), allocation_, &mapped_);
    }
    return mapped_;
}

void Buffer::unmap() {
    if (mapped_) {
        vmaUnmapMemory(device_->allocator(), allocation_);
        mapped_ = nullptr;
    }
}

void Buffer::cleanup() {
    if (device_ && buffer_ != VK_NULL_HANDLE) {
        if (mapped_)
            unmap();
        vmaDestroyBuffer(device_->allocator(), buffer_, allocation_);
        buffer_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
    }
}

} // namespace vkr
