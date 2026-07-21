#pragma once

#include "IRenderPass.h"
#include "core/FrameSync.h"

#include <array>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class FrameRenderTargets;
class RenderQueue;
class SwapChain;
struct RenderFrameContext;

class DirectionalShadowPass final : public IRenderPass {
  public:
    DirectionalShadowPass(Device &device, FrameRenderTargets &targets,
                          VkDescriptorSetLayout globalDescriptorSetLayout,
                          std::string shadowVertPath,
                          std::string shadowMaskFragPath);
    ~DirectionalShadowPass() override;

    DirectionalShadowPass(const DirectionalShadowPass &) = delete;
    DirectionalShadowPass &operator=(const DirectionalShadowPass &) = delete;

    std::string_view name() const override {
        return "DirectionalShadowPass";
    }
    void onResize(const SwapChain &) override {}
    void execute(const RenderFrameContext &frame,
                 const RenderQueue &queue) override;

  private:
    void createRenderPass();
    void createFramebuffers();
    void drawCasters(const RenderFrameContext &frame,
                     const RenderQueue &queue);

    Device *device_ = nullptr;
    FrameRenderTargets *targets_ = nullptr;
    VkDescriptorSetLayout globalDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::string shadowVertPath_;
    std::string shadowMaskFragPath_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::array<VkFramebuffer, MAX_FRAMES_IN_FLIGHT> framebuffers_{};
};

} // namespace vkr
