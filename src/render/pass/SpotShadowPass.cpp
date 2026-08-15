#include "SpotShadowPass.h"

#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/Pipeline.h"
#include "core/PipelineConfigBuilder.h"
#include "core/VulkanCheck.h"
#include "render/GpuMaterialData.h"
#include "render/MaterialInstance.h"
#include "render/MaterialTemplate.h"
#include "render/Mesh.h"
#include "render/PipelineCache.h"
#include "render/PipelineKey.h"
#include "render/PunctualShadowSliceBuffer.h"
#include "render/RenderFrame.h"
#include "render/RenderGraph.h"
#include "render/RenderResourceRegistry.h"
#include "render/RenderView.h"
#include "render/Visibility.h"
#include "render/pass/ShadowCasterDrawRecorder.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <string>
#include <utility>

namespace vkr {

SpotShadowPass::SpotShadowPass(Device &device,
                               const RenderResourceRegistry &resources,
                               std::array<RenderImageHandle, 4>
                                   shadowDepthByCapacity,
                               DescriptorAllocator &descriptorAllocator,
                               std::string vertPath,
                               std::string maskFragPath)
    : device_(&device),
      shadowDepthByCapacity_(std::move(shadowDepthByCapacity)),
      vertPath_(std::move(vertPath)),
      maskFragPath_(std::move(maskFragPath)) {
    sliceBuffer_ = std::make_unique<PunctualShadowSliceBuffer>(
        device, descriptorAllocator, kMaxSpotShadowLights,
        VK_SHADER_STAGE_VERTEX_BIT, "SpotShadowSliceBuffer");

    depthFormat_ =
        resources.description(shadowDepthByCapacity_.front()).format;
}

SpotShadowPass::~SpotShadowPass() {
    sliceBuffer_.reset();
}

void SpotShadowPass::setup(
    RenderGraphBuilder &builder,
    const RenderGraphBuildContext &context) const {
    const uint32_t capacityIndex =
        context.features.spotShadowLightCount == 0
            ? 0u
            : std::min(context.features.spotShadowLightCount,
                       static_cast<uint32_t>(shadowDepthByCapacity_.size())) -
                  1u;
    const RenderImageHandle shadowDepth =
        shadowDepthByCapacity_[capacityIndex];
    for (uint32_t light = 0; light < kMaxSpotShadowLights; ++light) {
        builder.addNode("SpotShadow/Light" + std::to_string(light),
                        RgPassType::Graphics, RgQueueClass::Graphics,
                        light);
        builder.setActive(light < context.features.spotShadowLightCount);
        builder.addDepthAttachment(
            shadowDepth, RenderImageAccess::DepthAttachmentWrite,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
            {1.0f, 0},
            {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, light, 1});
    }
}

void SpotShadowPass::recordNode(
    RenderGraphPassContext &context, uint32_t localNodeIndex,
    const VisibilityFrame &visibility) {
    if (localNodeIndex == 0)
        writeSlices(context.frame);
    recordLight(context.frame, visibility, localNodeIndex);
}

void SpotShadowPass::writeSlices(const RenderFrameContext &frame) {
    if (!frame.view)
        return;
    const auto &punctualData = frame.view->shadow.punctual;
    for (uint32_t light = 0;
         light < punctualData.activeSpotCount; ++light) {
        const SpotShadowData &spot = punctualData.spots[light];
        PunctualShadowSlice slice{};
        slice.viewProjection = spot.viewProjection;
        slice.lightPositionFar.w = spot.farPlane;
        sliceBuffer_->write(frame.frameIndex, light, slice);
    }
}

void SpotShadowPass::recordLight(
    const RenderFrameContext &frame,
    const VisibilityFrame &visibility, uint32_t lightIdx) {
    if (!frame.pipelineCache || !frame.view ||
        lightIdx >= kMaxSpotShadowLights)
        return;
    const auto &punctualData = frame.view->shadow.punctual;
    if (lightIdx >= punctualData.activeSpotCount ||
        !punctualData.spots[lightIdx].enabled)
        return;

    VkViewport viewport{};
    viewport.width = static_cast<float>(kSpotShadowMapSize);
    viewport.height = static_cast<float>(kSpotShadowMapSize);
    viewport.maxDepth = 1.0f;
    const VkRect2D scissor{{0, 0},
                           {kSpotShadowMapSize, kSpotShadowMapSize}};

    vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
    vkCmdSetScissor(frame.cmd, 0, 1, &scissor);

    ShadowCasterDrawConfig drawConfig{};
    drawConfig.rendering.depthAttachmentFormat = depthFormat_;
    drawConfig.rendering.samples = VK_SAMPLE_COUNT_1_BIT;
    drawConfig.sliceDescriptorLayout = sliceBuffer_->descriptorSetLayout();
    drawConfig.sliceDescriptorSet =
        sliceBuffer_->descriptorSet(frame.frameIndex);
    drawConfig.dynamicOffset = sliceBuffer_->dynamicOffset(lightIdx);
    drawConfig.vertexShader = vertPath_;
    drawConfig.maskFragmentShader = maskFragPath_;
    drawConfig.pipelinePrefix = "SpotShadow";
    drawConfig.rasterDepthBias = true;
    ShadowCasterDrawRecorder::record(
        frame, visibility, visibility.spotShadowCasters[lightIdx],
        drawConfig);
}

} // namespace vkr
