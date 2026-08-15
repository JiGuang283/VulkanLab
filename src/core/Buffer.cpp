#include "Buffer.h"
#include "Device.h"
#include "GpuDebugUtils.h"
#include "VulkanCheck.h"

#include <stdexcept>

namespace vkr {

Buffer::Buffer(Device &device, VkDeviceSize size, VkBufferUsageFlags usage,
               VkMemoryPropertyFlags memProps,
               VmaAllocationCreateFlags allocationFlags,
               std::string debugName)
    : device_(&device), size_(size) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocCI{};
    allocCI.requiredFlags = memProps;
    allocCI.flags = allocationFlags;

    VK_CHECK(vmaCreateBuffer(device.allocator(), &bufferInfo, &allocCI,
                             &buffer_, &allocation_, nullptr));
    if (!debugName.empty()) {
        device.debugUtils().setObjectName(VK_OBJECT_TYPE_BUFFER, buffer_,
                                          debugName);
        vmaSetAllocationName(device.allocator(), allocation_,
                             debugName.c_str());
    }
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
        VK_CHECK(vmaMapMemory(device_->allocator(), allocation_, &mapped_));
    }
    return mapped_;
}

VkDeviceAddress Buffer::deviceAddress() const {
    if (!device_ || buffer_ == VK_NULL_HANDLE)
        return 0;
    return device_->bufferDeviceAddress(buffer_);
}

void Buffer::invalidate(VkDeviceSize offset, VkDeviceSize size) {
    if (offset > size_)
        throw std::out_of_range("buffer invalidate range is out of bounds");
    if (size == VK_WHOLE_SIZE)
        size = size_ - offset;
    if (size > size_ - offset)
        throw std::out_of_range("buffer invalidate range is out of bounds");
    VK_CHECK(vmaInvalidateAllocation(device_->allocator(), allocation_, offset,
                                     size));
}

void Buffer::flush(VkDeviceSize offset, VkDeviceSize size) {
    if (offset > size_)
        throw std::out_of_range("buffer flush range is out of bounds");
    if (size == VK_WHOLE_SIZE)
        size = size_ - offset;
    if (size > size_ - offset)
        throw std::out_of_range("buffer flush range is out of bounds");
    VK_CHECK(vmaFlushAllocation(device_->allocator(), allocation_, offset,
                                size));
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
