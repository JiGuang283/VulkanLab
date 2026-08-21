#pragma once

#include "render/features/surface/GBufferResources.h"
#include "render/graph/IRenderPass.h"

#include <cstdint>

namespace vkr {

class Device;
class SurfaceFrameData;

class GBufferPass final : public IRenderPass {
  public:
    GBufferPass(Device &device, GBufferResources resources,
                SurfaceFrameData &frameData);

    std::string_view name() const override { return "Deferred/GBuffer"; }
    RgPassCondition condition() const override {
        return RgPassCondition::GBuffer;
    }
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void recordNode(RenderGraphPassContext &context,
                    uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;

    uint32_t lastDrawCount() const { return lastDrawCount_; }

  private:
    Device *device_ = nullptr;
    GBufferResources resources_{};
    SurfaceFrameData *frameData_ = nullptr;
    uint32_t lastDrawCount_ = 0;
};

} // namespace vkr
