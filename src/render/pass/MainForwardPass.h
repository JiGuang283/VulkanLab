#pragma once

#include "IRenderPass.h"
#include "core/FrameSync.h"

#include <array>
#include <vulkan/vulkan.h>

namespace vkr {

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
                     VkDescriptorSetLayout lightingDescriptorSetLayout,
                     VkDescriptorSetLayout atmosphereDescriptorSetLayout);
    ~MainForwardPass() override;

    MainForwardPass(const MainForwardPass &) = delete;
    MainForwardPass &operator=(const MainForwardPass &) = delete;

    std::string_view name() const override { return "MainForward"; }
    std::vector<RenderImageUsage> resourceUsages() const override;
    void releaseViewportResources() override;
    void onViewportResize(
        const RenderResourceRegistry &resources) override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const RenderQueue &queue) override;

    VkRenderPass renderPass() const { return renderPass_; }
  private:
    void createRenderPass(const RenderResourceRegistry &resources);
    void createFramebuffers(const RenderResourceRegistry &resources);
    void destroyFramebuffers();

    void begin(VkCommandBuffer cmd, uint32_t frameIndex,
               const RenderResourceRegistry &resources);
    void drawQueue(const RenderFrameContext &frame,
                   const RenderResourceRegistry &resources,
                   const RenderQueue &queue);

    Device *device_ = nullptr;
    RendererResourceHandles resourceHandles_{};
    VkDescriptorSetLayout lightingDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout atmosphereDescriptorSetLayout_ = VK_NULL_HANDLE;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::array<VkFramebuffer, MAX_FRAMES_IN_FLIGHT> framebuffers_{};
};

} // namespace vkr
