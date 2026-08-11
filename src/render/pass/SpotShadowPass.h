#pragma once

#include "render/pass/IRenderPass.h"
#include "render/PunctualShadow.h"
#include "render/RenderCommand.h"
#include "core/FrameSync.h"

#include <array>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class PunctualShadowSliceBuffer;
class RenderResourceRegistry;
struct RenderFrameContext;
struct VisibilityFrame;

class DescriptorAllocator;

class SpotShadowPass final : public IRenderPass {
  public:
    SpotShadowPass(Device &device,
                   const RenderResourceRegistry &resources,
                   RenderImageHandle shadowDepth,
                   DescriptorAllocator &descriptorAllocator,
                   std::string vertPath,
                   std::string maskFragPath);
    ~SpotShadowPass() override;

    SpotShadowPass(const SpotShadowPass &) = delete;
    SpotShadowPass &operator=(const SpotShadowPass &) = delete;

    std::string_view name() const override { return "SpotShadow"; }
    std::vector<RenderImageUsage> resourceUsages() const override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const VisibilityFrame &visibility) override;

  private:
    void createRenderPass(const RenderResourceRegistry &resources);
    void createFramebuffers(const RenderResourceRegistry &resources);
    void destroyFramebuffers();
    Device *device_ = nullptr;
    RenderImageHandle shadowDepth_{};
    std::string vertPath_;
    std::string maskFragPath_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::unique_ptr<PunctualShadowSliceBuffer> sliceBuffer_;
    std::array<VkFramebuffer, kMaxSpotShadowLights> framebuffers_{};
    std::array<VkImageView, kMaxSpotShadowLights> layerViews_{};
};

} // namespace vkr
