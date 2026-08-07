#pragma once

#include "IRenderPass.h"
#include "core/FrameSync.h"

#include <array>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class RenderResourceRegistry;
class SwapChain;
struct RenderFrameContext;
struct VisibilityFrame;

enum class ForwardPhase {
    Opaque,
    Transparent,
};

class MainForwardPass final : public IRenderPass {
  public:
    MainForwardPass(Device &device,
                    const RenderResourceRegistry &resources,
                     RendererResourceHandles resourceHandles,
                     ForwardPhase phase,
                     VkDescriptorSetLayout lightingDescriptorSetLayout,
                     VkDescriptorSetLayout atmosphereDescriptorSetLayout,
                     VkDescriptorSetLayout ddgiDescriptorSetLayout);
    ~MainForwardPass() override;

    MainForwardPass(const MainForwardPass &) = delete;
    MainForwardPass &operator=(const MainForwardPass &) = delete;

    std::string_view name() const override {
        return phase_ == ForwardPhase::Opaque ? "MainForwardOpaque"
                                              : "MainForwardTransparent";
    }
    std::vector<RenderImageUsage> resourceUsages() const override;
    void releaseViewportResources() override;
    void onViewportResize(
        const RenderResourceRegistry &resources) override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const VisibilityFrame &visibility) override;

    VkRenderPass renderPass() const { return renderPass_; }
  private:
    void createRenderPass(const RenderResourceRegistry &resources);
    void createFramebuffers(const RenderResourceRegistry &resources);
    void destroyFramebuffers();

    void begin(VkCommandBuffer cmd, uint32_t frameIndex,
               const RenderResourceRegistry &resources);
    void drawQueue(const RenderFrameContext &frame,
                   const RenderResourceRegistry &resources,
                   const VisibilityFrame &visibility);

    Device *device_ = nullptr;
    RendererResourceHandles resourceHandles_{};
    ForwardPhase phase_ = ForwardPhase::Opaque;
    VkDescriptorSetLayout lightingDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout atmosphereDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout ddgiDescriptorSetLayout_ = VK_NULL_HANDLE;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::array<VkFramebuffer, MAX_FRAMES_IN_FLIGHT> framebuffers_{};
};

} // namespace vkr
