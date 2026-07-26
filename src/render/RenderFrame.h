#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

namespace vkr {

class GuiSystem;
class GpuDebugUtils;
class PipelineCache;
struct RenderView;
struct ShaderVariant;

struct RenderFrameContext {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint32_t        frameIndex = 0;
    uint32_t        imageIndex = 0;
    VkExtent2D      extent{};
    VkDescriptorSet globalDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetLayout globalDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet lightingDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetLayout lightingDescriptorSetLayout = VK_NULL_HANDLE;
    PipelineCache  *pipelineCache = nullptr;
    const GpuDebugUtils *debugUtils = nullptr;
    GuiSystem      *gui = nullptr;
    const ShaderVariant *shaderVariant = nullptr;
    const RenderView *view = nullptr;
    bool environmentReady = false;
};

} // namespace vkr
