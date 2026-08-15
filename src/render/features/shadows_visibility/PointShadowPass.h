#pragma once

#include "render/graph/IRenderPass.h"
#include "render/features/shadows_visibility/PunctualShadow.h"
#include "render/geometry/RenderItem.h"
#include "core/FrameSync.h"

#include <array>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class PunctualShadowSliceBuffer;
class RenderResourcePool;
struct RenderFrameContext;
struct VisibilityFrame;

class DescriptorAllocator;

class PointShadowPass final : public IRenderPass {
  public:
    PointShadowPass(Device &device,
                    const RenderResourcePool &resources,
                    std::array<RenderImageHandle, 4> shadowDepthByCapacity,
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
    std::array<RenderImageHandle, 4> shadowDepthByCapacity_{};
    std::string vertPath_;
    std::string opaqueFragPath_;
    std::string maskFragPath_;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    std::unique_ptr<PunctualShadowSliceBuffer> sliceBuffer_;
};

} // namespace vkr
