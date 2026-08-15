#include "render/pass/SsaoPass.h"

#include "render/pipeline/ComputePipeline.h"
#include "render/pipeline/ComputePipelineConfig.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/VulkanCheck.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"
#include "render/PipelineCache.h"
#include "render/RenderFrame.h"
#include "render/RenderGraph.h"
#include "render/RenderResourceRegistry.h"
#include "render/RenderView.h"

#include <array>
#include <glm/glm.hpp>
#include <stdexcept>
#include <utility>

namespace vkr {

namespace {

constexpr uint32_t kWorkgroupSize = 8;

uint32_t dispatchCount(uint32_t value) {
    return (value + kWorkgroupSize - 1u) / kWorkgroupSize;
}

uint32_t sampleCount(SsaoQuality quality) {
    switch (quality) {
    case SsaoQuality::Low:
        return 8;
    case SsaoQuality::Medium:
        return 16;
    case SsaoQuality::High:
        return 32;
    }
    return 16;
}

struct SsaoPush {
    glm::vec4 parameters{0.0f};
    glm::uvec4 dimensions{0u};
};

} // namespace

SsaoPass::SsaoPass(Device &device,
                   const RenderResourceRegistry &resources,
                   RendererResourceHandles resourceHandles,
                   DescriptorAllocator &descriptorAllocator,
                   VkDescriptorSetLayout globalDescriptorSetLayout,
                   std::string traceShaderPath,
                   std::string blurShaderPath)
    : device_(&device), resourceHandles_(resourceHandles),
      descriptorAllocator_(&descriptorAllocator),
      globalDescriptorSetLayout_(globalDescriptorSetLayout),
      traceShaderPath_(std::move(traceShaderPath)),
      blurShaderPath_(std::move(blurShaderPath)) {
    if (!resourceHandles_.surfaceDepth.valid() ||
        !resourceHandles_.surfaceNormalRoughness.valid() ||
        !resourceHandles_.ssaoRaw.valid() ||
        !resourceHandles_.ssaoTemp.valid() ||
        !resourceHandles_.ssaoFiltered.valid()) {
        throw std::invalid_argument("SsaoPass requires all SSAO resources");
    }
    createDescriptorSetLayout();
    createDescriptors(resources);
}

SsaoPass::~SsaoPass() {
    freeDescriptors();
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     descriptorSetLayout_, nullptr);
    }
}

void SsaoPass::releaseViewportResources() {
    freeDescriptors();
}

void SsaoPass::onViewportResize(const RenderResourceRegistry &resources) {
    createDescriptors(resources);
}

void SsaoPass::setup(RenderGraphBuilder &builder,
                     const RenderGraphBuildContext &) const {
    const auto readSurface = [&] {
        builder.useImage(
            {resourceHandles_.surfaceDepth, RenderImageAccess::SampledRead,
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL});
        builder.useImage(
            {resourceHandles_.surfaceNormalRoughness,
             RenderImageAccess::SampledRead,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    };

    builder.addNode("SSAO/Trace", RgPassType::Compute,
                    RgQueueClass::Compute, 0);
    readSurface();
    builder.useImage(
        {resourceHandles_.ssaoRaw, RenderImageAccess::StorageWrite,
         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});

    builder.addNode("SSAO/BilateralHorizontal", RgPassType::Compute,
                    RgQueueClass::Compute, 1);
    readSurface();
    builder.useImage(
        {resourceHandles_.ssaoRaw, RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    builder.useImage(
        {resourceHandles_.ssaoTemp, RenderImageAccess::StorageWrite,
         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});

    builder.addNode("SSAO/BilateralVertical", RgPassType::Compute,
                    RgQueueClass::Compute, 2);
    readSurface();
    builder.useImage(
        {resourceHandles_.ssaoTemp, RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    builder.useImage(
        {resourceHandles_.ssaoFiltered, RenderImageAccess::StorageWrite,
         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});

    builder.addNode("SSAO/Finalize", RgPassType::Compute,
                    RgQueueClass::Compute, 3);
    builder.useImage(
        {resourceHandles_.ssaoRaw, RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_GENERAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    builder.useImage(
        {resourceHandles_.ssaoFiltered, RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_GENERAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
}

void SsaoPass::recordNode(RenderGraphPassContext &context,
                          uint32_t localNodeIndex,
                          const VisibilityFrame &) {
    if (localNodeIndex < 3)
        recordStage(context.frame, context.resources, localNodeIndex);
}

void SsaoPass::recordStage(const RenderFrameContext &frame,
                           const RenderResourceRegistry &resources,
                           uint32_t stage) {
    if (!frame.pipelineCache || !frame.view || stage > 2)
        return;
    const uint32_t frameIndex = frame.frameIndex;

    const VkPushConstantRange pushRange{
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SsaoPush)};
    ComputePipelineConfig config{};
    config.debugName = stage == 0
                           ? "Pipeline/ScreenSpace/SSAO/Trace"
                           : "Pipeline/ScreenSpace/SSAO/Blur";
    config.computeShaderPath = stage == 0 ? traceShaderPath_ : blurShaderPath_;
    config.descriptorLayouts = {globalDescriptorSetLayout_,
                                descriptorSetLayout_};
    config.pushConstants = {pushRange};
    ComputePipeline &pipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(config));

    const VkExtent2D fullExtent =
        resources.extent(resourceHandles_.surfaceDepth);
    const VkExtent2D aoExtent = resources.extent(resourceHandles_.ssaoRaw);
    SsaoPush push{};
    push.parameters =
        glm::vec4(frame.view->settings.ssaoRadius,
                  frame.view->settings.ssaoBias,
                  frame.view->settings.ssaoIntensity,
                  frame.view->settings.ssaoPower);
    push.dimensions =
        glm::uvec4(sampleCount(frame.view->settings.ssaoQuality),
                   fullExtent.width, fullExtent.height, 0u);

    const std::array<VkDescriptorSet, 3> sets = {
        traceSets_[frameIndex], horizontalSets_[frameIndex],
        verticalSets_[frameIndex]};
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.handle());
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 0, 1,
                            &frame.globalDescriptorSet, 0, nullptr);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 1, 1, &sets[stage], 0,
                            nullptr);
    push.dimensions.w = stage == 2 ? 1u : 0u;
    vkCmdPushConstants(frame.cmd, pipeline.layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(frame.cmd, dispatchCount(aoExtent.width),
                  dispatchCount(aoExtent.height), 1);
}

void SsaoPass::createDescriptorSetLayout() {
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    for (uint32_t binding = 0; binding < 3; ++binding) {
        bindings[binding] = {
            binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &descriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, descriptorSetLayout_,
        "DescriptorLayout/SSAO");
}

void SsaoPass::createDescriptors(const RenderResourceRegistry &resources) {
    const VkSampler depthSampler =
        resources.sampler(resourceHandles_.surfaceDepthSampler);
    const VkSampler surfaceSampler =
        resources.sampler(resourceHandles_.surfaceDataSampler);
    const VkSampler aoSampler = resources.sampler(resourceHandles_.ssaoSampler);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        const VkImageView depth =
            resources.image(resourceHandles_.surfaceDepth, frame).imageView();
        const VkImageView surface =
            resources.image(resourceHandles_.surfaceNormalRoughness, frame)
                .imageView();
        const VkImageView raw =
            resources.image(resourceHandles_.ssaoRaw, frame).imageView();
        const VkImageView temp =
            resources.image(resourceHandles_.ssaoTemp, frame).imageView();
        const VkImageView filtered =
            resources.image(resourceHandles_.ssaoFiltered, frame).imageView();

        const auto allocateSet = [&](std::string suffix,
                                     VkImageView source,
                                     VkImageView destination) {
            VkDescriptorSet set = descriptorAllocator_->allocate(
                descriptorSetLayout_,
                {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
                 {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}},
                "SSAO/Frame" + std::to_string(frame) + "/" + suffix);
            std::array<VkDescriptorImageInfo, 4> infos{};
            infos[0] = {depthSampler, depth,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
            infos[1] = {surfaceSampler, surface,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            infos[2] = {aoSampler, source, VK_IMAGE_LAYOUT_GENERAL};
            infos[3] = {VK_NULL_HANDLE, destination,
                        VK_IMAGE_LAYOUT_GENERAL};
            std::array<VkWriteDescriptorSet, 4> writes{};
            for (uint32_t binding = 0; binding < writes.size(); ++binding) {
                writes[binding].sType =
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[binding].dstSet = set;
                writes[binding].dstBinding = binding;
                writes[binding].descriptorCount = 1;
                writes[binding].descriptorType =
                    binding == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                 : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[binding].pImageInfo = &infos[binding];
            }
            vkUpdateDescriptorSets(device_->logicalDevice(),
                                   static_cast<uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
            return set;
        };

        traceSets_[frame] = allocateSet("Trace", raw, raw);
        horizontalSets_[frame] = allocateSet("Horizontal", raw, temp);
        verticalSets_[frame] = allocateSet("Vertical", temp, filtered);
    }
}

void SsaoPass::freeDescriptors() {
    for (auto *sets : {&traceSets_, &horizontalSets_, &verticalSets_}) {
        for (VkDescriptorSet &set : *sets) {
            if (set != VK_NULL_HANDLE)
                descriptorAllocator_->free(set);
            set = VK_NULL_HANDLE;
        }
    }
}

} // namespace vkr
