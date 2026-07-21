#pragma once

#include "render/RenderResourceRegistry.h"

#include <string_view>
#include <vector>

namespace vkr {

class RenderQueue;
class RenderResourceRegistry;
class SwapChain;
struct RenderFrameContext;

class IRenderPass {
  public:
    virtual ~IRenderPass() = default;

    virtual std::string_view name() const = 0;
    virtual std::vector<RenderImageUsage> resourceUsages() const = 0;
    virtual void releaseSwapChainResources() {}
    virtual void onResize(const SwapChain &swapChain,
                          const RenderResourceRegistry &resources) = 0;
    virtual void execute(const RenderFrameContext &frame,
                         const RenderResourceRegistry &resources,
                         const RenderQueue &queue) = 0;
};

} // namespace vkr
