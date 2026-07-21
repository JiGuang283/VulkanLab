#pragma once

#include "core/Buffer.h"
#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/SwapChain.h"
#include "render/RenderPipeline.h"
#include "render/RendererShaderPaths.h"

#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
class GuiSystem;
class FrameRenderTargets;
class MainForwardPass;
class ToneMapPass;
class PipelineCache;
class RenderQueue;
struct ShaderVariant;
struct DirectionalShadowFrameData;
struct RenderSettings;

class Renderer {
  public:
    Renderer(Device &device, SwapChain &swapChain, FrameSync &frameSync,
             DescriptorAllocator &descriptorAllocator,
             VkDeviceSize uniformBufferSize,
             RendererShaderPaths shaderPaths);
    ~Renderer();

    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    void renderFrame(const FrameSync::FrameContext &frame,
                     const RenderQueue &queue, PipelineCache &pipelineCache,
                     GuiSystem *gui, const ShaderVariant &shaderVariant,
                     const RenderSettings &settings,
                     const DirectionalShadowFrameData &shadow);

    // ---- 交换链重建 ----
    void recreateSwapChain();

    // ---- 访问器 ----
    VkRenderPass renderPass() const;
    VkDescriptorSetLayout globalDescriptorSetLayout() const {
        return globalDescriptorSetLayout_;
    }

    // ---- per-frame UBO 访问器 ----
    void *mappedUniformBuffer(uint32_t frameIndex) const;

  private:
    void createUniformBuffers();
    void createGlobalDescriptorSetLayout();
    void createGlobalDescriptorSets();
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
    std::unique_ptr<FrameRenderTargets> frameTargets_;
    RendererShaderPaths shaderPaths_;
    RenderPipeline pipeline_;
    MainForwardPass *mainForwardPass_ = nullptr;
    ToneMapPass *toneMapPass_ = nullptr;
};

} // namespace vkr
