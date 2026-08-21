#include "render/features/surface/GBufferPass.h"

#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "render/features/shadows_visibility/Visibility.h"
#include "render/features/surface/SurfaceDrawRecorder.h"
#include "render/features/surface/SurfaceFrameData.h"
#include "render/frame/RenderFrame.h"
#include "render/graph/RenderGraph.h"
#include "render/graph/RenderResourcePool.h"

#include <stdexcept>

namespace vkr {

GBufferPass::GBufferPass(Device &device, GBufferResources resources,
                         SurfaceFrameData &frameData)
    : device_(&device), resources_(resources), frameData_(&frameData) {
    if (!resources_.valid())
        throw std::invalid_argument(
            "GBufferPass requires the complete GBuffer resource set");
}

void GBufferPass::setup(RenderGraphBuilder &builder,
                        const RenderGraphBuildContext &) const {
    builder.addNode(std::string(name()), RgPassType::Graphics,
                    RgQueueClass::Graphics);
    builder.addColorAttachment(
        resources_.baseColorMetallic,
        RenderImageAccess::ColorAttachmentWrite,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
        VkClearColorValue{{0.0f, 0.0f, 0.0f, 0.0f}});
    builder.addColorAttachment(
        resources_.normalRoughnessOcclusion,
        RenderImageAccess::ColorAttachmentWrite,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
        VkClearColorValue{{0.5f, 0.5f, 1.0f, 1.0f}});
    builder.addColorAttachment(
        resources_.emissiveSurfaceFlags,
        RenderImageAccess::ColorAttachmentWrite,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
        VkClearColorValue{{0.0f, 0.0f, 0.0f, 0.0f}});
    builder.addColorAttachment(
        resources_.motion, RenderImageAccess::ColorAttachmentWrite,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
        VkClearColorValue{{0.0f, 0.0f, 0.0f, 0.0f}});
    builder.addDepthAttachment(
        resources_.depth, RenderImageAccess::DepthAttachmentWrite,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
}

void GBufferPass::recordNode(RenderGraphPassContext &context, uint32_t,
                             const VisibilityFrame &visibility) {
    const VkExtent2D extent =
        context.resources.extent(resources_.depth);
    frameData_->prepare(context.frame.frameIndex, visibility, extent);

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(context.frame.cmd, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(context.frame.cmd, 0, 1, &scissor);

    ScopedGpuLabel label(device_->debugUtils(), context.frame.cmd,
                         "Deferred/GBuffer/Geometry");
    SurfaceDrawRecordConfig config{};
    config.debugName = "Deferred/GBuffer";
    config.opaquePass = MaterialShaderPass::GBufferOpaque;
    config.maskPass = MaterialShaderPass::GBufferMask;
    for (RenderImageHandle color : resources_.colors()) {
        config.colorAttachmentFormats.push_back(
            context.resources.description(color).format);
    }
    config.depthAttachmentFormat =
        context.resources.description(resources_.depth).format;
    config.useVisibilityIndirect = true;
    recordSurfaceDraws(context.frame, context.resources, visibility,
                       *frameData_, config);
    lastDrawCount_ =
        static_cast<uint32_t>(visibility.cameraOpaque.size());
}

} // namespace vkr
