#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

namespace vkr {

class GuiSystem;
class GpuDebugUtils;
class PipelineCache;
class TracyProfiler;
struct RenderView;
struct ShaderVariant;
struct GpuVisibilityDrawStream;

struct FrameRenderFeatures {
    bool surfaceDataRequired = false;
    bool hiZRequired = false;
    bool occlusionRequired = false;
    bool screenDepthPyramidRequired = false;
    bool sceneColorPyramidRequired = false;
    bool ssaoRequired = false;
    bool cacaoRequired = false;
};

struct RenderFrameContext {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint32_t        frameIndex = 0;
    uint32_t        imageIndex = 0;
    VkExtent2D      viewportExtent{};
    VkExtent2D      swapchainExtent{};
    VkDescriptorSet globalDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetLayout globalDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet lightingDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetLayout lightingDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet atmosphereDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetLayout atmosphereDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet screenSpaceDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetLayout screenSpaceDescriptorSetLayout = VK_NULL_HANDLE;
    PipelineCache  *pipelineCache = nullptr;
    const GpuDebugUtils *debugUtils = nullptr;
    const TracyProfiler *tracyProfiler = nullptr;
    GuiSystem      *gui = nullptr;
    const ShaderVariant *shaderVariant = nullptr;
    const RenderView *view = nullptr;
    bool environmentReady = false;
    bool atmosphereReady = false;
    FrameRenderFeatures features{};
    const GpuVisibilityDrawStream *visibilityDrawStream = nullptr;
};

} // namespace vkr
