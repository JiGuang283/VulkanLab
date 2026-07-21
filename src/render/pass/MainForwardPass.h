#pragma once

#include "IRenderPass.h"
#include "core/FrameSync.h"

#include <array>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
class Device;
class FrameRenderTargets;
class RenderQueue;
class SwapChain;
struct RenderFrameContext;

class MainForwardPass final : public IRenderPass {
  public:
    MainForwardPass(Device &device, FrameRenderTargets &targets,
                    DescriptorAllocator &descriptorAllocator);
    ~MainForwardPass() override;

    MainForwardPass(const MainForwardPass &) = delete;
    MainForwardPass &operator=(const MainForwardPass &) = delete;

    std::string_view name() const override { return "MainForwardPass"; }
    void releaseSwapChainResources() override;
    void onResize(const SwapChain &swapChain) override;
    void execute(const RenderFrameContext &frame,
                 const RenderQueue &queue) override;

    VkRenderPass renderPass() const { return renderPass_; }
    VkDescriptorSetLayout shadowDescriptorSetLayout() const {
        return shadowDescriptorSetLayout_;
    }

  private:
    void createRenderPass();
    void createFramebuffers();
    void createShadowDescriptors();
    void destroyFramebuffers();

    void begin(VkCommandBuffer cmd, uint32_t frameIndex);
    void drawQueue(const RenderFrameContext &frame, const RenderQueue &queue);

    Device *device_ = nullptr;
    FrameRenderTargets *targets_ = nullptr;
    DescriptorAllocator *descriptorAllocator_ = nullptr;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::array<VkFramebuffer, MAX_FRAMES_IN_FLIGHT> framebuffers_{};
    VkDescriptorSetLayout shadowDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> shadowDescriptorSets_{};
};

} // namespace vkr
