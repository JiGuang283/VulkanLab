#include "render/features/global_illumination/SsgiPass.h"
#include "render/features/surface/DepthHierarchyResources.h"

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
#include "render/frame/RenderView.h"
#include "render/features/shadows_visibility/Visibility.h"

#include <glm/glm.hpp>
#include <stdexcept>
#include <utility>

namespace vkr {
namespace {

constexpr uint32_t kWorkgroupSize = 8;
uint32_t groups(uint32_t value) {
    return (value + kWorkgroupSize - 1u) / kWorkgroupSize;
}
VkImageSubresourceRange colorRange() {
    return {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
}

struct TracePush {
    glm::vec4 parameters{};
    glm::uvec4 dimensions{};
    glm::vec4 sampling{};
};
struct TemporalPush {
    glm::vec4 parameters{};
    glm::uvec4 dimensions{};
};
struct FilterPush { glm::uvec4 dimensions{}; };

glm::uvec2 samplingBudget(SsgiQuality quality) {
    switch (quality) {
    case SsgiQuality::Low: return {4u, 12u};
    case SsgiQuality::Medium: return {6u, 20u};
    case SsgiQuality::High: return {8u, 32u};
    }
    return {6u, 20u};
}

uint64_t signature(const RenderSettings &settings) {
    uint64_t value = static_cast<uint64_t>(settings.ssgiQuality);
    const auto add = [&](float input) {
        value ^= static_cast<uint64_t>(glm::floatBitsToUint(input)) +
                 0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
    };
    add(settings.ssgiMaxDistance);
    add(settings.ssgiThickness);
    add(settings.ssgiIntensity);
    add(settings.ssgiRadianceClamp);
    add(settings.ssgiHistoryWeight);
    return value;
}

} // namespace

SsgiPass::SsgiPass(Device &device,
                   const RenderResourcePool &resources,
                   RendererResourceHandles resourceHandles,
                   DescriptorAllocator &descriptorAllocator,
                   VkDescriptorSetLayout globalDescriptorSetLayout,
                   std::string traceShaderPath,
                   std::string temporalShaderPath,
                   std::string filterShaderPath)
    : device_(&device), resources_(resourceHandles),
      descriptorAllocator_(&descriptorAllocator),
      globalLayout_(globalDescriptorSetLayout),
      traceShaderPath_(std::move(traceShaderPath)),
      temporalShaderPath_(std::move(temporalShaderPath)),
      filterShaderPath_(std::move(filterShaderPath)) {
    if (!resources_.surfaceAlbedoMetallic.valid() ||
        !resources_.ssgiRaw.valid() || !resources_.ssgiHistory.valid() ||
        !resources_.ssgiMoments.valid() || !resources_.ssgiTemp.valid() ||
        !resources_.ssgiFiltered.valid() || !resources_.ssgiDebug.valid()) {
        throw std::invalid_argument("SsgiPass requires all SSGI resources");
    }
    status_.supported = true;
    createLayouts();
    createDescriptors(resources);
    status_.extent = resources.extent(resources_.ssgiFiltered);
}

SsgiPass::~SsgiPass() {
    freeDescriptors();
    for (VkDescriptorSetLayout layout :
         {traceLayout_, temporalLayout_, filterLayout_}) {
        if (layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_->logicalDevice(), layout,
                                         nullptr);
        }
    }
}

void SsgiPass::setup(RenderGraphBuilder &builder,
                     const RenderGraphBuildContext &) const {
    const auto surfaceReads = [&] {
        builder.useImage({resources_.surfaceDepth,
                          RenderImageAccess::SampledRead,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL});
        builder.useImage({resources_.surfaceNormalRoughness,
                          RenderImageAccess::SampledRead,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    };

    builder.addNode("SSGI/Trace", RgPassType::Compute,
                    RgQueueClass::Compute, 0);
    surfaceReads();
    builder.useImage({resources_.surfaceAlbedoMetallic,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    builder.useImage({depthHierarchyResources(resources_)
                          .image(DepthHierarchySemantic::Nearest),
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    builder.useImage({resources_.sceneColorPyramid,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    builder.useImage({resources_.ssgiRaw, RenderImageAccess::StorageWrite,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});

    builder.addNode("SSGI/Temporal", RgPassType::Compute,
                    RgQueueClass::Compute, 1);
    surfaceReads();
    builder.useImage({resources_.surfaceMotion,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    builder.useImage({resources_.surfaceDepth,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                      RenderImageFrame::Previous});
    builder.useImage({resources_.surfaceNormalRoughness,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      RenderImageFrame::Previous});
    builder.useImage({resources_.ssgiHistory,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      RenderImageFrame::Previous});
    builder.useImage({resources_.ssgiMoments,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      RenderImageFrame::Previous});
    builder.useImage({resources_.ssgiRaw, RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    for (RenderImageHandle handle :
         {resources_.ssgiHistory, resources_.ssgiMoments,
          resources_.ssgiDebug}) {
        builder.useImage({handle, RenderImageAccess::StorageWrite,
                          VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    }

    builder.addNode("SSGI/BilateralHorizontal", RgPassType::Compute,
                    RgQueueClass::Compute, 2);
    surfaceReads();
    builder.useImage({resources_.ssgiMoments,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    builder.useImage({resources_.ssgiHistory,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    builder.useImage({resources_.ssgiTemp, RenderImageAccess::StorageWrite,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});

    builder.addNode("SSGI/BilateralVertical", RgPassType::Compute,
                    RgQueueClass::Compute, 3);
    surfaceReads();
    builder.useImage({resources_.ssgiMoments,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    builder.useImage({resources_.ssgiTemp, RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    builder.useImage({resources_.ssgiFiltered,
                      RenderImageAccess::StorageWrite,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});

    builder.addNode("SSGI/Finalize", RgPassType::Compute,
                    RgQueueClass::Compute, 4);
    for (RenderImageHandle handle :
         {resources_.ssgiRaw, resources_.ssgiHistory,
          resources_.ssgiMoments, resources_.ssgiFiltered,
          resources_.ssgiDebug}) {
        builder.useImage({handle, RenderImageAccess::SampledRead,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
}

void SsgiPass::recordNode(RenderGraphPassContext &context,
                          uint32_t localNodeIndex,
                          const VisibilityFrame &visibility) {
    if (localNodeIndex == 0)
        beginFrame(context.frame, context.resources, visibility);
    if (localNodeIndex < 4)
        recordStage(context.frame, context.resources, localNodeIndex);
    else if (localNodeIndex == 4)
        finishFrame(context.frame, visibility);
}

void SsgiPass::beginFrame(const RenderFrameContext &frame,
                          const RenderResourcePool &resources,
                          const VisibilityFrame &visibility) {
    status_.active = frame.features.ssgiActive;
    if (!frame.view)
        return;
    const uint32_t previous =
        (frame.frameIndex + resources.frameCount() - 1u) %
        resources.frameCount();
    currentSettingsSignature_ = signature(frame.view->settings);
    const bool contiguous = lastExecutionSerial_ != 0 &&
                            lastExecutionSerial_ + 1u ==
                                frame.submissionSerial;
    const bool generationMatches =
        lastExecutionSerial_ != 0 &&
        lastHistoryGeneration_ == visibility.history.historyGeneration;
    const bool settingsMatch =
        lastExecutionSerial_ != 0 &&
        lastSettingsSignature_ == currentSettingsSignature_;
    currentHistoryValid_ = visibility.history.globalValid && contiguous &&
                           generationMatches && settingsMatch &&
                           historyWritten_[previous];
    updateTemporalHistoryDescriptors(resources, frame.frameIndex, previous,
                                     currentHistoryValid_);
    status_.historyValid = currentHistoryValid_;
    status_.historyGeneration = visibility.history.historyGeneration;
    status_.lastFrameSerial = frame.submissionSerial;
    if (!currentHistoryValid_) {
        if (!visibility.history.invalidationReason.empty())
            status_.lastResetReason = visibility.history.invalidationReason;
        else if (!contiguous)
            status_.lastResetReason = "SSGI was not executed continuously";
        else if (!generationMatches)
            status_.lastResetReason = "temporal generation changed";
        else if (!settingsMatch)
            status_.lastResetReason = "SSGI settings changed";
        else
            status_.lastResetReason = "history is unavailable";
    } else {
        status_.lastResetReason.clear();
    }
    status_.extent = resources.extent(resources_.ssgiFiltered);
}

void SsgiPass::recordStage(const RenderFrameContext &frame,
                           const RenderResourcePool &resources,
                           uint32_t stage) {
    if (!frame.pipelineCache || !frame.view || stage > 3)
        return;
    const uint32_t current = frame.frameIndex;
    const VkExtent2D full = resources.extent(resources_.surfaceDepth);
    const VkExtent2D half = resources.extent(resources_.ssgiFiltered);

    if (stage == 0) {
        const glm::uvec2 budget =
            samplingBudget(frame.view->settings.ssgiQuality);
        TracePush push{};
        push.parameters = {frame.view->settings.ssgiMaxDistance,
                           frame.view->settings.ssgiThickness,
                           frame.view->settings.ssgiIntensity,
                           frame.view->settings.ssgiRadianceClamp};
        push.dimensions = {budget.x, budget.y, full.width, full.height};
        push.sampling = {
            float(resources.mipLevelCount(
                      depthHierarchyResources(resources_)
                          .image(DepthHierarchySemantic::Nearest)) - 1u),
            float(resources.mipLevelCount(resources_.sceneColorPyramid) - 1u),
            float(frame.submissionSerial & 7u), 0.0f};
        ComputePipelineConfig config{};
        config.debugName = "Pipeline/ScreenSpace/SSGI/Trace";
        config.computeShaderPath = traceShaderPath_;
        config.descriptorLayouts = {globalLayout_, traceLayout_};
        config.pushConstants = {{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                 sizeof(push)}};
        ComputePipeline &pipeline =
            frame.pipelineCache->getOrCreateCompute(std::move(config));
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipeline.handle());
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline.layout(), 0, 1,
                                &frame.globalDescriptorSet, 0, nullptr);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline.layout(), 1, 1,
                                &traceSets_[current], 0, nullptr);
        vkCmdPushConstants(frame.cmd, pipeline.layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        vkCmdDispatch(frame.cmd, groups(half.width), groups(half.height), 1);
        return;
    }

    if (stage == 1) {
        TemporalPush push{};
        push.parameters = {frame.view->settings.ssgiHistoryWeight,
                           0.01f, 0.8f,
                           frame.view->settings.ssgiRadianceClamp};
        push.dimensions = {full.width, full.height,
                           currentHistoryValid_ ? 1u : 0u, 0u};
        ComputePipelineConfig config{};
        config.debugName = "Pipeline/ScreenSpace/SSGI/Temporal";
        config.computeShaderPath = temporalShaderPath_;
        config.descriptorLayouts = {temporalLayout_};
        config.pushConstants = {{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                 sizeof(push)}};
        ComputePipeline &pipeline =
            frame.pipelineCache->getOrCreateCompute(std::move(config));
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipeline.handle());
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline.layout(), 0, 1,
                                &temporalSets_[current], 0, nullptr);
        vkCmdPushConstants(frame.cmd, pipeline.layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        vkCmdDispatch(frame.cmd, groups(half.width), groups(half.height), 1);
        return;
    }

    FilterPush push{{full.width, full.height, stage == 2 ? 1u : 2u, 0u}};
    ComputePipelineConfig config{};
    config.debugName = "Pipeline/ScreenSpace/SSGI/Filter";
    config.computeShaderPath = filterShaderPath_;
    config.descriptorLayouts = {filterLayout_};
    config.pushConstants = {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push)}};
    ComputePipeline &pipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(config));
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.handle());
    const VkDescriptorSet set = stage == 2 ? firstFilterSets_[current]
                                           : secondFilterSets_[current];
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(frame.cmd, pipeline.layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(frame.cmd, groups(half.width), groups(half.height), 1);
}

void SsgiPass::finishFrame(const RenderFrameContext &frame,
                           const VisibilityFrame &visibility) {
    historyWritten_[frame.frameIndex] = true;
    lastExecutionSerial_ = frame.submissionSerial;
    lastHistoryGeneration_ = visibility.history.historyGeneration;
    lastSettingsSignature_ = currentSettingsSignature_;
}

void SsgiPass::releaseViewportResources() {
    freeDescriptors();
    historyWritten_.fill(false);
    lastExecutionSerial_ = 0;
    status_.active = false;
    status_.historyValid = false;
    status_.lastResetReason = "viewport resized";
}

void SsgiPass::onViewportResize(const RenderResourcePool &resources) {
    createDescriptors(resources);
    status_.extent = resources.extent(resources_.ssgiFiltered);
}

void SsgiPass::createLayouts() {
    std::array<VkDescriptorSetLayoutBinding, 6> trace{};
    for (uint32_t index = 0; index < 5; ++index) {
        trace[index] = {index, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                        VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    trace[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<uint32_t>(trace.size());
    info.pBindings = trace.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &traceLayout_));

    std::array<VkDescriptorSetLayoutBinding, 11> temporal{};
    for (uint32_t index = 0; index < 8; ++index) {
        temporal[index] = {
            index, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    for (uint32_t index = 8; index < 11; ++index) {
        temporal[index] = {index, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                           VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    info.bindingCount = static_cast<uint32_t>(temporal.size());
    info.pBindings = temporal.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &temporalLayout_));

    std::array<VkDescriptorSetLayoutBinding, 5> filter{};
    for (uint32_t index = 0; index < 4; ++index) {
        filter[index] = {index, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    filter[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    info.bindingCount = static_cast<uint32_t>(filter.size());
    info.pBindings = filter.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &filterLayout_));
}

void SsgiPass::createDescriptors(const RenderResourcePool &registry) {
    const VkSampler depthSampler =
        registry.sampler(resources_.surfaceDepthSampler);
    const VkSampler surfaceSampler =
        registry.sampler(resources_.surfaceDataSampler);
    const VkSampler pyramidSampler =
        registry.sampler(resources_.screenPyramidSampler);
    const VkSampler ssgiSampler = registry.sampler(resources_.ssgiSampler);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        const auto view = [&](RenderImageHandle handle) {
            return registry.image(handle, frame).imageView();
        };
        const VkImageView depth = view(resources_.surfaceDepth);
        const VkImageView normal = view(resources_.surfaceNormalRoughness);
        const VkImageView motion = view(resources_.surfaceMotion);
        const VkImageView albedo = view(resources_.surfaceAlbedoMetallic);
        const VkImageView raw = view(resources_.ssgiRaw);
        const VkImageView history = view(resources_.ssgiHistory);
        const VkImageView moments = view(resources_.ssgiMoments);
        const VkImageView temp = view(resources_.ssgiTemp);
        const VkImageView filtered = view(resources_.ssgiFiltered);
        const VkImageView debug = view(resources_.ssgiDebug);

        traceSets_[frame] = descriptorAllocator_->allocate(
            traceLayout_, {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5},
                           {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}},
            "SSGI/Trace/Frame" + std::to_string(frame));
        std::array<VkDescriptorImageInfo, 6> traceInfos = {{
            {depthSampler, depth,
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {surfaceSampler, normal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {surfaceSampler, albedo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {pyramidSampler,
             view(depthHierarchyResources(resources_)
                      .image(DepthHierarchySemantic::Nearest)),
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {pyramidSampler, view(resources_.sceneColorPyramid),
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, raw, VK_IMAGE_LAYOUT_GENERAL}}};
        std::array<VkWriteDescriptorSet, 6> traceWrites{};
        for (uint32_t index = 0; index < traceWrites.size(); ++index) {
            traceWrites[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            traceWrites[index].dstSet = traceSets_[frame];
            traceWrites[index].dstBinding = index;
            traceWrites[index].descriptorCount = 1;
            traceWrites[index].descriptorType =
                index < 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                          : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            traceWrites[index].pImageInfo = &traceInfos[index];
        }
        vkUpdateDescriptorSets(device_->logicalDevice(),
                               static_cast<uint32_t>(traceWrites.size()),
                               traceWrites.data(), 0, nullptr);

        temporalSets_[frame] = descriptorAllocator_->allocate(
            temporalLayout_,
            {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8},
             {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3}},
            "SSGI/Temporal/Frame" + std::to_string(frame));
        std::array<VkDescriptorImageInfo, 11> temporalInfos = {{
            {ssgiSampler, raw, VK_IMAGE_LAYOUT_GENERAL},
            {depthSampler, depth,
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {surfaceSampler, normal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {surfaceSampler, motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {depthSampler,
             registry.previousImage(resources_.surfaceDepth, frame)
                 .imageView(),
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {surfaceSampler,
             registry.previousImage(resources_.surfaceNormalRoughness, frame)
                 .imageView(),
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {ssgiSampler,
             registry.previousImage(resources_.ssgiHistory, frame)
                 .imageView(),
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {ssgiSampler,
             registry.previousImage(resources_.ssgiMoments, frame)
                 .imageView(),
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, history, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, moments, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, debug, VK_IMAGE_LAYOUT_GENERAL}}};
        std::array<VkWriteDescriptorSet, 11> temporalWrites{};
        for (uint32_t index = 0; index < temporalWrites.size(); ++index) {
            temporalWrites[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            temporalWrites[index].dstSet = temporalSets_[frame];
            temporalWrites[index].dstBinding = index;
            temporalWrites[index].descriptorCount = 1;
            temporalWrites[index].descriptorType =
                index < 8 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                          : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            temporalWrites[index].pImageInfo = &temporalInfos[index];
        }
        vkUpdateDescriptorSets(device_->logicalDevice(),
                               static_cast<uint32_t>(temporalWrites.size()),
                               temporalWrites.data(), 0, nullptr);

        const auto makeFilter = [&](std::string name, VkImageView source,
                                    VkImageView destination) {
            VkDescriptorSet set = descriptorAllocator_->allocate(
                filterLayout_,
                {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
                 {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}},
                "SSGI/" + name + "/Frame" + std::to_string(frame));
            std::array<VkDescriptorImageInfo, 5> infos = {{
                {depthSampler, depth,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
                {surfaceSampler, normal,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {ssgiSampler, moments, VK_IMAGE_LAYOUT_GENERAL},
                {ssgiSampler, source, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, destination, VK_IMAGE_LAYOUT_GENERAL}}};
            std::array<VkWriteDescriptorSet, 5> writes{};
            for (uint32_t index = 0; index < writes.size(); ++index) {
                writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[index].dstSet = set;
                writes[index].dstBinding = index;
                writes[index].descriptorCount = 1;
                writes[index].descriptorType =
                    index < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                              : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[index].pImageInfo = &infos[index];
            }
            vkUpdateDescriptorSets(device_->logicalDevice(),
                                   static_cast<uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
            return set;
        };
        firstFilterSets_[frame] =
            makeFilter("Filter1", history, temp);
        secondFilterSets_[frame] =
            makeFilter("Filter2", temp, filtered);
    }
}

void SsgiPass::freeDescriptors() {
    for (auto *sets : {&traceSets_, &temporalSets_, &firstFilterSets_,
                       &secondFilterSets_}) {
        for (VkDescriptorSet &set : *sets) {
            if (set != VK_NULL_HANDLE)
                descriptorAllocator_->free(set);
            set = VK_NULL_HANDLE;
        }
    }
}

void SsgiPass::updateTemporalHistoryDescriptors(
    const RenderResourcePool &registry, uint32_t current,
    uint32_t previous, bool historyValid) {
    const VkSampler depthSampler =
        registry.sampler(resources_.surfaceDepthSampler);
    const VkSampler surfaceSampler =
        registry.sampler(resources_.surfaceDataSampler);
    const VkSampler ssgiSampler = registry.sampler(resources_.ssgiSampler);
    const VkImageView fallback =
        registry.image(resources_.ssgiRaw, current).imageView();
    std::array<VkDescriptorImageInfo, 4> infos = {{
        {depthSampler,
         historyValid
             ? registry.image(resources_.surfaceDepth, previous).imageView()
             : registry.image(resources_.surfaceDepth, current).imageView(),
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        {surfaceSampler,
         historyValid
             ? registry.image(resources_.surfaceNormalRoughness, previous)
                   .imageView()
             : registry.image(resources_.surfaceNormalRoughness, current)
                   .imageView(),
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {ssgiSampler,
         historyValid
             ? registry.image(resources_.ssgiHistory, previous).imageView()
             : fallback,
         historyValid ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                      : VK_IMAGE_LAYOUT_GENERAL},
        {ssgiSampler,
         historyValid
             ? registry.image(resources_.ssgiMoments, previous).imageView()
             : fallback,
         historyValid ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                      : VK_IMAGE_LAYOUT_GENERAL}}};
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (uint32_t index = 0; index < writes.size(); ++index) {
        writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[index].dstSet = temporalSets_[current];
        writes[index].dstBinding = index + 4u;
        writes[index].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[index].descriptorCount = 1;
        writes[index].pImageInfo = &infos[index];
    }
    vkUpdateDescriptorSets(device_->logicalDevice(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

} // namespace vkr
