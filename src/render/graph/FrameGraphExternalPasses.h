#pragma once

#include "render/graph/IRenderPass.h"

namespace vkr {

class RayTracingScene;

class RayTracingSceneBuildPass final : public IRenderPass {
  public:
    explicit RayTracingSceneBuildPass(RayTracingScene &scene)
        : scene_(&scene) {}

    std::string_view name() const override { return "RayTracingScene"; }
    RgPassType passType() const override { return RgPassType::External; }
    RgPassCondition condition() const override { return RgPassCondition::Ddgi; }
    void prepareGraph(const RenderFrameContext &frame,
                      const RenderResourcePool &resources,
                      const VisibilityFrame &visibility) override;
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void recordNode(RenderGraphPassContext &context, uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;

  private:
    RayTracingScene *scene_ = nullptr;
};

class ScreenshotCopyPass final : public IRenderPass {
  public:
    explicit ScreenshotCopyPass(const RendererResourceHandles &resources)
        : resources_(resources) {}

    std::string_view name() const override { return "ScreenshotCopy"; }
    RgPassType passType() const override { return RgPassType::Transfer; }
    RgPassCondition condition() const override { return RgPassCondition::Capture; }
    void setup(RenderGraphBuilder &builder,
               const RenderGraphBuildContext &context) const override;
    void recordNode(RenderGraphPassContext &context, uint32_t localNodeIndex,
                    const VisibilityFrame &visibility) override;

  private:
    RendererResourceHandles resources_{};
};

} // namespace vkr
