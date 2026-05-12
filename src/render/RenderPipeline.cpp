#include "RenderPipeline.h"

#include "RenderFrame.h"
#include "RenderQueue.h"
#include "core/SwapChain.h"
#include "render/pass/IRenderPass.h"

#include <utility>

namespace vkr {

RenderPipeline::~RenderPipeline() = default;

void RenderPipeline::addPass(std::unique_ptr<IRenderPass> pass) {
    passes_.push_back(std::move(pass));
}

void RenderPipeline::onResize(const SwapChain &swapChain) {
    for (auto &pass : passes_)
        pass->onResize(swapChain);
}

void RenderPipeline::execute(const RenderFrameContext &frame,
                             const RenderQueue &queue) {
    for (auto &pass : passes_)
        pass->execute(frame, queue);
}

} // namespace vkr
