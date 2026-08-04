#pragma once

#include <vulkan/vulkan.h>

namespace vkr {

inline void cmdImageBarrier(VkCommandBuffer commandBuffer,
                            VkPipelineStageFlags sourceStage,
                            VkPipelineStageFlags destinationStage,
                            VkAccessFlags sourceAccess,
                            VkAccessFlags destinationAccess, VkImage image,
                            VkImageLayout oldLayout,
                            VkImageLayout newLayout,
                            VkImageSubresourceRange range) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = range;
    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
}

inline void cmdBufferBarrier(VkCommandBuffer commandBuffer,
                             VkPipelineStageFlags sourceStage,
                             VkPipelineStageFlags destinationStage,
                             VkAccessFlags sourceAccess,
                             VkAccessFlags destinationAccess, VkBuffer buffer,
                             VkDeviceSize offset, VkDeviceSize size) {
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = offset;
    barrier.size = size;
    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0,
                         nullptr, 1, &barrier, 0, nullptr);
}

} // namespace vkr
