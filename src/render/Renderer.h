#pragma once

#include "core/Buffer.h"
#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/SwapChain.h"
#include "render/RenderPipeline.h"
#include "render/GpuPassProfiler.h"
#include "render/RenderResourceRegistry.h"
#include "render/RendererShaderPaths.h"

#include <memory>
#include <array>
#include <deque>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
struct EnvironmentGpuResources;
class GuiSystem;
class MainForwardPass;
class ToneMapPass;
class PresentPass;
class PipelineCache;
class RenderQueue;
struct ShaderVariant;
struct RenderView;

struct RendererViewportOutput {
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSampler sampler = VK_NULL_HANDLE;
    std::array<VkImage, MAX_FRAMES_IN_FLIGHT> images{};
    std::array<VkImageView, MAX_FRAMES_IN_FLIGHT> imageViews{};
};

class Renderer {
  public:
    Renderer(Device &device, SwapChain &swapChain, FrameSync &frameSync,
             DescriptorAllocator &descriptorAllocator,
             RendererShaderPaths shaderPaths);
    ~Renderer();

    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    void renderFrame(const FrameSync::FrameContext &frame,
                     const RenderQueue &queue, PipelineCache &pipelineCache,
                     GuiSystem *gui, const ShaderVariant &shaderVariant,
                     const RenderView &view);

    // ---- 交换链重建 ----
    void recreateSwapChain();
    void resizeViewport(VkExtent2D extent);

    // ---- 访问器 ----
    VkRenderPass renderPass() const;
    VkExtent2D viewportExtent() const;
    RendererViewportOutput viewportOutput() const;
    const GpuPassTimings &gpuPassTimings() const;
    VkDescriptorSetLayout globalDescriptorSetLayout() const {
        return globalDescriptorSetLayout_;
    }
    VkDescriptorSetLayout lightingDescriptorSetLayout() const {
        return lightingDescriptorSetLayout_;
    }
    void publishEnvironment(
        std::shared_ptr<EnvironmentGpuResources> environment);
    void clearEnvironment();
    bool environmentReady() const;
    std::string currentEnvironmentId() const;
    float currentEnvironmentMaxSpecularLod() const;
    bool bloomSupported() const;
    const std::string &bloomUnsupportedReason() const;

    // ---- per-frame UBO 访问器 ----
  private:
    struct LightingDescriptorGeneration {
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sets{};
        std::shared_ptr<EnvironmentGpuResources> environment;
        uint64_t retireAfterSerial = 0;
    };

    void createUniformBuffers();
    void createGlobalDescriptorSetLayout();
    void createGlobalDescriptorSets();
    void createLightingDescriptorSetLayout();
    void createFallbackEnvironment();
    void createLightingGeneration(
        std::shared_ptr<EnvironmentGpuResources> environment);
    void collectRetiredLightingGenerations();
    void freeLightingGeneration(
        LightingDescriptorGeneration &generation);
    void createRenderPipeline();
    VkDescriptorSet globalDescriptorSet(uint32_t frameIndex) const;

    Device    *device_;
    SwapChain *swapChain_;
    FrameSync *frameSync_;
    DescriptorAllocator *descriptorAllocator_;

    std::vector<std::unique_ptr<Buffer>> uniformBuffers_;
    VkDeviceSize                         uniformBufferSize_ = 0;
    VkDescriptorSetLayout globalDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> globalDescriptorSets_;
    VkDescriptorSetLayout lightingDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::shared_ptr<EnvironmentGpuResources> fallbackEnvironment_;
    std::unique_ptr<LightingDescriptorGeneration>
        currentLightingGeneration_;
    std::deque<LightingDescriptorGeneration> retiredLightingGenerations_;
    std::unique_ptr<RenderResourceRegistry> renderResources_;
    RendererResourceHandles resourceHandles_{};
    RendererShaderPaths shaderPaths_;
    RenderPipeline pipeline_;
    std::unique_ptr<GpuPassProfiler> gpuPassProfiler_;
    MainForwardPass *mainForwardPass_ = nullptr;
    ToneMapPass *toneMapPass_ = nullptr;
    PresentPass *presentPass_ = nullptr;
};

} // namespace vkr
