#pragma once

#include "render/RenderGraphTypes.h"
#include "render/RenderResourceRegistry.h"

#include <string_view>
#include <vector>

namespace vkr {

class RenderResourceRegistry;
class RenderGraphBuilder;
class SwapChain;
struct RenderGraphBuildContext;
struct RenderGraphPassContext;
struct RenderFrameContext;
struct VisibilityFrame;

class IRenderPass {
  public:
    virtual ~IRenderPass() = default;

    virtual std::string_view name() const = 0;
    virtual RgPassType passType() const { return RgPassType::Graphics; }
    virtual RgQueueClass queueClass() const { return RgQueueClass::Graphics; }
    virtual RgPassCondition condition() const {
        return RgPassCondition::Always;
    }
    virtual void prepareGraph(const RenderFrameContext &,
                              const RenderResourceRegistry &,
                              const VisibilityFrame &) {}
    virtual void prepareFrame(const RenderFrameContext &,
                              const RenderResourceRegistry &,
                              const VisibilityFrame &) {}
    virtual void setup(RenderGraphBuilder &builder,
                       const RenderGraphBuildContext &context) const = 0;
    virtual void recordNode(RenderGraphPassContext &context,
                            uint32_t localNodeIndex,
                            const VisibilityFrame &visibility) = 0;
    virtual bool managesDeclaredTransitionsInternally() const { return false; }
    virtual uint64_t topologySignature() const { return 0; }
    virtual void onResourceResidencyChanged(
        const RenderResourceRegistry &resources, uint32_t,
        const std::vector<RenderImageHandle> &) {
        releaseViewportResources();
        onViewportResize(resources);
    }
    virtual void releaseViewportResources() {}
    virtual void onViewportResize(const RenderResourceRegistry &) {}
    virtual void releaseSwapChainResources() {}
    virtual void onSwapChainResize(const SwapChain &) {}
};

} // namespace vkr
