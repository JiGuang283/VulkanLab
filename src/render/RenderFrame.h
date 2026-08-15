#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vulkan/vulkan.h>

namespace vkr {

class GuiSystem;
class GpuDebugUtils;
class PipelineCache;
class MaterialSystem;
class TracyProfiler;
struct RenderView;
struct ShaderVariant;
struct GpuVisibilityDrawStream;

enum class FrameCaptureSource { Viewport, Workspace, Hdr };

struct FrameRenderFeatures {
    bool atmosphereRequired = false;
    bool directionalShadowRequired = false;
    bool pointShadowRequired = false;
    bool spotShadowRequired = false;
    uint32_t directionalShadowCascadeCount = 0;
    uint32_t pointShadowLightCount = 0;
    uint32_t spotShadowLightCount = 0;
    bool surfaceDataRequired = false;
    bool hiZRequired = false;
    bool occlusionRequired = false;
    bool screenDepthPyramidRequired = false;
    bool sceneColorPyramidRequired = false;
    bool ssaoRequired = false;
    bool ssaoActive = false;
    bool cacaoRequired = false;
    bool cacaoActive = false;
    bool gtaoRequired = false;
    bool gtaoActive = false;
    bool taaRequired = false;
    bool taaActive = false;
    bool ssrRequired = false;
    bool ssrActive = false;
    bool ssgiRequired = false;
    bool ssgiActive = false;
    bool ddgiRequired = false;
    bool ddgiActive = false;
    bool bloomRequired = false;
    bool captureRequired = false;
    std::optional<FrameCaptureSource> captureSource;
};

struct RenderFrameContext {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint32_t        frameIndex = 0;
    uint32_t        imageIndex = 0;
    VkImage         swapchainImage = VK_NULL_HANDLE;
    VkImageView     swapchainImageView = VK_NULL_HANDLE;
    uint64_t        submissionSerial = 0;
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
    VkDescriptorSet ddgiDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetLayout ddgiDescriptorSetLayout = VK_NULL_HANDLE;
    PipelineCache  *pipelineCache = nullptr;
    MaterialSystem *materialSystem = nullptr;
    const GpuDebugUtils *debugUtils = nullptr;
    const TracyProfiler *tracyProfiler = nullptr;
    GuiSystem      *gui = nullptr;
    const ShaderVariant *shaderVariant = nullptr;
    const RenderView *view = nullptr;
    bool environmentReady = false;
    bool atmosphereReady = false;
    FrameRenderFeatures features{};
    const GpuVisibilityDrawStream *visibilityDrawStream = nullptr;
    std::function<void(VkCommandBuffer)> screenshotCopy;
};

} // namespace vkr
