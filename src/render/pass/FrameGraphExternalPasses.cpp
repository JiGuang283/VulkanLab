#include "render/pass/FrameGraphExternalPasses.h"

#include "render/RayTracingScene.h"
#include "render/RenderFrame.h"
#include "render/RenderGraph.h"

namespace vkr {

void RayTracingSceneBuildPass::prepareFrame(
    const RenderFrameContext &frame, const RenderResourceRegistry &,
    const VisibilityFrame &visibility) {
    if (frame.features.ddgiRequired)
        scene_->prepareFrame(frame.frameIndex, visibility);
}

void RayTracingSceneBuildPass::setup(
    RenderGraphBuilder &builder,
    const RenderGraphBuildContext &context) const {
    IRenderPass::setup(builder, context);
    builder.setSideEffect();
}

void RayTracingSceneBuildPass::execute(
    const RenderFrameContext &frame, const RenderResourceRegistry &,
    const VisibilityFrame &visibility) {
    scene_->build(frame.cmd, frame.frameIndex, visibility);
}

void ScreenshotCopyPass::setup(
    RenderGraphBuilder &builder,
    const RenderGraphBuildContext &context) const {
    builder.addNode(std::string(name()), passType(), queueClass());
    if (!context.features.captureSource) {
        builder.setActive(false);
        return;
    }
    switch (*context.features.captureSource) {
    case FrameCaptureSource::Viewport:
        builder.useImage({resources_.viewportColor,
                          RenderImageAccess::TransferRead,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        break;
    case FrameCaptureSource::Hdr:
        builder.useImage({resources_.compositedHdrColor,
                          RenderImageAccess::TransferRead,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        break;
    case FrameCaptureSource::Workspace:
        builder.useSwapchainImage(RenderImageAccess::TransferRead,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        break;
    }
    builder.setSideEffect();
}

void ScreenshotCopyPass::execute(
    const RenderFrameContext &frame, const RenderResourceRegistry &,
    const VisibilityFrame &) {
    if (frame.screenshotCopy)
        frame.screenshotCopy(frame.cmd);
}

} // namespace vkr
