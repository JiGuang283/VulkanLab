#include "UploadContext.h"

#include "Buffer.h"
#include "Device.h"
#include "VulkanCheck.h"
#include "diagnostics/SceneLoadStats.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace vkr {

UploadContext::UploadContext(Device &device, ResourceLoadStats *stats,
                             VkDeviceSize stagingCapacity)
    : device_(&device), stats_(stats), defaultCapacity_(stagingCapacity) {
    if (defaultCapacity_ == 0)
        throw std::invalid_argument("UploadContext staging capacity is zero");

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.physicalDevice(), &properties);
    copyAlignment_ = std::max<VkDeviceSize>(
        4, properties.limits.optimalBufferCopyOffsetAlignment);

    const QueueFamilyIndices families = device.queueFamilies();
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                     VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = families.graphicsFamily.value();
    VK_CHECK(vkCreateCommandPool(device.logicalDevice(), &poolInfo, nullptr,
                                 &commandPool_));

    VkCommandBufferAllocateInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = commandPool_;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device.logicalDevice(), &commandInfo,
                                      &commandBuffer_));

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VK_CHECK(vkCreateFence(device.logicalDevice(), &fenceInfo, nullptr,
                           &fence_));
}

UploadContext::~UploadContext() {
    if (!device_)
        return;

    if (staging_ && mapped_) {
        staging_->unmap();
        mapped_ = nullptr;
    }
    staging_.reset();

    VkDevice device = device_->logicalDevice();
    if (fence_ != VK_NULL_HANDLE)
        vkDestroyFence(device, fence_, nullptr);
    if (commandPool_ != VK_NULL_HANDLE)
        vkDestroyCommandPool(device, commandPool_, nullptr);
}

VkDeviceSize UploadContext::alignUp(VkDeviceSize value,
                                    VkDeviceSize alignment) {
    const VkDeviceSize remainder = value % alignment;
    if (remainder == 0)
        return value;
    const VkDeviceSize increment = alignment - remainder;
    if (value > std::numeric_limits<VkDeviceSize>::max() - increment)
        throw std::overflow_error("UploadContext staging offset overflow");
    return value + increment;
}

void UploadContext::ensureStaging(VkDeviceSize capacity, bool allowShrink) {
    if (staging_ &&
        ((!allowShrink && stagingCapacity_ >= capacity) ||
         (allowShrink && stagingCapacity_ == capacity))) {
        return;
    }
    if (recording_ || hasCommands_ || cursor_ != 0)
        throw std::logic_error("Cannot replace an active staging buffer");

    if (staging_ && mapped_) {
        staging_->unmap();
        mapped_ = nullptr;
    }
    staging_.reset();
    staging_ = std::make_unique<Buffer>(
        *device_, capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);
    mapped_ = staging_->map();
    stagingCapacity_ = capacity;
}

StagedSlice UploadContext::stageBytes(const void *data, VkDeviceSize size) {
    if (!data || size == 0)
        throw std::invalid_argument("UploadContext cannot stage empty data");

    if (oversizedBatch_) {
        flushAndWait();
        ensureStaging(defaultCapacity_, true);
    }

    if (size > defaultCapacity_) {
        flushAndWait();
        ensureStaging(alignUp(size, copyAlignment_), true);
        oversizedBatch_ = true;
    } else {
        if (!staging_) {
            const VkDeviceSize initialCapacity = std::min(
                defaultCapacity_,
                std::max(kInitialStagingCapacity,
                         alignUp(size, copyAlignment_)));
            ensureStaging(initialCapacity);
        } else if (stagingCapacity_ > defaultCapacity_) {
            ensureStaging(defaultCapacity_, true);
        }

        VkDeviceSize offset = alignUp(cursor_, copyAlignment_);
        if (offset > stagingCapacity_ || size > stagingCapacity_ - offset) {
            const VkDeviceSize previousCapacity = stagingCapacity_;
            flushAndWait();
            const VkDeviceSize grownCapacity =
                previousCapacity >= defaultCapacity_ / 2
                    ? defaultCapacity_
                    : previousCapacity * 2;
            const VkDeviceSize requiredCapacity =
                alignUp(size, copyAlignment_);
            const VkDeviceSize nextCapacity = std::min(
                defaultCapacity_,
                std::max(grownCapacity, requiredCapacity));
            ensureStaging(nextCapacity, nextCapacity != stagingCapacity_);
        }
    }

    const VkDeviceSize offset = alignUp(cursor_, copyAlignment_);
    if (offset > stagingCapacity_ || size > stagingCapacity_ - offset)
        throw std::runtime_error("UploadContext staging allocation failed");

    std::memcpy(static_cast<uint8_t *>(mapped_) + offset, data,
                static_cast<size_t>(size));
    cursor_ = offset + size;
    if (stats_)
        stats_->peakStagingBytes = std::max<uint64_t>(
            stats_->peakStagingBytes, static_cast<uint64_t>(cursor_));
    return {staging_->handle(), offset, size};
}

void UploadContext::uploadBuffer(const void *data, VkDeviceSize size,
                                 VkBuffer dst, VkDeviceSize dstOffset) {
    const StagedSlice staged = stageBytes(data, size);
    VkBufferCopy copy{};
    copy.srcOffset = staged.offset;
    copy.dstOffset = dstOffset;
    copy.size = staged.size;
    vkCmdCopyBuffer(commandBuffer(), staged.buffer, dst, 1, &copy);
}

VkCommandBuffer UploadContext::commandBuffer() {
    beginCommands();
    hasCommands_ = true;
    return commandBuffer_;
}

void UploadContext::beginCommands() {
    if (recording_)
        return;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(commandBuffer_, &beginInfo));
    recording_ = true;
}

void UploadContext::flushAndWait() {
    if (!hasCommands_) {
        cursor_ = 0;
        oversizedBatch_ = false;
        return;
    }

    VK_CHECK(vkEndCommandBuffer(commandBuffer_));
    recording_ = false;

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer_;

    ScopedLoadTimer waitTimer(stats_ ? &stats_->batchSubmitWaitMs : nullptr);
    VK_CHECK(vkQueueSubmit(device_->graphicsQueue(), 1, &submitInfo, fence_));
    if (stats_)
        ++stats_->batchSubmits;
    VK_CHECK(vkWaitForFences(device_->logicalDevice(), 1, &fence_, VK_TRUE,
                             UINT64_MAX));
    if (stats_)
        ++stats_->fenceWaitCalls;

    VK_CHECK(vkResetFences(device_->logicalDevice(), 1, &fence_));
    VK_CHECK(vkResetCommandPool(device_->logicalDevice(), commandPool_, 0));
    hasCommands_ = false;
    cursor_ = 0;
    oversizedBatch_ = false;
}

void UploadContext::finish() {
    flushAndWait();
}

} // namespace vkr
