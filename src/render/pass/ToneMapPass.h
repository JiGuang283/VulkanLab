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

class ToneMapPass final : public IRenderPass {
  public:
    ToneMapPass(Device &device, SwapChain &swapChain,
                const RenderResourceRegistry &resources,
                RenderImageHandle hdrColor,
                RenderSamplerHandle hdrSampler,
                DescriptorAllocator &descriptorAllocator,
                std::string fullscreenVertPath,
                std::string toneMapFragPath);
    ~ToneMapPass() override;

    ToneMapPass(const ToneMapPass &) = delete;
    ToneMapPass &operator=(const ToneMapPass &) = delete;

    std::string_view name() const override { return "ToneMapPass"; }
    std::vector<RenderImageUsage> resourceUsages() const override;
    void releaseSwapChainResources() override;
    void onResize(const SwapChain &swapChain,
                  const RenderResourceRegistry &resources) override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const RenderQueue &queue) override;

    VkRenderPass renderPass() const { return renderPass_; }

  private:
    void createRenderPass();
    void createFramebuffers();
    void destroyFramebuffers();
    void createDescriptors(const RenderResourceRegistry &resources);
    void updateDescriptors(const RenderResourceRegistry &resources);

    Device *device_ = nullptr;
    SwapChain *swapChain_ = nullptr;
    RenderImageHandle hdrColor_{};
    RenderSamplerHandle hdrSampler_{};
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    std::string fullscreenVertPath_;
    std::string toneMapFragPath_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
    VkDescriptorSetLayout sourceDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sourceDescriptorSets_{};
};

} // namespace vkr
