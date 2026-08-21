#include "render/features/surface/SurfacePrepass.h"

#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "render/pipeline/Pipeline.h"
#include "render/pipeline/PipelineConfigBuilder.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"
#include "render/features/surface/SurfaceFrameData.h"
#include "render/features/surface/SurfaceDrawRecorder.h"
#include "render/material/GpuMaterialData.h"
#include "render/material/MaterialInstance.h"
#include "render/material/MaterialSystem.h"
#include "render/material/MaterialTemplate.h"
#include "render/shader/ShaderRegistry.h"
#include "render/geometry/Mesh.h"
#include "render/pipeline/PipelineCache.h"
#include "render/frame/RenderFrame.h"
#include "render/graph/RenderGraph.h"
#include "render/graph/RenderResourcePool.h"
#include "render/features/shadows_visibility/Visibility.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vkr {

namespace {

uint32_t surfaceAttachmentCount(const FrameRenderFeatures &features) {
    if (features.surfaceAlbedoRequired)
        return 3;
    if (features.surfaceMotionRequired)
        return 2;
    return features.surfaceNormalsRequired ? 1u : 0u;
}

} // namespace

SurfacePrepass::SurfacePrepass(
    Device &device, const RenderResourcePool &,
    RendererResourceHandles resourceHandles,
    SurfaceFrameData &frameData,
    VkDescriptorSetLayout globalDescriptorSetLayout)
    : device_(&device), frameData_(&frameData),
      resourceHandles_(resourceHandles),
      globalDescriptorSetLayout_(globalDescriptorSetLayout) {
    if (!resourceHandles_.surfaceDepth.valid() ||
        !resourceHandles_.surfaceNormalRoughness.valid() ||
        !resourceHandles_.surfaceMotion.valid()) {
        throw std::invalid_argument(
            "SurfacePrepass requires all surface attachments");
    }
}

void SurfacePrepass::setup(RenderGraphBuilder &builder,
                           const RenderGraphBuildContext &context) const {
    builder.addNode(std::string(name()), RgPassType::Graphics,
                    RgQueueClass::Graphics);
    if (context.features.surfaceNormalsRequired) {
        builder.addColorAttachment(
            resourceHandles_.surfaceNormalRoughness,
            RenderImageAccess::ColorAttachmentWrite,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
            {{0.5f, 0.5f, 1.0f, 0.0f}});
    }
    if (context.features.surfaceMotionRequired) {
        builder.addColorAttachment(
            resourceHandles_.surfaceMotion,
            RenderImageAccess::ColorAttachmentWrite,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
            {{0.0f, 0.0f, 0.0f, 0.0f}});
    }
    if (context.features.surfaceAlbedoRequired &&
        resourceHandles_.surfaceAlbedoMetallic.valid()) {
        builder.addColorAttachment(
            resourceHandles_.surfaceAlbedoMetallic,
            RenderImageAccess::ColorAttachmentWrite,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
            {{0.0f, 0.0f, 0.0f, 0.0f}});
    }
    builder.addDepthAttachment(
        resourceHandles_.surfaceDepth,
        RenderImageAccess::DepthAttachmentWrite,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
}

void SurfacePrepass::recordNode(
    RenderGraphPassContext &context, uint32_t,
    const VisibilityFrame &visibility) {
    const VkExtent2D extent =
        context.resources.extent(resourceHandles_.surfaceDepth);
    frameData_->prepare(context.frame.frameIndex, visibility, extent);
    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(context.frame.cmd, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(context.frame.cmd, 0, 1, &scissor);
    draw(context.frame, context.resources, visibility);
}

uint32_t SurfacePrepass::historyCapacity(uint32_t frameIndex) const {
    return frameData_->historyCapacity(frameIndex);
}

uint64_t SurfacePrepass::allocatedBytes() const {
    return frameData_->allocatedBytes();
}

void SurfacePrepass::draw(const RenderFrameContext &frame,
                          const RenderResourcePool &resources,
                          const VisibilityFrame &visibility) {
    SurfaceDrawRecordConfig config{};
    config.debugName = "SurfacePrepass";
    config.opaquePass = MaterialShaderPass::SurfaceOpaque;
    config.maskPass = MaterialShaderPass::SurfaceMask;
    const uint32_t attachmentCount =
        surfaceAttachmentCount(frame.features);
    if (attachmentCount >= 1) {
        config.colorAttachmentFormats.push_back(
            resources.description(resourceHandles_.surfaceNormalRoughness)
                .format);
    }
    if (attachmentCount >= 2) {
        config.colorAttachmentFormats.push_back(
            resources.description(resourceHandles_.surfaceMotion).format);
    }
    if (attachmentCount >= 3) {
        config.colorAttachmentFormats.push_back(
            resources.description(resourceHandles_.surfaceAlbedoMetallic)
                .format);
    }
    config.depthAttachmentFormat =
        resources.description(resourceHandles_.surfaceDepth).format;
    recordSurfaceDraws(frame, resources, visibility, *frameData_, config);
}

} // namespace vkr
