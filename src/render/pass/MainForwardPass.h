#pragma once

#include "IRenderPass.h"
#include "core/FrameSync.h"

#include <array>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
class Device;
class RenderQueue;
class RenderResourceRegistry;
class SwapChain;
struct RenderFrameContext;

class MainForwardPass final : public IRenderPass {
  public:
    MainForwardPass(Device &device,
                    const RenderResourceRegistry &resources,
                    RendererResourceHandles resourceHandles,
                    DescriptorAllocator &descriptorAllocator);
    ~MainForwardPass() override;

    MainForwardPass(const MainForwardPass &) = delete;
    MainForwardPass &operator=(const MainForwardPass &) = delete;

    std::string_view name() const override { return "MainForward"; }
    std::vector<RenderImageUsage> resourceUsages() const override;
    void releaseSwapChainResources() override;
    void onResize(const SwapChain &swapChain,
                  const RenderResourceRegistry &resources) override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const RenderQueue &queue) override;

    VkRenderPass renderPass() const { return renderPass_; }
    VkDescriptorSetLayout shadowDescriptorSetLayout() const {
        return shadowDescriptorSetLayout_;
    }

  private:
    void createRenderPass(const RenderResourceRegistry &resources);
    void createFramebuffers(const RenderResourceRegistry &resources);
    void createShadowDescriptors(const RenderResourceRegistry &resources);
    void destroyFramebuffers();

    void begin(VkCommandBuffer cmd, uint32_t frameIndex,
               const RenderResourceRegistry &resources);
    void drawQueue(const RenderFrameContext &frame,
                   const RenderResourceRegistry &resources,
                   const RenderQueue &queue);

    Device *device_ = nullptr;
    RendererResourceHandles resourceHandles_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::array<VkFramebuffer, MAX_FRAMES_IN_FLIGHT> framebuffers_{};
    VkDescriptorSetLayout shadowDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> shadowDescriptorSets_{};
};

} // namespace vkr
