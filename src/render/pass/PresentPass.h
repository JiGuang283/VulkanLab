#pragma once

#include "IRenderPass.h"
#include "core/FrameSync.h"

#include <array>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class DescriptorAllocator;
class Device;
class RenderQueue;
class RenderResourceRegistry;
class SwapChain;
struct RenderFrameContext;

class PresentPass final : public IRenderPass {
  public:
    PresentPass(Device &device, SwapChain &swapChain,
                const RenderResourceRegistry &resources,
                RenderImageHandle viewportColor,
                RenderSamplerHandle viewportSampler,
                DescriptorAllocator &descriptorAllocator,
                std::string fullscreenVertPath,
                std::string presentFragPath);
    ~PresentPass() override;

    PresentPass(const PresentPass &) = delete;
    PresentPass &operator=(const PresentPass &) = delete;

    std::string_view name() const override { return "Present + UI"; }
    std::vector<RenderImageUsage> resourceUsages() const override;
    void releaseViewportResources() override {}
    void onViewportResize(
        const RenderResourceRegistry &resources) override;
    void releaseSwapChainResources() override;
    void onSwapChainResize(const SwapChain &swapChain) override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const VisibilityFrame &visibility) override;

    VkRenderPass renderPass() const { return renderPass_; }

  private:
    void createRenderPass();
    void createFramebuffers();
    void destroyFramebuffers();
    void createDescriptors(const RenderResourceRegistry &resources);
    void updateDescriptors(const RenderResourceRegistry &resources);

    Device *device_ = nullptr;
    SwapChain *swapChain_ = nullptr;
    RenderImageHandle viewportColor_{};
    RenderSamplerHandle viewportSampler_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    std::string fullscreenVertPath_;
    std::string presentFragPath_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
    VkDescriptorSetLayout sourceDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sourceDescriptorSets_{};
};

} // namespace vkr
