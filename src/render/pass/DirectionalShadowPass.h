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
    RgPassCondition condition() const override {
        return RgPassCondition::DirectionalShadow;
    }
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void recordNode(RenderGraphPassContext &context,
                    uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const VisibilityFrame &visibility) override;

  private:
    void drawCasters(const RenderFrameContext &frame,
                     const VisibilityFrame &visibility,
                     uint32_t cascadeIndex);
    void recordCascade(const RenderFrameContext &frame,
                       const VisibilityFrame &visibility,
                       uint32_t cascadeIndex);

    Device *device_ = nullptr;
    RenderImageHandle shadowDepth_{};
    VkDescriptorSetLayout globalDescriptorSetLayout_ = VK_NULL_HANDLE;
    std::string shadowVertPath_;
    std::string shadowMaskFragPath_;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
};

} // namespace vkr
