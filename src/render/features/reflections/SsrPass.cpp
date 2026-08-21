#include "render/features/reflections/SsrPass.h"
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
struct BlurPush { glm::uvec4 dimensions{}; };

uint32_t stepCount(SsrQuality quality) {
    switch (quality) {
    case SsrQuality::Low: return 24;
    case SsrQuality::Medium: return 48;
    case SsrQuality::High: return 96;
    }
    return 48;
}

uint64_t signature(const RenderSettings &settings) {
    uint64_t value = static_cast<uint64_t>(settings.ssrQuality);
    const auto add = [&](float input) {
        value ^= static_cast<uint64_t>(glm::floatBitsToUint(input)) +
                 0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
    };
    add(settings.ssrMaxDistance);
    add(settings.ssrThickness);
    add(settings.ssrMaxRoughness);
    add(settings.ssrIntensity);
    add(settings.ssrHistoryWeight);
    return value;
}

} // namespace

SsrPass::SsrPass(Device &device, const RenderResourcePool &resources,
                 RendererResourceHandles resourceHandles,
                 DescriptorAllocator &descriptorAllocator,
                 VkDescriptorSetLayout globalDescriptorSetLayout,
                 std::string traceShaderPath,
                 std::string temporalShaderPath,
                 std::string blurShaderPath)
    : device_(&device), resources_(resourceHandles),
      descriptorAllocator_(&descriptorAllocator),
      globalLayout_(globalDescriptorSetLayout),
      traceShaderPath_(std::move(traceShaderPath)),
      temporalShaderPath_(std::move(temporalShaderPath)),
      blurShaderPath_(std::move(blurShaderPath)) {
    if (!resources_.ssrRaw.valid() || !resources_.ssrHistory.valid() ||
        !resources_.ssrTemp.valid() || !resources_.ssrFiltered.valid() ||
        !resources_.ssrDebug.valid())
        throw std::invalid_argument("SsrPass requires all SSR resources");
    status_.supported = true;
    createLayouts();
    createDescriptors(resources);
    status_.extent = resources.extent(resources_.ssrFiltered);
}

SsrPass::~SsrPass() {
    freeDescriptors();
    for (VkDescriptorSetLayout layout :
         {traceLayout_, temporalLayout_, blurLayout_}) {
        if (layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device_->logicalDevice(), layout,
                                         nullptr);
    }
}

void SsrPass::setup(RenderGraphBuilder &builder,
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

    builder.addNode("SSR/Trace", RgPassType::Compute,
                    RgQueueClass::Compute, 0);
    surfaceReads();
    builder.useImage({depthHierarchyResources(resources_)
                          .image(DepthHierarchySemantic::Nearest),
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    builder.useImage({resources_.sceneColorPyramid,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    builder.useImage({resources_.ssrRaw, RenderImageAccess::StorageWrite,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});

    builder.addNode("SSR/Temporal", RgPassType::Compute,
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
    builder.useImage({resources_.ssrHistory,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      RenderImageFrame::Previous});
    builder.useImage({resources_.ssrRaw, RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    builder.useImage({resources_.ssrHistory,
                      RenderImageAccess::StorageWrite,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    builder.useImage({resources_.ssrDebug,
                      RenderImageAccess::StorageWrite,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});

    builder.addNode("SSR/BilateralHorizontal", RgPassType::Compute,
                    RgQueueClass::Compute, 2);
    surfaceReads();
    builder.useImage({resources_.ssrHistory,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    builder.useImage({resources_.ssrTemp, RenderImageAccess::StorageWrite,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});

    builder.addNode("SSR/BilateralVertical", RgPassType::Compute,
                    RgQueueClass::Compute, 3);
    surfaceReads();
    builder.useImage({resources_.ssrTemp, RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    builder.useImage({resources_.ssrFiltered,
                      RenderImageAccess::StorageWrite,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});

    builder.addNode("SSR/Finalize", RgPassType::Compute,
                    RgQueueClass::Compute, 4);
    for (RenderImageHandle handle :
         {resources_.ssrRaw, resources_.ssrHistory,
          resources_.ssrFiltered, resources_.ssrDebug}) {
        builder.useImage({handle, RenderImageAccess::SampledRead,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
}

void SsrPass::recordNode(RenderGraphPassContext &context,
                         uint32_t localNodeIndex,
                         const VisibilityFrame &visibility) {
    if (localNodeIndex == 0)
        beginFrame(context.frame, context.resources, visibility);
    if (localNodeIndex < 4)
        recordStage(context.frame, context.resources, localNodeIndex);
    else if (localNodeIndex == 4)
        finishFrame(context.frame, visibility);
}

void SsrPass::beginFrame(const RenderFrameContext &frame,
                         const RenderResourcePool &resources,
                         const VisibilityFrame &visibility) {
    status_.active = frame.features.ssrActive;
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
            status_.lastResetReason = "SSR was not executed continuously";
        else if (!generationMatches)
            status_.lastResetReason = "temporal generation changed";
        else if (!settingsMatch)
            status_.lastResetReason = "SSR settings changed";
        else
            status_.lastResetReason = "history is unavailable";
    } else {
        status_.lastResetReason.clear();
    }
    status_.extent = resources.extent(resources_.ssrFiltered);
}

void SsrPass::recordStage(const RenderFrameContext &frame,
                          const RenderResourcePool &resources,
                          uint32_t stage) {
    if (!frame.pipelineCache || !frame.view || stage > 3)
        return;
    const uint32_t current = frame.frameIndex;
    const VkExtent2D full = resources.extent(resources_.surfaceDepth);
    const VkExtent2D half = resources.extent(resources_.ssrFiltered);

    if (stage == 0) {
        TracePush push{};
        push.parameters = {frame.view->settings.ssrMaxDistance,
                           frame.view->settings.ssrThickness,
                           frame.view->settings.ssrMaxRoughness,
                           frame.view->settings.ssrIntensity};
        push.dimensions = {stepCount(frame.view->settings.ssrQuality), 4,
                           full.width, full.height};
        push.sampling = {
            float(resources.mipLevelCount(
                      depthHierarchyResources(resources_)
                          .image(DepthHierarchySemantic::Nearest)) - 1),
            float(resources.mipLevelCount(resources_.sceneColorPyramid) - 1),
            float(frame.submissionSerial & 7u), 0.0f};
        ComputePipelineConfig config{};
        config.debugName = "Pipeline/ScreenSpace/SSR/Trace";
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
        push.parameters = {frame.view->settings.ssrHistoryWeight,
                           0.01f, 0.8f, 0.0f};
        push.dimensions = {full.width, full.height,
                           currentHistoryValid_ ? 1u : 0u, 0u};
        ComputePipelineConfig config{};
        config.debugName = "Pipeline/ScreenSpace/SSR/Temporal";
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

    BlurPush push{{full.width, full.height, stage == 2 ? 0u : 1u, 0u}};
    ComputePipelineConfig config{};
    config.debugName = "Pipeline/ScreenSpace/SSR/Blur";
    config.computeShaderPath = blurShaderPath_;
    config.descriptorLayouts = {globalLayout_, blurLayout_};
    config.pushConstants = {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push)}};
    ComputePipeline &pipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(config));
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.handle());
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 0, 1,
                            &frame.globalDescriptorSet, 0, nullptr);
    const VkDescriptorSet set = stage == 2 ? horizontalSets_[current]
                                           : verticalSets_[current];
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 1, 1, &set, 0, nullptr);
    vkCmdPushConstants(frame.cmd, pipeline.layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(frame.cmd, groups(half.width), groups(half.height), 1);
}

void SsrPass::finishFrame(const RenderFrameContext &frame,
                          const VisibilityFrame &visibility) {
    historyWritten_[frame.frameIndex] = true;
    lastExecutionSerial_ = frame.submissionSerial;
    lastHistoryGeneration_ = visibility.history.historyGeneration;
    lastSettingsSignature_ = currentSettingsSignature_;
}

void SsrPass::releaseViewportResources() {
    freeDescriptors();
    historyWritten_.fill(false);
    lastExecutionSerial_ = 0;
    status_.active = false;
    status_.historyValid = false;
    status_.lastResetReason = "viewport resized";
}

void SsrPass::onViewportResize(const RenderResourcePool &resources) {
    createDescriptors(resources);
    status_.extent = resources.extent(resources_.ssrFiltered);
}

void SsrPass::createLayouts() {
    std::array<VkDescriptorSetLayoutBinding, 5> trace{};
    for (uint32_t i = 0; i < 4; ++i)
        trace[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    trace[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = uint32_t(trace.size());
    info.pBindings = trace.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &traceLayout_));

    std::array<VkDescriptorSetLayoutBinding, 9> temporal{};
    for (uint32_t i = 0; i < 7; ++i)
        temporal[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    for (uint32_t i = 7; i < 9; ++i)
        temporal[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    info.bindingCount = uint32_t(temporal.size());
    info.pBindings = temporal.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &temporalLayout_));

    std::array<VkDescriptorSetLayoutBinding, 4> blur{};
    for (uint32_t i = 0; i < 3; ++i)
        blur[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    blur[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
               VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    info.bindingCount = uint32_t(blur.size());
    info.pBindings = blur.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &blurLayout_));
}

void SsrPass::createDescriptors(const RenderResourcePool &registry) {
    const VkSampler depthSampler = registry.sampler(resources_.surfaceDepthSampler);
    const VkSampler surfaceSampler = registry.sampler(resources_.surfaceDataSampler);
    const VkSampler pyramidSampler = registry.sampler(resources_.screenPyramidSampler);
    const VkSampler ssrSampler = registry.sampler(resources_.ssrSampler);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        const auto view = [&](RenderImageHandle handle) {
            return registry.image(handle, frame).imageView();
        };
        const VkImageView depth = view(resources_.surfaceDepth);
        const VkImageView normal = view(resources_.surfaceNormalRoughness);
        const VkImageView motion = view(resources_.surfaceMotion);
        const VkImageView raw = view(resources_.ssrRaw);
        const VkImageView history = view(resources_.ssrHistory);
        const VkImageView temp = view(resources_.ssrTemp);
        const VkImageView filtered = view(resources_.ssrFiltered);
        const VkImageView debug = view(resources_.ssrDebug);

        traceSets_[frame] = descriptorAllocator_->allocate(
            traceLayout_, {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
                           {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}},
            "SSR/Trace/Frame" + std::to_string(frame));
        std::array<VkDescriptorImageInfo, 5> traceInfos = {{
            {depthSampler, depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {surfaceSampler, normal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {pyramidSampler,
             view(depthHierarchyResources(resources_)
                      .image(DepthHierarchySemantic::Nearest)),
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {pyramidSampler, view(resources_.sceneColorPyramid), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, raw, VK_IMAGE_LAYOUT_GENERAL}}};
        std::array<VkWriteDescriptorSet, 5> traceWrites{};
        for (uint32_t i = 0; i < traceWrites.size(); ++i) {
            traceWrites[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            traceWrites[i].dstSet = traceSets_[frame];
            traceWrites[i].dstBinding = i;
            traceWrites[i].descriptorCount = 1;
            traceWrites[i].descriptorType = i < 4
                ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            traceWrites[i].pImageInfo = &traceInfos[i];
        }
        vkUpdateDescriptorSets(device_->logicalDevice(), uint32_t(traceWrites.size()),
                               traceWrites.data(), 0, nullptr);

        temporalSets_[frame] = descriptorAllocator_->allocate(
            temporalLayout_, {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 7},
                              {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}},
            "SSR/Temporal/Frame" + std::to_string(frame));
        std::array<VkDescriptorImageInfo, 9> temporalInfos = {{
            {ssrSampler, raw, VK_IMAGE_LAYOUT_GENERAL},
            {depthSampler, depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {surfaceSampler, normal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {surfaceSampler, motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {depthSampler, registry.previousImage(resources_.surfaceDepth, frame).imageView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {surfaceSampler, registry.previousImage(resources_.surfaceNormalRoughness, frame).imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {ssrSampler, registry.previousImage(resources_.ssrHistory, frame).imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, history, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, debug, VK_IMAGE_LAYOUT_GENERAL}}};
        std::array<VkWriteDescriptorSet, 9> temporalWrites{};
        for (uint32_t i = 0; i < temporalWrites.size(); ++i) {
            temporalWrites[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            temporalWrites[i].dstSet = temporalSets_[frame];
            temporalWrites[i].dstBinding = i;
            temporalWrites[i].descriptorCount = 1;
            temporalWrites[i].descriptorType = i < 7
                ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            temporalWrites[i].pImageInfo = &temporalInfos[i];
        }
        vkUpdateDescriptorSets(device_->logicalDevice(), uint32_t(temporalWrites.size()),
                               temporalWrites.data(), 0, nullptr);

        const auto makeBlur = [&](std::string name, VkImageView source,
                                  VkImageView destination) {
            VkDescriptorSet set = descriptorAllocator_->allocate(
                blurLayout_, {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
                              {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}},
                "SSR/" + name + "/Frame" + std::to_string(frame));
            std::array<VkDescriptorImageInfo, 4> infos = {{
                {depthSampler, depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
                {surfaceSampler, normal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {ssrSampler, source, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, destination, VK_IMAGE_LAYOUT_GENERAL}}};
            std::array<VkWriteDescriptorSet, 4> writes{};
            for (uint32_t i = 0; i < writes.size(); ++i) {
                writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[i].dstSet = set;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = i < 3
                    ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                    : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[i].pImageInfo = &infos[i];
            }
            vkUpdateDescriptorSets(device_->logicalDevice(), uint32_t(writes.size()),
                                   writes.data(), 0, nullptr);
            return set;
        };
        horizontalSets_[frame] = makeBlur("Horizontal", history, temp);
        verticalSets_[frame] = makeBlur("Vertical", temp, filtered);
    }
}

void SsrPass::freeDescriptors() {
    for (auto *sets : {&traceSets_, &temporalSets_, &horizontalSets_,
                       &verticalSets_}) {
        for (VkDescriptorSet &set : *sets) {
            if (set != VK_NULL_HANDLE) descriptorAllocator_->free(set);
            set = VK_NULL_HANDLE;
        }
    }
}

void SsrPass::updateTemporalHistoryDescriptors(
    const RenderResourcePool &registry, uint32_t current,
    uint32_t previous, bool historyValid) {
    const VkSampler depthSampler =
        registry.sampler(resources_.surfaceDepthSampler);
    const VkSampler surfaceSampler =
        registry.sampler(resources_.surfaceDataSampler);
    const VkSampler ssrSampler = registry.sampler(resources_.ssrSampler);
    std::array<VkDescriptorImageInfo, 3> infos = {{
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
        {ssrSampler,
         historyValid
             ? registry.image(resources_.ssrHistory, previous).imageView()
             : registry.image(resources_.ssrRaw, current).imageView(),
         historyValid ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                      : VK_IMAGE_LAYOUT_GENERAL}}};
    std::array<VkWriteDescriptorSet, 3> writes{};
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
