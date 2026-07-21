#pragma once

#include "IRenderPass.h"
#include "core/FrameSync.h"

#include <array>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class RenderQueue;
class RenderResourceRegistry;
class SwapChain;
struct RenderFrameContext;

class DirectionalShadowPass final : public IRenderPass {
  public:
    DirectionalShadowPass(Device &device,
                          const RenderResourceRegistry &resources,
                          RenderImageHandle shadowDepth,
                          VkDescriptorSetLayout globalDescriptorSetLayout,
                          std::string shadowVertPath,
                          std::string shadowMaskFragPath);
    ~DirectionalShadowPass() override;

    DirectionalShadowPass(const DirectionalShadowPass &) = delete;
    DirectionalShadowPass &operator=(const DirectionalShadowPass &) = delete;

    std::string_view name() const override {
        return "DirectionalShadowPass";
    }
    std::vector<RenderImageUsage> resourceUsages() const override;
    void onResize(const SwapChain &,
                  const RenderResourceRegistry &) override {}
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const RenderQueue &queue) override;

  private:
    void createRenderPass(const RenderResourceRegistry &resources);
    void createFramebuffers(const RenderResourceRegistry &resources);
    void drawCasters(const RenderFrameContext &frame,
                     const RenderQueue &queue);

    Device *device_ = nullptr;
    RenderImageHandle shadowDepth_{};
    VkDescriptorSetLayout globalDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::string shadowVertPath_;
    std::string shadowMaskFragPath_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::array<VkFramebuffer, MAX_FRAMES_IN_FLIGHT> framebuffers_{};
};

} // namespace vkr
