#pragma once

#include <vector>
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
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = static_cast<VkPipelineStageFlags2>(sourceStage);
    barrier.srcAccessMask = static_cast<VkAccessFlags2>(sourceAccess);
    barrier.dstStageMask = static_cast<VkPipelineStageFlags2>(destinationStage);
    barrier.dstAccessMask = static_cast<VkAccessFlags2>(destinationAccess);
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = range;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

inline void cmdBufferBarrier(VkCommandBuffer commandBuffer,
                             VkPipelineStageFlags sourceStage,
                             VkPipelineStageFlags destinationStage,
                             VkAccessFlags sourceAccess,
                             VkAccessFlags destinationAccess, VkBuffer buffer,
                             VkDeviceSize offset, VkDeviceSize size) {
    VkBufferMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = static_cast<VkPipelineStageFlags2>(sourceStage);
    barrier.srcAccessMask = static_cast<VkAccessFlags2>(sourceAccess);
    barrier.dstStageMask = static_cast<VkPipelineStageFlags2>(destinationStage);
    barrier.dstAccessMask = static_cast<VkAccessFlags2>(destinationAccess);
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = offset;
    barrier.size = size;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

inline void cmdPipelineBarrier2Compat(
    VkCommandBuffer commandBuffer, VkPipelineStageFlags sourceStage,
    VkPipelineStageFlags destinationStage, VkDependencyFlags flags,
    uint32_t memoryBarrierCount, const VkMemoryBarrier *memoryBarriers,
    uint32_t bufferBarrierCount, const VkBufferMemoryBarrier *bufferBarriers,
    uint32_t imageBarrierCount, const VkImageMemoryBarrier *imageBarriers) {
    std::vector<VkMemoryBarrier2> memory2(memoryBarrierCount);
    for (uint32_t index = 0; index < memoryBarrierCount; ++index) {
        memory2[index] = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        memory2[index].srcStageMask = sourceStage;
        memory2[index].srcAccessMask = memoryBarriers[index].srcAccessMask;
        memory2[index].dstStageMask = destinationStage;
        memory2[index].dstAccessMask = memoryBarriers[index].dstAccessMask;
    }
    std::vector<VkBufferMemoryBarrier2> buffers2(bufferBarrierCount);
    for (uint32_t index = 0; index < bufferBarrierCount; ++index) {
        const VkBufferMemoryBarrier &source = bufferBarriers[index];
        VkBufferMemoryBarrier2 &target = buffers2[index];
        target = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        target.srcStageMask = sourceStage;
        target.srcAccessMask = source.srcAccessMask;
        target.dstStageMask = destinationStage;
        target.dstAccessMask = source.dstAccessMask;
        target.srcQueueFamilyIndex = source.srcQueueFamilyIndex;
        target.dstQueueFamilyIndex = source.dstQueueFamilyIndex;
        target.buffer = source.buffer;
        target.offset = source.offset;
        target.size = source.size;
    }
    std::vector<VkImageMemoryBarrier2> images2(imageBarrierCount);
    for (uint32_t index = 0; index < imageBarrierCount; ++index) {
        const VkImageMemoryBarrier &source = imageBarriers[index];
        VkImageMemoryBarrier2 &target = images2[index];
        target = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        target.srcStageMask = sourceStage;
        target.srcAccessMask = source.srcAccessMask;
        target.dstStageMask = destinationStage;
        target.dstAccessMask = source.dstAccessMask;
        target.oldLayout = source.oldLayout;
        target.newLayout = source.newLayout;
        target.srcQueueFamilyIndex = source.srcQueueFamilyIndex;
        target.dstQueueFamilyIndex = source.dstQueueFamilyIndex;
        target.image = source.image;
        target.subresourceRange = source.subresourceRange;
    }
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.dependencyFlags = flags;
    dependency.memoryBarrierCount = memoryBarrierCount;
    dependency.pMemoryBarriers = memory2.data();
    dependency.bufferMemoryBarrierCount = bufferBarrierCount;
    dependency.pBufferMemoryBarriers = buffers2.data();
    dependency.imageMemoryBarrierCount = imageBarrierCount;
    dependency.pImageMemoryBarriers = images2.data();
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

} // namespace vkr
