#pragma once

#include <memory>
#include <string>
#include <vector>

namespace vkr {

class IRenderPass;
class GpuPassProfiler;
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
    void releaseViewportResources();
    void onViewportResize(const RenderResourceRegistry &resources);
    void releaseSwapChainResources();
    void onSwapChainResize(const SwapChain &swapChain);
    void validateResources(const RenderResourceRegistry &resources) const;
    std::vector<std::string> passNames() const;
    void execute(const RenderFrameContext &frame,
                 const RenderResourceRegistry &resources,
                 const RenderQueue &queue, GpuPassProfiler *profiler);

  private:
    std::vector<std::unique_ptr<IRenderPass>> passes_;
};

} // namespace vkr
