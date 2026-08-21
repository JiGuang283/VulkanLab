#include "render/features/lighting/ClusteredLightCullingPass.h"

#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "render/features/lighting/ClusteredLighting.h"
#include "render/frame/RenderFrame.h"
#include "render/frame/RenderView.h"
#include "render/graph/RenderGraph.h"
#include "render/pipeline/ComputePipeline.h"
#include "render/pipeline/ComputePipelineConfig.h"
#include "render/pipeline/PipelineCache.h"

#include <array>
#include <utility>

namespace vkr {

ClusteredLightCullingPass::ClusteredLightCullingPass(
    Device &device, ClusteredLightingResources &resources,
    VkDescriptorSetLayout globalLayout, std::string shaderPath)
    : device_(&device), resources_(&resources), globalLayout_(globalLayout),
      shaderPath_(std::move(shaderPath)) {}

void ClusteredLightCullingPass::setup(
    RenderGraphBuilder &builder, const RenderGraphBuildContext &) const {
    builder.addNode("ClusteredLighting/ClearStats", RgPassType::Transfer,
                    RgQueueClass::Transfer, 0);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        builder.useBuffer(resources_->statisticsBuffer(frame),
                          RgBufferAccess::TransferWrite, 0,
                          sizeof(GpuClusterStatistics), frame);
    }

    builder.addNode("ClusteredLighting/Build", RgPassType::Compute,
                    RgQueueClass::Compute, 1);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        builder.useBuffer(resources_->sceneLightBuffer(frame),
                          RgBufferAccess::StorageRead, 0,
                          VK_WHOLE_SIZE, frame);
        builder.useBuffer(resources_->clusterCountBuffer(frame),
                          RgBufferAccess::StorageWrite, 0,
                          VK_WHOLE_SIZE, frame);
        builder.useBuffer(resources_->lightIndexBuffer(frame),
                          RgBufferAccess::StorageWrite, 0,
                          VK_WHOLE_SIZE, frame);
        builder.useBuffer(resources_->statisticsBuffer(frame),
                          RgBufferAccess::StorageReadWrite, 0,
                          sizeof(GpuClusterStatistics), frame);
    }
}

void ClusteredLightCullingPass::recordNode(
    RenderGraphPassContext &context, uint32_t localNodeIndex,
    const VisibilityFrame &) {
    const RenderFrameContext &frame = context.frame;
    if (!resources_->active(frame.frameIndex))
        return;

    if (localNodeIndex == 0) {
        vkCmdFillBuffer(frame.cmd,
                        resources_->statisticsBuffer(frame.frameIndex), 0,
                        sizeof(GpuClusterStatistics), 0);
        return;
    }
    if (!frame.pipelineCache)
        return;

    ScopedGpuLabel label(device_->debugUtils(), frame.cmd,
                         "ClusteredLighting/Build");
    ComputePipelineConfig config{};
    config.debugName = "Pipeline/Lighting/ClusterBuild";
    config.computeShaderPath = shaderPath_;
    config.descriptorLayouts = {
        globalLayout_, resources_->descriptorSetLayout()};
    ComputePipeline &pipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(config));
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.handle());
    const std::array<VkDescriptorSet, 2> sets{
        frame.globalDescriptorSet,
        resources_->descriptorSet(frame.frameIndex)};
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 0,
                            static_cast<uint32_t>(sets.size()), sets.data(),
                            0, nullptr);
    const uint32_t workgroups =
        (resources_->grid().clusterCount + 63u) / 64u;
    vkCmdDispatch(frame.cmd, workgroups, 1, 1);
}

uint64_t ClusteredLightCullingPass::topologySignature() const {
    return resources_->topologySignature();
}

} // namespace vkr
