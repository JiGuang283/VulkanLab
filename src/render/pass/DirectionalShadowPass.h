#pragma once

#include "IRenderPass.h"
#include "core/FrameSync.h"
#include "render/DirectionalShadow.h"

#include <array>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class RenderResourceRegistry;
class SwapChain;
struct RenderFrameContext;
struct VisibilityFrame;

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
        return "DirectionalShadow";
    }
    std::vector<RenderImageUsage> resourceUsages() const override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const VisibilityFrame &visibility) override;

  private:
    void createRenderPass(const RenderResourceRegistry &resources);
    void createFramebuffers(const RenderResourceRegistry &resources);
    void destroyFramebuffers();
    void drawCasters(const RenderFrameContext &frame,
                     const VisibilityFrame &visibility,
                     uint32_t cascadeIndex);

    Device *device_ = nullptr;
    RenderImageHandle shadowDepth_{};
    VkDescriptorSetLayout globalDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::string shadowVertPath_;
    std::string shadowMaskFragPath_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::array<VkFramebuffer, kCsmCascadeCount> framebuffers_{};
    std::array<VkImageView, kCsmCascadeCount> cascadeViews_{};
};

} // namespace vkr
