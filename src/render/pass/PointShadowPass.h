#pragma once

#include "render/pass/IRenderPass.h"
#include "render/PunctualShadow.h"
#include "render/RenderItem.h"
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

class PointShadowPass final : public IRenderPass {
  public:
    PointShadowPass(Device &device,
                    const RenderResourceRegistry &resources,
                    RenderImageHandle shadowDepth,
                    DescriptorAllocator &descriptorAllocator,
                    std::string vertPath,
                    std::string opaqueFragPath,
                    std::string maskFragPath);
    ~PointShadowPass() override;

    PointShadowPass(const PointShadowPass &) = delete;
    PointShadowPass &operator=(const PointShadowPass &) = delete;

    std::string_view name() const override { return "PointShadow"; }
    RgPassCondition condition() const override {
        return RgPassCondition::PointShadow;
    }
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void recordNode(RenderGraphPassContext &context,
                    uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;

  private:
    void writeSlices(const RenderFrameContext &frame);
    void recordFace(const RenderFrameContext &frame,
                    const VisibilityFrame &visibility,
                    uint32_t layer);
    Device *device_ = nullptr;
    RenderImageHandle shadowDepth_{};
    std::string vertPath_;
    std::string opaqueFragPath_;
    std::string maskFragPath_;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    std::unique_ptr<PunctualShadowSliceBuffer> sliceBuffer_;
};

} // namespace vkr
