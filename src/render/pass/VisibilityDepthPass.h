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

class VisibilityDepthPass final : public IRenderPass {
  public:
    VisibilityDepthPass(Device &device,
                        const RenderResourceRegistry &resources,
                        RenderImageHandle visibilityDepth,
                        VkDescriptorSetLayout globalDescriptorSetLayout,
                        std::string depthVertPath,
                        std::string depthMaskFragPath);
    ~VisibilityDepthPass() override;

    std::string_view name() const override { return "VisibilityDepth"; }
    std::vector<RenderImageUsage> resourceUsages() const override;
    void releaseViewportResources() override;
    void onViewportResize(const RenderResourceRegistry &resources) override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const VisibilityFrame &visibility) override;

  private:
    void createRenderPass(const RenderResourceRegistry &resources);
    void createFramebuffers(const RenderResourceRegistry &resources);
    void destroyFramebuffers();
    void draw(const RenderFrameContext &frame, const RenderQueue &queue);

    Device *device_ = nullptr;
    RenderImageHandle visibilityDepth_{};
    VkDescriptorSetLayout globalDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::string depthVertPath_;
    std::string depthMaskFragPath_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::array<VkFramebuffer, MAX_FRAMES_IN_FLIGHT> framebuffers_{};
};

} // namespace vkr
