#include "BloomPass.h"

#include "render/pipeline/ComputePipeline.h"
#include "render/pipeline/ComputePipelineConfig.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/VulkanCheck.h"
#include "render/FrameGpuData.h"
#include "render/PipelineCache.h"
#include "render/RenderFrame.h"
#include "render/RenderGraph.h"
#include "render/RenderResourceRegistry.h"
#include "render/RenderView.h"
#include "render/ShaderVariant.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vkr {

namespace {

constexpr uint32_t kWorkgroupSize = 8;

uint32_t dispatchCount(uint32_t extent) {
    return (extent + kWorkgroupSize - 1u) / kWorkgroupSize;
}

} // namespace

BloomPass::BloomPass(Device &device,
                     const RenderResourceRegistry &resources,
                     RendererResourceHandles resourceHandles,
                     DescriptorAllocator &descriptorAllocator,
                     std::string downsampleShaderPath,
                     std::string upsampleShaderPath)
    : device_(&device), resourceHandles_(resourceHandles),
      descriptorAllocator_(&descriptorAllocator),
      downsampleShaderPath_(std::move(downsampleShaderPath)),
      upsampleShaderPath_(std::move(upsampleShaderPath)) {
    if (!resourceHandles_.bloomLevels.front().valid() ||
        !resourceHandles_.bloomSampler.valid()) {
        throw std::invalid_argument(
            "BloomPass requires valid Bloom resources");
    }
    createDescriptorSetLayout();
    createDescriptors(resources);
}

BloomPass::~BloomPass() {
    freeDescriptors();
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     descriptorSetLayout_, nullptr);
    }
}

void BloomPass::setup(
    RenderGraphBuilder &builder,
    const RenderGraphBuildContext &context) const {
    const uint32_t levels = activeLevelCount(context.resources);
    for (uint32_t level = 0; level < levels; ++level) {
        builder.addNode("Bloom/Downsample/L" + std::to_string(level),
                        RgPassType::Compute, RgQueueClass::Compute, level);
        if (level == 0) {
            if (context.features.taaActive &&
                resourceHandles_.taaHistory.valid()) {
                builder.useImage(
                    {resourceHandles_.taaHistory,
                     RenderImageAccess::SampledRead,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            } else {
                builder.useImage(
                    {context.features.lightingCompositeRequired
                         ? resourceHandles_.compositedHdrColor
                         : resourceHandles_.hdrColor,
                     RenderImageAccess::SampledRead,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            }
        } else {
            builder.useImage(
                {resourceHandles_.bloomLevels[level - 1u],
                 RenderImageAccess::SampledRead, VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_GENERAL});
        }
        builder.useImage(
            {resourceHandles_.bloomLevels[level],
             RenderImageAccess::StorageWrite, VK_IMAGE_LAYOUT_GENERAL,
             VK_IMAGE_LAYOUT_GENERAL});
    }
    for (uint32_t sourceLevel = levels - 1u; sourceLevel > 0;
         --sourceLevel) {
        const uint32_t destinationLevel = sourceLevel - 1u;
        builder.addNode("Bloom/Upsample/L" +
                            std::to_string(sourceLevel) + "-L" +
                            std::to_string(destinationLevel),
                        RgPassType::Compute, RgQueueClass::Compute,
                        kLevelCount + destinationLevel);
        builder.useImage(
            {resourceHandles_.bloomLevels[sourceLevel],
             RenderImageAccess::SampledRead, VK_IMAGE_LAYOUT_GENERAL,
             VK_IMAGE_LAYOUT_GENERAL});
        builder.useImage(
            {resourceHandles_.bloomLevels[destinationLevel],
             RenderImageAccess::StorageReadWrite,
             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    }
}

void BloomPass::recordNode(
    RenderGraphPassContext &context, uint32_t localNodeIndex,
    const VisibilityFrame &) {
    if (localNodeIndex < kLevelCount)
        recordDownsample(context.frame, context.resources, localNodeIndex);
    else
        recordUpsample(context.frame, context.resources,
                       localNodeIndex - kLevelCount);
}

void BloomPass::releaseViewportResources() {
    freeDescriptors();
}

void BloomPass::onViewportResize(
    const RenderResourceRegistry &resources) {
    createDescriptors(resources);
}

void BloomPass::onResourceResidencyChanged(
    const RenderResourceRegistry &resources, uint32_t,
    const std::vector<RenderImageHandle> &createdImages) {
    const bool pyramidCreated = std::any_of(
        resourceHandles_.bloomLevels.begin(),
        resourceHandles_.bloomLevels.end(), [&](RenderImageHandle handle) {
            return std::find(createdImages.begin(), createdImages.end(),
                             handle) != createdImages.end();
        });
    if (!pyramidCreated)
        return;
    releaseViewportResources();
    onViewportResize(resources);
}

void BloomPass::recordDownsample(
    const RenderFrameContext &frame,
    const RenderResourceRegistry &resources, uint32_t level) {
    if (!frame.pipelineCache || !frame.view || !frame.shaderVariant ||
        level >= activeLevelCount(resources))
        return;
    const bool useTaa = frame.features.taaActive &&
                        resourceHandles_.taaHistory.valid();
    if (level == 0) {
        updatePrimarySource(
            resources, frame.frameIndex,
            useTaa ? resourceHandles_.taaHistory
                   : (frame.features.lightingCompositeRequired
                          ? resourceHandles_.compositedHdrColor
                          : resourceHandles_.hdrColor),
            useTaa ? resourceHandles_.taaSampler
                   : resourceHandles_.hdrSampler);
    }
    const VkPushConstantRange pushRange{
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BloomPushConstants)};
    ComputePipelineConfig config{};
    config.debugName = "Pipeline/Bloom/Downsample";
    config.computeShaderPath = downsampleShaderPath_;
    config.descriptorLayouts = {descriptorSetLayout_};
    config.pushConstants = {pushRange};
    ComputePipeline &pipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(config));
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.handle());
    const VkDescriptorSet set =
        downsampleSets_[frame.frameIndex][level];
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 0, 1, &set, 0, nullptr);
    BloomPushConstants push{};
    push.threshold = frame.view->settings.bloomThreshold;
    push.softKnee = frame.view->settings.bloomSoftKnee;
    push.applyThreshold = level == 0 ? 1u : 0u;
    vkCmdPushConstants(frame.cmd, pipeline.layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    const VkExtent2D extent =
        resources.extent(resourceHandles_.bloomLevels[level]);
    vkCmdDispatch(frame.cmd, dispatchCount(extent.width),
                  dispatchCount(extent.height), 1);
}

void BloomPass::recordUpsample(
    const RenderFrameContext &frame,
    const RenderResourceRegistry &resources,
    uint32_t destinationLevel) {
    const uint32_t levels = activeLevelCount(resources);
    if (!frame.pipelineCache || destinationLevel + 1u >= levels)
        return;
    const VkPushConstantRange pushRange{
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BloomPushConstants)};
    ComputePipelineConfig config{};
    config.debugName = "Pipeline/Bloom/Upsample";
    config.computeShaderPath = upsampleShaderPath_;
    config.descriptorLayouts = {descriptorSetLayout_};
    config.pushConstants = {pushRange};
    ComputePipeline &pipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(config));
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.handle());
    const VkDescriptorSet set =
        upsampleSets_[frame.frameIndex][destinationLevel];
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 0, 1, &set, 0, nullptr);
    BloomPushConstants push{};
    push.filterRadius = 1.0f;
    vkCmdPushConstants(frame.cmd, pipeline.layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    const VkExtent2D extent =
        resources.extent(resourceHandles_.bloomLevels[destinationLevel]);
    vkCmdDispatch(frame.cmd, dispatchCount(extent.width),
                  dispatchCount(extent.height), 1);
}

void BloomPass::updatePrimarySource(
    const RenderResourceRegistry &resources, uint32_t frameIndex,
    RenderImageHandle source, RenderSamplerHandle sampler) {
    VkDescriptorImageInfo info{
        resources.sampler(sampler),
        resources.image(source, frameIndex).imageView(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = downsampleSets_[frameIndex][0];
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device_->logicalDevice(), 1, &write, 0, nullptr);
}

void BloomPass::createDescriptorSetLayout() {
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
                                         nullptr,
                                         &descriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, descriptorSetLayout_,
        "Pass/Bloom/DescriptorSetLayout");
}

void BloomPass::createDescriptors(
    const RenderResourceRegistry &resources) {
    freeDescriptors();
    const VkSampler sampler =
        resources.sampler(resourceHandles_.bloomSampler);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        for (uint32_t level = 0; level < kLevelCount; ++level) {
            VkDescriptorSet &set = downsampleSets_[frame][level];
            set = descriptorAllocator_->allocate(
                descriptorSetLayout_,
                {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                 {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}},
                "Pass/Bloom/Downsample/Frame" + std::to_string(frame) +
                    "/Level" + std::to_string(level));

            const RenderImageHandle source =
                level == 0 ? resourceHandles_.hdrColor
                           : resourceHandles_.bloomLevels[level - 1];
            VkDescriptorImageInfo sourceInfo{};
            sourceInfo.sampler = sampler;
            sourceInfo.imageView =
                resources.image(source, frame).imageView();
            sourceInfo.imageLayout =
                level == 0 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                           : VK_IMAGE_LAYOUT_GENERAL;
            VkDescriptorImageInfo destinationInfo{};
            destinationInfo.imageView =
                resources
                    .image(resourceHandles_.bloomLevels[level], frame)
                    .imageView();
            destinationInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            std::array<VkWriteDescriptorSet, 2> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = set;
            writes[0].dstBinding = 0;
            writes[0].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].descriptorCount = 1;
            writes[0].pImageInfo = &sourceInfo;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = set;
            writes[1].dstBinding = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[1].descriptorCount = 1;
            writes[1].pImageInfo = &destinationInfo;
            vkUpdateDescriptorSets(device_->logicalDevice(),
                                   static_cast<uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }

        for (uint32_t destinationLevel = 0;
             destinationLevel + 1 < kLevelCount; ++destinationLevel) {
            VkDescriptorSet &set =
                upsampleSets_[frame][destinationLevel];
            set = descriptorAllocator_->allocate(
                descriptorSetLayout_,
                {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                 {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}},
                "Pass/Bloom/Upsample/Frame" + std::to_string(frame) +
                    "/Level" + std::to_string(destinationLevel));

            VkDescriptorImageInfo sourceInfo{};
            sourceInfo.sampler = sampler;
            sourceInfo.imageView =
                resources
                    .image(resourceHandles_
                               .bloomLevels[destinationLevel + 1],
                           frame)
                    .imageView();
            sourceInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkDescriptorImageInfo destinationInfo{};
            destinationInfo.imageView =
                resources
                    .image(resourceHandles_.bloomLevels[destinationLevel],
                           frame)
                    .imageView();
            destinationInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            std::array<VkWriteDescriptorSet, 2> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = set;
            writes[0].dstBinding = 0;
            writes[0].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].descriptorCount = 1;
            writes[0].pImageInfo = &sourceInfo;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = set;
            writes[1].dstBinding = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[1].descriptorCount = 1;
            writes[1].pImageInfo = &destinationInfo;
            vkUpdateDescriptorSets(device_->logicalDevice(),
                                   static_cast<uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }
    }
}

void BloomPass::freeDescriptors() {
    for (auto &sets : downsampleSets_) {
        for (VkDescriptorSet &set : sets) {
            if (set != VK_NULL_HANDLE)
                descriptorAllocator_->free(set);
            set = VK_NULL_HANDLE;
        }
    }
    for (auto &sets : upsampleSets_) {
        for (VkDescriptorSet &set : sets) {
            if (set != VK_NULL_HANDLE)
                descriptorAllocator_->free(set);
            set = VK_NULL_HANDLE;
        }
    }
}

uint32_t BloomPass::activeLevelCount(
    const RenderResourceRegistry &resources) const {
    uint32_t levels = 0;
    for (RenderImageHandle handle : resourceHandles_.bloomLevels) {
        ++levels;
        const VkExtent2D extent = resources.extent(handle);
        if (extent.width <= 2 && extent.height <= 2)
            break;
    }
    return std::max(1u, levels);
}

} // namespace vkr
