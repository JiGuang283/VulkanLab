#include "vulkan_vertex.h"
#include "app.h"

void HelloTriangleApplication::createVertexBuffer() {
    VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();

    vkr::Buffer staging(*device, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    void *data = staging.map();
    memcpy(data, vertices.data(), (size_t)bufferSize);
    staging.unmap();

    vertexBuffer_ = std::make_unique<vkr::Buffer>(
        *device, bufferSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    copyBuffer(staging.handle(), vertexBuffer_->handle(), bufferSize);
}

// uint32_t
// HelloTriangleApplication::findMemoryType(uint32_t typeFilter,
//                                          VkMemoryPropertyFlags properties) {
//     VkPhysicalDeviceMemoryProperties memProperties;
//     vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

//     for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
//         if (typeFilter & (1 << i) &&
//             (memProperties.memoryTypes[i].propertyFlags & properties) ==
//                 properties) {
//             return i;
//         }
//     }

//     throw std::runtime_error("failed to find suitable memory type!");
// }

VkCommandBuffer HelloTriangleApplication::beginSingleTimeCommands() {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(device->logicalDevice(), &allocInfo,
                             &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    return commandBuffer;
}

void HelloTriangleApplication::endSingleTimeCommands(
    VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(device->graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(device->graphicsQueue());

    vkFreeCommandBuffers(device->logicalDevice(), commandPool, 1,
                         &commandBuffer);
}

void HelloTriangleApplication::copyBuffer(VkBuffer     srcBuffer,
                                          VkBuffer     dstBuffer,
                                          VkDeviceSize size) {
    VkCommandBuffer commandBuffer = beginSingleTimeCommands();

    VkBufferCopy copyRegion{};
    copyRegion.srcOffset = 0;
    copyRegion.dstOffset = 0;
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

    endSingleTimeCommands(commandBuffer);
}

void HelloTriangleApplication::createIndexBuffer() {
    VkDeviceSize bufferSize = sizeof(indices[0]) * indices.size();

    vkr::Buffer staging(*device, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    void *data = staging.map();
    memcpy(data, indices.data(), (size_t)bufferSize);
    staging.unmap();

    indexBuffer_ = std::make_unique<vkr::Buffer>(
        *device, bufferSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    copyBuffer(staging.handle(), indexBuffer_->handle(), bufferSize);
}