#include "RenderPipeline.h"

#include "RenderFrame.h"
#include "GpuPassProfiler.h"
#include "RenderQueue.h"
#include "RenderResourceRegistry.h"
#include "core/SwapChain.h"
#include "render/pass/IRenderPass.h"

#include <string>
#include <utility>

namespace vkr {

RenderPipeline::~RenderPipeline() = default;

void RenderPipeline::addPass(std::unique_ptr<IRenderPass> pass) {
    passes_.push_back(std::move(pass));
}

void RenderPipeline::releaseSwapChainResources() {
    for (auto pass = passes_.rbegin(); pass != passes_.rend(); ++pass)
        (*pass)->releaseSwapChainResources();
}

void RenderPipeline::onResize(const SwapChain &swapChain,
                              const RenderResourceRegistry &resources) {
    for (auto &pass : passes_)
        pass->onResize(swapChain, resources);
}

void RenderPipeline::validateResources(
    const RenderResourceRegistry &resources) const {
    std::vector<RenderPassResourceUsage> usages;
    usages.reserve(passes_.size());
    for (const auto &pass : passes_) {
        usages.push_back(
            {std::string(pass->name()), pass->resourceUsages()});
    }
    validateRenderResourceContracts(resources.imageDescriptions(), usages);
}

std::vector<std::string> RenderPipeline::passNames() const {
    std::vector<std::string> names;
    names.reserve(passes_.size());
    for (const auto &pass : passes_)
        names.emplace_back(pass->name());
    return names;
}

void RenderPipeline::execute(const RenderFrameContext &frame,
                             const RenderResourceRegistry &resources,
                             const RenderQueue &queue,
                             GpuPassProfiler *profiler) {
    for (uint32_t passIndex = 0; passIndex < passes_.size(); ++passIndex) {
        if (profiler)
            profiler->beginPass(frame.cmd, frame.frameIndex, passIndex);
        auto &pass = passes_[passIndex];
        pass->execute(frame, resources, queue);
        if (profiler)
            profiler->endPass(frame.cmd, frame.frameIndex, passIndex);
    }
}

} // namespace vkr
