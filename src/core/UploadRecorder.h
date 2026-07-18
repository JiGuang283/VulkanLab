#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

namespace vkr {

struct ResourceLoadStats;

struct StagedSlice {
    VkBuffer     buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
};

class UploadRecorder {
  public:
    virtual ~UploadRecorder() = default;

    virtual StagedSlice stageBytes(const void *data, VkDeviceSize size) = 0;
    virtual VkCommandBuffer commandBuffer() = 0;
    virtual ResourceLoadStats *stats() const = 0;

    void uploadBuffer(const void *data, VkDeviceSize size, VkBuffer dst,
                      VkDeviceSize dstOffset = 0) {
        const StagedSlice staged = stageBytes(data, size);
        VkBufferCopy copy{};
        copy.srcOffset = staged.offset;
        copy.dstOffset = dstOffset;
        copy.size = staged.size;
        vkCmdCopyBuffer(commandBuffer(), staged.buffer, dst, 1, &copy);
    }
};

} // namespace vkr
