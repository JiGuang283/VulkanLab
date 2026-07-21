#pragma once

#include <memory>
#include <vector>

namespace vkr {

class IRenderPass;
class RenderQueue;
class RenderResourceRegistry;
class SwapChain;
struct RenderFrameContext;

class RenderPipeline {
  public:
    RenderPipeline() = default;
    ~RenderPipeline();

    RenderPipeline(const RenderPipeline &) = delete;
    RenderPipeline &operator=(const RenderPipeline &) = delete;

    void addPass(std::unique_ptr<IRenderPass> pass);
    void releaseSwapChainResources();
    void onResize(const SwapChain &swapChain,
                  const RenderResourceRegistry &resources);
    void validateResources(const RenderResourceRegistry &resources) const;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const RenderQueue &queue);

  private:
    std::vector<std::unique_ptr<IRenderPass>> passes_;
};

} // namespace vkr
