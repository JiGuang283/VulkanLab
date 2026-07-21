#include "RenderPipeline.h"

#include "RenderFrame.h"
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

void RenderPipeline::execute(const RenderFrameContext &frame,
                             const RenderResourceRegistry &resources,
                             const RenderQueue &queue) {
    for (auto &pass : passes_)
        pass->execute(frame, resources, queue);
}

} // namespace vkr
