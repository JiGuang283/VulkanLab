#pragma once

#include <string_view>

namespace vkr {

class RenderQueue;
class SwapChain;
struct RenderFrameContext;

class IRenderPass {
  public:
    virtual ~IRenderPass() = default;

    virtual std::string_view name() const = 0;
    virtual void releaseSwapChainResources() {}
    virtual void onResize(const SwapChain &swapChain) = 0;
    virtual void execute(const RenderFrameContext &frame,
                         const RenderQueue &queue) = 0;
};

} // namespace vkr
