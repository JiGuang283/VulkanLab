#pragma once

#include "render/RenderResourceRegistry.h"

#include <string_view>
#include <vector>

namespace vkr {

class RenderResourceRegistry;
class SwapChain;
struct RenderFrameContext;
struct VisibilityFrame;

class IRenderPass {
  public:
    virtual ~IRenderPass() = default;

    virtual std::string_view name() const = 0;
    virtual std::vector<RenderImageUsage> resourceUsages() const = 0;
    virtual void releaseViewportResources() {}
    virtual void onViewportResize(const RenderResourceRegistry &) {}
    virtual void releaseSwapChainResources() {}
    virtual void onSwapChainResize(const SwapChain &) {}
    virtual void execute(const RenderFrameContext &frame,
                         const RenderResourceRegistry &resources,
                         const VisibilityFrame &visibility) = 0;
};

} // namespace vkr
