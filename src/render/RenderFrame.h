#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

namespace vkr {

class GuiSystem;
class PipelineCache;

struct RenderFrameContext {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint32_t        frameIndex = 0;
    uint32_t        imageIndex = 0;
    VkExtent2D      extent{};
    VkDescriptorSet globalDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetLayout globalDescriptorSetLayout = VK_NULL_HANDLE;
    PipelineCache  *pipelineCache = nullptr;
    GuiSystem      *gui = nullptr;
};

} // namespace vkr
