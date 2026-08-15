#include "render/features/shadows_visibility/HiZBuildPass.h"

#include "render/pipeline/ComputePipeline.h"
#include "render/pipeline/ComputePipelineConfig.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/VulkanCheck.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"
#include "render/pipeline/PipelineCache.h"
#include "render/frame/RenderFrame.h"
#include "render/graph/RenderGraph.h"
#include "render/graph/RenderResourcePool.h"
#include "render/frame/RenderView.h"
#include "render/features/shadows_visibility/Visibility.h"

#include <array>
#include <stdexcept>
#include <utility>

namespace vkr {

namespace {

constexpr uint32_t kWorkgroupSize = 8;

uint32_t dispatchCount(uint32_t value) {
    return (value + kWorkgroupSize - 1u) / kWorkgroupSize;
}

struct HiZPush {
    glm::uvec4 extents{0};
};

} // namespace

HiZBuildPass::HiZBuildPass(Device &device,
                           const RenderResourcePool &resources,
                           RendererResourceHandles resourceHandles,
                           DescriptorAllocator &descriptorAllocator,
                           std::string initShaderPath,
                           std::string reduceShaderPath)
    : device_(&device), resourceHandles_(resourceHandles),
      descriptorAllocator_(&descriptorAllocator),
      initShaderPath_(std::move(initShaderPath)),
      reduceShaderPath_(std::move(reduceShaderPath)) {
    if (!resourceHandles_.surfaceDepth.valid() ||
        !resourceHandles_.visibilityHiZ.valid()) {
        throw std::invalid_argument("HiZBuildPass requires visibility images");
    }
    createDescriptorSetLayout();
    createDescriptors(resources);
}

HiZBuildPass::~HiZBuildPass() {
    freeDescriptors();
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     descriptorSetLayout_, nullptr);
    }
}

void HiZBuildPass::releaseViewportResources() {
    freeDescriptors();
}

void HiZBuildPass::onViewportResize(
    const RenderResourcePool &resources) {
    createDescriptors(resources);
}

void HiZBuildPass::setup(
    RenderGraphBuilder &builder,
    const RenderGraphBuildContext &context) const {
    const uint32_t mipCount = context.resources.mipLevelCount(
        resourceHandles_.visibilityHiZ);
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        builder.addNode("HiZBuild/Mip" + std::to_string(mip),
                        RgPassType::Compute, RgQueueClass::Compute, mip);
        if (mip == 0) {
            builder.useImage(
                {resourceHandles_.surfaceDepth,
                 RenderImageAccess::SampledRead,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL});
        } else {
            builder.useImage(
                {resourceHandles_.visibilityHiZ,
                 RenderImageAccess::SampledRead, VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_GENERAL},
                {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1u, 1, 0, 1});
        }
        builder.useImage(
            {resourceHandles_.visibilityHiZ,
             RenderImageAccess::StorageWrite, VK_IMAGE_LAYOUT_GENERAL,
             VK_IMAGE_LAYOUT_GENERAL},
            {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1});
    }
}

void HiZBuildPass::recordNode(
    RenderGraphPassContext &context, uint32_t localNodeIndex,
    const VisibilityFrame &visibility) {
    recordMip(context.frame, context.resources, visibility,
              localNodeIndex);
}

void HiZBuildPass::recordMip(
    const RenderFrameContext &frame,
    const RenderResourcePool &resources,
    const VisibilityFrame &visibility, uint32_t mip) {
    if (!frame.pipelineCache || !frame.view ||
        !frame.features.hiZRequired ||
        visibility.cpuStats.occlusionCandidates == 0 ||
        mip >= resources.mipLevelCount(resourceHandles_.visibilityHiZ))
        return;
    const VkPushConstantRange pushRange{
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(HiZPush)};
    ComputePipelineConfig config{};
    config.debugName = mip == 0 ? "Pipeline/Visibility/HiZInit"
                                : "Pipeline/Visibility/HiZReduce";
    config.computeShaderPath = mip == 0 ? initShaderPath_ : reduceShaderPath_;
    config.descriptorLayouts = {descriptorSetLayout_};
    config.pushConstants = {pushRange};
    ComputePipeline &pipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(config));
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.handle());
    const VkDescriptorSet set = sets_[frame.frameIndex].at(mip);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 0, 1, &set, 0, nullptr);
    const VkExtent2D destination =
        resources.mipExtent(resourceHandles_.visibilityHiZ, mip);
    const VkExtent2D source =
        mip == 0 ? resources.extent(resourceHandles_.surfaceDepth)
                 : resources.mipExtent(resourceHandles_.visibilityHiZ,
                                       mip - 1u);
    const HiZPush push{{source.width, source.height, destination.width,
                        destination.height}};
    vkCmdPushConstants(frame.cmd, pipeline.layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(frame.cmd, dispatchCount(destination.width),
                  dispatchCount(destination.height), 1);
}

void HiZBuildPass::createDescriptorSetLayout() {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &descriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, descriptorSetLayout_,
        "DescriptorLayout/VisibilityHiZ");
}

void HiZBuildPass::createDescriptors(
    const RenderResourcePool &resources) {
    const uint32_t mipCount =
        resources.mipLevelCount(resourceHandles_.visibilityHiZ);
    for (uint32_t frame = 0; frame < sets_.size(); ++frame) {
        sets_[frame].resize(mipCount);
        for (uint32_t mip = 0; mip < mipCount; ++mip) {
            VkDescriptorSet set = descriptorAllocator_->allocate(
                descriptorSetLayout_,
                {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                 {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}},
                "Visibility/HiZ/Frame" + std::to_string(frame) + "/Mip" +
                    std::to_string(mip));
            sets_[frame][mip] = set;
            VkDescriptorImageInfo source{};
            if (mip == 0) {
                source.sampler = resources.sampler(
                    resourceHandles_.surfaceDepthSampler);
                source.imageView =
                    resources.image(resourceHandles_.surfaceDepth, frame)
                        .imageView();
                source.imageLayout =
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            } else {
                source.sampler =
                    resources.sampler(resourceHandles_.visibilityHiZSampler);
                source.imageView = resources.mipView(
                    resourceHandles_.visibilityHiZ, frame, mip - 1u);
                source.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            }
            VkDescriptorImageInfo destination{};
            destination.imageView = resources.mipView(
                resourceHandles_.visibilityHiZ, frame, mip);
            destination.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            std::array<VkWriteDescriptorSet, 2> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = set;
            writes[0].dstBinding = 0;
            writes[0].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].descriptorCount = 1;
            writes[0].pImageInfo = &source;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = set;
            writes[1].dstBinding = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[1].descriptorCount = 1;
            writes[1].pImageInfo = &destination;
            vkUpdateDescriptorSets(device_->logicalDevice(),
                                   static_cast<uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }
    }
}

void HiZBuildPass::freeDescriptors() {
    for (auto &frameSets : sets_) {
        for (VkDescriptorSet set : frameSets)
            descriptorAllocator_->free(set);
        frameSets.clear();
    }
}

} // namespace vkr
