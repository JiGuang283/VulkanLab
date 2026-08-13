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
    RgPassCondition condition() const override {
        return RgPassCondition::SpotShadow;
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
    void writeSlices(const RenderFrameContext &frame);
    void recordLight(const RenderFrameContext &frame,
                     const VisibilityFrame &visibility,
                     uint32_t lightIndex);
    Device *device_ = nullptr;
    RenderImageHandle shadowDepth_{};
    std::string vertPath_;
    std::string maskFragPath_;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    std::unique_ptr<PunctualShadowSliceBuffer> sliceBuffer_;
};

} // namespace vkr
