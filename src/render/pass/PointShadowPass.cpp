#include "PointShadowPass.h"

#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/Pipeline.h"
#include "core/PipelineConfigBuilder.h"
#include "core/VulkanCheck.h"
#include "render/DirectionalShadow.h"
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

PointShadowPass::PointShadowPass(Device &device,
                                 const RenderResourceRegistry &resources,
                                 std::array<RenderImageHandle, 4>
                                     shadowDepthByCapacity,
                                 DescriptorAllocator &descriptorAllocator,
                                 std::string vertPath,
                                 std::string opaqueFragPath,
                                 std::string maskFragPath)
    : device_(&device),
      shadowDepthByCapacity_(std::move(shadowDepthByCapacity)),
      vertPath_(std::move(vertPath)),
      opaqueFragPath_(std::move(opaqueFragPath)),
      maskFragPath_(std::move(maskFragPath)) {
    sliceBuffer_ = std::make_unique<PunctualShadowSliceBuffer>(
        device, descriptorAllocator, kPointShadowLayers,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        "PointShadowSliceBuffer");

    depthFormat_ =
        resources.description(shadowDepthByCapacity_.front()).format;
}

PointShadowPass::~PointShadowPass() {
    sliceBuffer_.reset();
}

void PointShadowPass::setup(
    RenderGraphBuilder &builder,
    const RenderGraphBuildContext &context) const {
    const uint32_t capacityIndex =
        context.features.pointShadowLightCount == 0
            ? 0u
            : std::min(context.features.pointShadowLightCount,
                       static_cast<uint32_t>(shadowDepthByCapacity_.size())) -
                  1u;
    const RenderImageHandle shadowDepth =
        shadowDepthByCapacity_[capacityIndex];
    for (uint32_t layer = 0; layer < kPointShadowLayers; ++layer) {
        const uint32_t light = layer / kPointShadowFaceCount;
        const uint32_t face = layer % kPointShadowFaceCount;
        builder.addNode("PointShadow/Light" + std::to_string(light) +
                            "/Face" + std::to_string(face),
                        RgPassType::Graphics, RgQueueClass::Graphics,
                        layer);
        builder.setActive(
            light < context.features.pointShadowLightCount);
        builder.addDepthAttachment(
            shadowDepth, RenderImageAccess::DepthAttachmentWrite,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
            {1.0f, 0},
            {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1});
    }
}

void PointShadowPass::recordNode(
    RenderGraphPassContext &context, uint32_t localNodeIndex,
    const VisibilityFrame &visibility) {
    if (localNodeIndex == 0)
        writeSlices(context.frame);
    recordFace(context.frame, visibility, localNodeIndex);
}

void PointShadowPass::writeSlices(const RenderFrameContext &frame) {
    if (!frame.view)
        return;
    const auto &punctualData = frame.view->shadow.punctual;
    for (uint32_t light = 0;
         light < punctualData.activePointCount; ++light) {
        const PointShadowData &point = punctualData.points[light];
        for (uint32_t face = 0; face < kPointShadowFaceCount; ++face) {
            PunctualShadowSlice slice{};
            slice.viewProjection = point.faceViewProjections[face];
            slice.lightPositionFar =
                glm::vec4(point.position, point.farPlane);
            sliceBuffer_->write(
                frame.frameIndex,
                light * kPointShadowFaceCount + face, slice);
        }
    }
}

void PointShadowPass::recordFace(
    const RenderFrameContext &frame,
    const VisibilityFrame &visibility, uint32_t layer) {
    if (!frame.pipelineCache || !frame.view ||
        layer >= kPointShadowLayers)
        return;
    const uint32_t lightIdx = layer / kPointShadowFaceCount;
    const uint32_t face = layer % kPointShadowFaceCount;
    const auto &punctualData = frame.view->shadow.punctual;
    if (lightIdx >= punctualData.activePointCount ||
        !punctualData.points[lightIdx].enabled)
        return;

    VkViewport viewport{};
    viewport.width = static_cast<float>(kPointShadowMapSize);
    viewport.height = static_cast<float>(kPointShadowMapSize);
    viewport.maxDepth = 1.0f;
    const VkRect2D scissor{{0, 0},
                           {kPointShadowMapSize, kPointShadowMapSize}};

    vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
    vkCmdSetScissor(frame.cmd, 0, 1, &scissor);

    ShadowCasterDrawConfig drawConfig{};
    drawConfig.rendering.depthAttachmentFormat = depthFormat_;
    drawConfig.rendering.samples = VK_SAMPLE_COUNT_1_BIT;
    drawConfig.sliceDescriptorLayout = sliceBuffer_->descriptorSetLayout();
    drawConfig.sliceDescriptorSet =
        sliceBuffer_->descriptorSet(frame.frameIndex);
    drawConfig.dynamicOffset = sliceBuffer_->dynamicOffset(layer);
    drawConfig.vertexShader = vertPath_;
    drawConfig.opaqueFragmentShader = opaqueFragPath_;
    drawConfig.maskFragmentShader = maskFragPath_;
    drawConfig.pipelinePrefix = "PointShadow";
    drawConfig.rasterDepthBias = false;
    ShadowCasterDrawRecorder::record(
        frame, visibility,
        visibility.pointShadowCasters[lightIdx][face], drawConfig);
}

} // namespace vkr
