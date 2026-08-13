#include "render/pass/GtaoPass.h"

#include "core/ComputePipeline.h"
#include "core/ComputePipelineConfig.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuBarrier.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/VulkanCheck.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"
#include "render/PipelineCache.h"
#include "render/RenderFrame.h"
#include "render/RenderGraph.h"
#include "render/RenderView.h"
#include "render/Visibility.h"

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

struct GtaoTracePush {
    glm::vec4 parameters{0.0f};
    glm::uvec4 dimensions{0u};
    glm::vec4 sampling{0.0f};
};

struct GtaoTemporalPush {
    glm::vec4 parameters{0.0f};
    glm::uvec4 dimensions{0u};
};

struct BlurPush {
    glm::vec4 parameters{0.0f};
    glm::uvec4 dimensions{0u};
};

std::pair<uint32_t, uint32_t> qualitySamples(GtaoQuality quality) {
    switch (quality) {
    case GtaoQuality::Low:
        return {2u, 2u};
    case GtaoQuality::Medium:
        return {3u, 4u};
    case GtaoQuality::High:
        return {4u, 6u};
    }
    return {3u, 4u};
}

uint64_t settingsSignature(const GtaoSettings &settings) {
    uint64_t result = static_cast<uint64_t>(settings.quality);
    const auto add = [&](float value) {
        result ^= static_cast<uint64_t>(glm::floatBitsToUint(value)) +
                  0x9e3779b97f4a7c15ull + (result << 6u) + (result >> 2u);
    };
    add(settings.radius);
    add(settings.falloff);
    add(settings.intensity);
    add(settings.power);
    add(settings.temporalWeight);
    return result;
}

VkImageSubresourceRange colorRange() {
    return {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
}

} // namespace

GtaoPass::GtaoPass(Device &device,
                   const RenderResourceRegistry &resources,
                   RendererResourceHandles resourceHandles,
                   DescriptorAllocator &descriptorAllocator,
                   VkDescriptorSetLayout globalDescriptorSetLayout,
                   std::string traceShaderPath,
                   std::string temporalShaderPath,
                   std::string blurShaderPath)
    : device_(&device), resourceHandles_(resourceHandles),
      descriptorAllocator_(&descriptorAllocator),
      globalDescriptorSetLayout_(globalDescriptorSetLayout),
      traceShaderPath_(std::move(traceShaderPath)),
      temporalShaderPath_(std::move(temporalShaderPath)),
      blurShaderPath_(std::move(blurShaderPath)) {
    if (!resourceHandles_.surfaceDepth.valid() ||
        !resourceHandles_.surfaceNormalRoughness.valid() ||
        !resourceHandles_.surfaceMotion.valid() ||
        !resourceHandles_.screenDepthPyramid.valid() ||
        !resourceHandles_.gtaoRaw.valid() ||
        !resourceHandles_.gtaoHistory.valid() ||
        !resourceHandles_.gtaoTemp.valid() ||
        !resourceHandles_.gtaoFiltered.valid() ||
        !resourceHandles_.gtaoDebug.valid()) {
        throw std::invalid_argument("GtaoPass requires all GTAO resources");
    }
    status_.supported = true;
    createDescriptorSetLayouts();
    createDescriptors(resources);
}

GtaoPass::~GtaoPass() {
    freeDescriptors();
    if (temporalLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_->logicalDevice(), temporalLayout_,
                                     nullptr);
    if (sampleStorageLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     sampleStorageLayout_, nullptr);
    }
}

void GtaoPass::releaseViewportResources() {
    freeDescriptors();
    initialized_.fill(false);
    historyWritten_.fill(false);
    lastExecutionSerial_ = 0;
    status_.active = false;
    status_.historyValid = false;
    status_.lastResetReason = "viewport resized";
}

void GtaoPass::setup(RenderGraphBuilder &builder,
                     const RenderGraphBuildContext &) const {
    const auto surfaceReads = [&] {
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
    builder.addNode("GTAO/Trace", RgPassType::Compute,
                    RgQueueClass::Compute, 0);
    surfaceReads();
    builder.useImage(
        {resourceHandles_.screenDepthPyramid,
         RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    builder.useImage({resourceHandles_.gtaoRaw, RenderImageAccess::StorageWrite,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});

    builder.addNode("GTAO/Temporal", RgPassType::Compute,
                    RgQueueClass::Compute, 1);
    surfaceReads();
    builder.useImage(
        {resourceHandles_.surfaceMotion, RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    builder.useImage(
        {resourceHandles_.surfaceDepth, RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
         RenderImageFrame::Previous});
    builder.useImage(
        {resourceHandles_.surfaceNormalRoughness,
         RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         RenderImageFrame::Previous});
    builder.useImage(
        {resourceHandles_.gtaoHistory, RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         RenderImageFrame::Previous});
    builder.useImage({resourceHandles_.gtaoRaw, RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    builder.useImage(
        {resourceHandles_.gtaoHistory, RenderImageAccess::StorageWrite,
         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    builder.useImage(
        {resourceHandles_.gtaoDebug, RenderImageAccess::StorageWrite,
         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});

    builder.addNode("GTAO/BilateralHorizontal", RgPassType::Compute,
                    RgQueueClass::Compute, 2);
    surfaceReads();
    builder.useImage(
        {resourceHandles_.gtaoHistory, RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    builder.useImage({resourceHandles_.gtaoTemp, RenderImageAccess::StorageWrite,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});

    builder.addNode("GTAO/BilateralVertical", RgPassType::Compute,
                    RgQueueClass::Compute, 3);
    surfaceReads();
    builder.useImage({resourceHandles_.gtaoTemp, RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    builder.useImage(
        {resourceHandles_.gtaoFiltered, RenderImageAccess::StorageWrite,
         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});

    builder.addNode("GTAO/Finalize", RgPassType::Compute,
                    RgQueueClass::Compute, 4);
    for (RenderImageHandle handle :
         {resourceHandles_.gtaoRaw, resourceHandles_.gtaoHistory,
          resourceHandles_.gtaoFiltered, resourceHandles_.gtaoDebug}) {
        builder.useImage({handle, RenderImageAccess::SampledRead,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
}

void GtaoPass::recordNode(RenderGraphPassContext &context,
                          uint32_t localNodeIndex,
                          const VisibilityFrame &visibility) {
    if (localNodeIndex == 0)
        beginFrame(context.frame, context.resources, visibility);
    if (localNodeIndex < 4)
        recordStage(context.frame, context.resources, localNodeIndex);
    else if (localNodeIndex == 4)
        finishFrame(context.frame, visibility);
}

void GtaoPass::beginFrame(const RenderFrameContext &frame,
                          const RenderResourceRegistry &resources,
                          const VisibilityFrame &visibility) {
    status_.active = frame.view &&
                     frame.view->settings.ambientOcclusionMode ==
                         AmbientOcclusionMode::Gtao;
    if (!frame.view)
        return;
    const uint32_t previousFrame =
        (frame.frameIndex + resources.frameCount() - 1u) %
        resources.frameCount();
    currentSettingsSignature_ = settingsSignature(frame.view->settings.gtao);
    const bool contiguous = lastExecutionSerial_ != 0 &&
                            lastExecutionSerial_ + 1u ==
                                frame.submissionSerial;
    const bool generationMatches =
        lastExecutionSerial_ != 0 &&
        lastHistoryGeneration_ == visibility.history.historyGeneration;
    const bool settingsMatch = lastExecutionSerial_ != 0 &&
                               lastSettingsSignature_ ==
                                   currentSettingsSignature_;
    currentHistoryValid_ = visibility.history.globalValid && contiguous &&
                           generationMatches && settingsMatch &&
                           historyWritten_[previousFrame];
    if (!currentHistoryValid_) {
        if (!visibility.history.invalidationReason.empty())
            status_.lastResetReason = visibility.history.invalidationReason;
        else if (!contiguous)
            status_.lastResetReason = "GTAO was not executed continuously";
        else if (!generationMatches)
            status_.lastResetReason = "temporal generation changed";
        else if (!settingsMatch)
            status_.lastResetReason = "GTAO settings changed";
        else
            status_.lastResetReason = "history is unavailable";
    } else {
        status_.lastResetReason.clear();
    }
    status_.historyValid = currentHistoryValid_;
    status_.historyGeneration = visibility.history.historyGeneration;
    status_.lastFrameSerial = frame.submissionSerial;
    status_.extent = resources.extent(resourceHandles_.gtaoFiltered);
}

void GtaoPass::recordStage(const RenderFrameContext &frame,
                           const RenderResourceRegistry &resources,
                           uint32_t stage) {
    if (!frame.pipelineCache || !frame.view || stage > 3)
        return;
    const uint32_t frameIndex = frame.frameIndex;
    const VkExtent2D fullExtent =
        resources.extent(resourceHandles_.surfaceDepth);
    const VkExtent2D aoExtent =
        resources.extent(resourceHandles_.gtaoFiltered);

    if (stage == 0) {
        const auto [sliceCount, stepCount] =
            qualitySamples(frame.view->settings.gtao.quality);
        GtaoTracePush push{};
        push.parameters =
            glm::vec4(frame.view->settings.gtao.radius,
                      frame.view->settings.gtao.falloff,
                      frame.view->settings.gtao.intensity,
                      frame.view->settings.gtao.power);
        push.dimensions = glm::uvec4(sliceCount, stepCount, fullExtent.width,
                                     fullExtent.height);
        push.sampling = glm::vec4(
            static_cast<float>(resources.mipLevelCount(
                resourceHandles_.screenDepthPyramid) - 1u),
            static_cast<float>(frame.submissionSerial & 7u), 0.0f, 0.0f);
        ComputePipelineConfig config{};
        config.debugName = "Pipeline/ScreenSpace/GTAO/Trace";
        config.computeShaderPath = traceShaderPath_;
        config.descriptorLayouts = {globalDescriptorSetLayout_,
                                    sampleStorageLayout_};
        config.pushConstants = {
            {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push)}};
        ComputePipeline &pipeline =
            frame.pipelineCache->getOrCreateCompute(std::move(config));
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipeline.handle());
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline.layout(), 0, 1,
                                &frame.globalDescriptorSet, 0, nullptr);
        const VkDescriptorSet set = traceSets_[frameIndex];
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline.layout(), 1, 1, &set, 0, nullptr);
        vkCmdPushConstants(frame.cmd, pipeline.layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        vkCmdDispatch(frame.cmd, dispatchCount(aoExtent.width),
                      dispatchCount(aoExtent.height), 1);
        return;
    }

    if (stage == 1) {
        GtaoTemporalPush push{};
        push.parameters =
            glm::vec4(frame.view->settings.gtao.temporalWeight, 0.01f, 0.8f,
                      0.0f);
        const ScreenSpaceDebugView debugView =
            frame.view->settings.screenSpaceDebugView;
        push.dimensions = glm::uvec4(
            fullExtent.width, fullExtent.height,
            currentHistoryValid_ ? 1u : 0u,
            debugView == ScreenSpaceDebugView::GtaoHistoryWeight ? 1u : 0u);
        ComputePipelineConfig config{};
        config.debugName = "Pipeline/ScreenSpace/GTAO/Temporal";
        config.computeShaderPath = temporalShaderPath_;
        config.descriptorLayouts = {temporalLayout_};
        config.pushConstants = {
            {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push)}};
        ComputePipeline &pipeline =
            frame.pipelineCache->getOrCreateCompute(std::move(config));
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipeline.handle());
        const VkDescriptorSet set = temporalSets_[frameIndex];
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline.layout(), 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(frame.cmd, pipeline.layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        vkCmdDispatch(frame.cmd, dispatchCount(aoExtent.width),
                      dispatchCount(aoExtent.height), 1);
        return;
    }

    BlurPush push{};
    push.dimensions = glm::uvec4(0u, fullExtent.width, fullExtent.height,
                                 stage == 3 ? 1u : 0u);
    ComputePipelineConfig config{};
    config.debugName = "Pipeline/ScreenSpace/GTAO/Blur";
    config.computeShaderPath = blurShaderPath_;
    config.descriptorLayouts = {globalDescriptorSetLayout_,
                                sampleStorageLayout_};
    config.pushConstants = {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push)}};
    ComputePipeline &pipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(config));
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.handle());
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 0, 1,
                            &frame.globalDescriptorSet, 0, nullptr);
    const VkDescriptorSet set = stage == 2 ? horizontalSets_[frameIndex]
                                           : verticalSets_[frameIndex];
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 1, 1, &set, 0, nullptr);
    vkCmdPushConstants(frame.cmd, pipeline.layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(frame.cmd, dispatchCount(aoExtent.width),
                  dispatchCount(aoExtent.height), 1);
}

void GtaoPass::finishFrame(const RenderFrameContext &frame,
                           const VisibilityFrame &visibility) {
    historyWritten_[frame.frameIndex] = true;
    lastExecutionSerial_ = frame.submissionSerial;
    lastHistoryGeneration_ = visibility.history.historyGeneration;
    lastSettingsSignature_ = currentSettingsSignature_;
}

void GtaoPass::onViewportResize(const RenderResourceRegistry &resources) {
    createDescriptors(resources);
    status_.extent = resources.extent(resourceHandles_.gtaoFiltered);
}

void GtaoPass::execute(const RenderFrameContext &frame,
                       const RenderResourceRegistry &resources,
                       const VisibilityFrame &visibility) {
    status_.active = frame.view &&
                     frame.view->settings.ambientOcclusionMode ==
                         AmbientOcclusionMode::Gtao;
    if (!frame.features.gtaoRequired || !frame.pipelineCache || !frame.view)
        return;

    VKL_PROFILE_ZONE("Record GTAO");
    VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd, "GTAO");
    const uint32_t frameIndex = frame.frameIndex;
    const uint32_t previousFrame =
        (frameIndex + resources.frameCount() - 1u) % resources.frameCount();
    const uint64_t signature = settingsSignature(frame.view->settings.gtao);
    const bool contiguous = lastExecutionSerial_ != 0 &&
                            lastExecutionSerial_ + 1u ==
                                frame.submissionSerial;
    const bool generationMatches =
        lastExecutionSerial_ != 0 &&
        lastHistoryGeneration_ == visibility.history.historyGeneration;
    const bool settingsMatch = lastExecutionSerial_ != 0 &&
                               lastSettingsSignature_ == signature;
    const bool historyValid = visibility.history.globalValid && contiguous &&
                              generationMatches && settingsMatch &&
                              historyWritten_[previousFrame];
    if (!historyValid) {
        if (!visibility.history.invalidationReason.empty()) {
            status_.lastResetReason = visibility.history.invalidationReason;
        } else if (!contiguous) {
            status_.lastResetReason = "GTAO was not executed continuously";
        } else if (!generationMatches) {
            status_.lastResetReason = "temporal generation changed";
        } else if (!settingsMatch) {
            status_.lastResetReason = "GTAO settings changed";
        } else {
            status_.lastResetReason = "history is unavailable";
        }
    } else {
        status_.lastResetReason.clear();
    }
    status_.historyValid = historyValid;
    status_.historyGeneration = visibility.history.historyGeneration;
    status_.lastFrameSerial = frame.submissionSerial;
    status_.extent = resources.extent(resourceHandles_.gtaoFiltered);

    const std::array<RenderImageHandle, 5> handles = {
        resourceHandles_.gtaoRaw, resourceHandles_.gtaoHistory,
        resourceHandles_.gtaoTemp, resourceHandles_.gtaoFiltered,
        resourceHandles_.gtaoDebug};
    for (uint32_t index = 0; index < handles.size(); ++index) {
        const Image &image = resources.image(handles[index], frameIndex);
        const bool generalImage = index == 2u;
        cmdImageBarrier(
            frame.cmd,
            initialized_[frameIndex]
                ? (generalImage ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            initialized_[frameIndex]
                ? (generalImage ? VK_ACCESS_SHADER_READ_BIT |
                                      VK_ACCESS_SHADER_WRITE_BIT
                                : VK_ACCESS_SHADER_READ_BIT)
                : 0u,
            VK_ACCESS_SHADER_WRITE_BIT, image.handle(),
            initialized_[frameIndex]
                ? (generalImage ? VK_IMAGE_LAYOUT_GENERAL
                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL, colorRange());
    }

    const VkPushConstantRange tracePushRange{
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GtaoTracePush)};
    const VkPushConstantRange temporalPushRange{
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GtaoTemporalPush)};
    const VkPushConstantRange blurPushRange{
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BlurPush)};

    ComputePipelineConfig traceConfig{};
    traceConfig.debugName = "Pipeline/ScreenSpace/GTAO/Trace";
    traceConfig.computeShaderPath = traceShaderPath_;
    traceConfig.descriptorLayouts = {globalDescriptorSetLayout_,
                                     sampleStorageLayout_};
    traceConfig.pushConstants = {tracePushRange};
    ComputePipelineConfig temporalConfig{};
    temporalConfig.debugName = "Pipeline/ScreenSpace/GTAO/Temporal";
    temporalConfig.computeShaderPath = temporalShaderPath_;
    temporalConfig.descriptorLayouts = {temporalLayout_};
    temporalConfig.pushConstants = {temporalPushRange};
    ComputePipelineConfig blurConfig{};
    blurConfig.debugName = "Pipeline/ScreenSpace/GTAO/Blur";
    blurConfig.computeShaderPath = blurShaderPath_;
    blurConfig.descriptorLayouts = {globalDescriptorSetLayout_,
                                    sampleStorageLayout_};
    blurConfig.pushConstants = {blurPushRange};
    ComputePipeline &tracePipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(traceConfig));
    ComputePipeline &temporalPipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(temporalConfig));
    ComputePipeline &blurPipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(blurConfig));

    const VkExtent2D fullExtent =
        resources.extent(resourceHandles_.surfaceDepth);
    const VkExtent2D aoExtent =
        resources.extent(resourceHandles_.gtaoFiltered);
    const auto [sliceCount, stepCount] =
        qualitySamples(frame.view->settings.gtao.quality);
    GtaoTracePush tracePush{};
    tracePush.parameters =
        glm::vec4(frame.view->settings.gtao.radius,
                  frame.view->settings.gtao.falloff,
                  frame.view->settings.gtao.intensity,
                  frame.view->settings.gtao.power);
    tracePush.dimensions =
        glm::uvec4(sliceCount, stepCount, fullExtent.width, fullExtent.height);
    tracePush.sampling =
        glm::vec4(static_cast<float>(resources.mipLevelCount(
                      resourceHandles_.screenDepthPyramid) - 1u),
                  static_cast<float>(frame.submissionSerial & 7u), 0.0f, 0.0f);

    {
        ScopedGpuLabel label(device_->debugUtils(), frame.cmd, "Trace");
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          tracePipeline.handle());
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                tracePipeline.layout(), 0, 1,
                                &frame.globalDescriptorSet, 0, nullptr);
        const VkDescriptorSet set = traceSets_[frameIndex];
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                tracePipeline.layout(), 1, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(frame.cmd, tracePipeline.layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tracePush),
                           &tracePush);
        vkCmdDispatch(frame.cmd, dispatchCount(aoExtent.width),
                      dispatchCount(aoExtent.height), 1);
    }
    const Image &raw = resources.image(resourceHandles_.gtaoRaw, frameIndex);
    cmdImageBarrier(frame.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    raw.handle(), VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_GENERAL, colorRange());

    GtaoTemporalPush temporalPush{};
    temporalPush.parameters =
        glm::vec4(frame.view->settings.gtao.temporalWeight, 0.01f, 0.8f,
                  0.0f);
    const ScreenSpaceDebugView debugView =
        frame.view->settings.screenSpaceDebugView;
    temporalPush.dimensions =
        glm::uvec4(fullExtent.width, fullExtent.height,
                   historyValid ? 1u : 0u,
                   debugView == ScreenSpaceDebugView::GtaoHistoryWeight ? 1u
                                                                        : 0u);
    {
        ScopedGpuLabel label(device_->debugUtils(), frame.cmd,
                             "Temporal Resolve");
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          temporalPipeline.handle());
        const VkDescriptorSet set = temporalSets_[frameIndex];
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                temporalPipeline.layout(), 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(frame.cmd, temporalPipeline.layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(temporalPush), &temporalPush);
        vkCmdDispatch(frame.cmd, dispatchCount(aoExtent.width),
                      dispatchCount(aoExtent.height), 1);
    }
    const Image &history =
        resources.image(resourceHandles_.gtaoHistory, frameIndex);
    cmdImageBarrier(frame.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    history.handle(), VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_GENERAL, colorRange());

    BlurPush blurPush{};
    blurPush.dimensions = glm::uvec4(0u, fullExtent.width,
                                     fullExtent.height, 0u);
    const auto blur = [&](VkDescriptorSet set, uint32_t axis,
                          std::string_view name) {
        ScopedGpuLabel label(device_->debugUtils(), frame.cmd, name);
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          blurPipeline.handle());
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                blurPipeline.layout(), 0, 1,
                                &frame.globalDescriptorSet, 0, nullptr);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                blurPipeline.layout(), 1, 1, &set, 0,
                                nullptr);
        blurPush.dimensions.w = axis;
        vkCmdPushConstants(frame.cmd, blurPipeline.layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(blurPush),
                           &blurPush);
        vkCmdDispatch(frame.cmd, dispatchCount(aoExtent.width),
                      dispatchCount(aoExtent.height), 1);
    };
    blur(horizontalSets_[frameIndex], 0u, "Bilateral Horizontal");
    const Image &temp = resources.image(resourceHandles_.gtaoTemp, frameIndex);
    cmdImageBarrier(frame.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    temp.handle(), VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_GENERAL, colorRange());
    blur(verticalSets_[frameIndex], 1u, "Bilateral Vertical");

    for (RenderImageHandle handle :
         {resourceHandles_.gtaoRaw, resourceHandles_.gtaoHistory,
          resourceHandles_.gtaoFiltered, resourceHandles_.gtaoDebug}) {
        const Image &image = resources.image(handle, frameIndex);
        cmdImageBarrier(
            frame.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT, image.handle(),
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, colorRange());
    }
    initialized_[frameIndex] = true;
    historyWritten_[frameIndex] = true;
    lastExecutionSerial_ = frame.submissionSerial;
    lastHistoryGeneration_ = visibility.history.historyGeneration;
    lastSettingsSignature_ = signature;
}

void GtaoPass::createDescriptorSetLayouts() {
    std::array<VkDescriptorSetLayoutBinding, 4> sampleBindings{};
    for (uint32_t binding = 0; binding < 3; ++binding) {
        sampleBindings[binding] = {
            binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    sampleBindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(sampleBindings.size());
    info.pBindings = sampleBindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &sampleStorageLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, sampleStorageLayout_,
        "DescriptorLayout/GTAO/SampleStorage");

    std::array<VkDescriptorSetLayoutBinding, 9> temporalBindings{};
    for (uint32_t binding = 0; binding < 7; ++binding) {
        temporalBindings[binding] = {
            binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    for (uint32_t binding = 7; binding < 9; ++binding) {
        temporalBindings[binding] = {
            binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    info.bindingCount = static_cast<uint32_t>(temporalBindings.size());
    info.pBindings = temporalBindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &temporalLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, temporalLayout_,
        "DescriptorLayout/GTAO/Temporal");
}

void GtaoPass::createDescriptors(
    const RenderResourceRegistry &resources) {
    const VkSampler depthSampler =
        resources.sampler(resourceHandles_.surfaceDepthSampler);
    const VkSampler surfaceSampler =
        resources.sampler(resourceHandles_.surfaceDataSampler);
    const VkSampler pyramidSampler =
        resources.sampler(resourceHandles_.screenPyramidSampler);
    const VkSampler aoSampler = resources.sampler(resourceHandles_.ssaoSampler);

    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        const VkImageView depth =
            resources.image(resourceHandles_.surfaceDepth, frame).imageView();
        const VkImageView normal =
            resources.image(resourceHandles_.surfaceNormalRoughness, frame)
                .imageView();
        const VkImageView motion =
            resources.image(resourceHandles_.surfaceMotion, frame).imageView();
        const VkImageView raw =
            resources.image(resourceHandles_.gtaoRaw, frame).imageView();
        const VkImageView history =
            resources.image(resourceHandles_.gtaoHistory, frame).imageView();
        const VkImageView temp =
            resources.image(resourceHandles_.gtaoTemp, frame).imageView();
        const VkImageView filtered =
            resources.image(resourceHandles_.gtaoFiltered, frame).imageView();
        const VkImageView debug =
            resources.image(resourceHandles_.gtaoDebug, frame).imageView();

        const auto sampleStorageSet = [&](std::string suffix,
                                          VkImageView source,
                                          VkSampler sourceSampler,
                                          VkImageView destination) {
            VkDescriptorSet set = descriptorAllocator_->allocate(
                sampleStorageLayout_,
                {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
                 {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}},
                "GTAO/Frame" + std::to_string(frame) + "/" + suffix);
            std::array<VkDescriptorImageInfo, 4> infos{};
            infos[0] = {depthSampler, depth,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
            infos[1] = {surfaceSampler, normal,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            infos[2] = {sourceSampler, source, VK_IMAGE_LAYOUT_GENERAL};
            infos[3] = {VK_NULL_HANDLE, destination, VK_IMAGE_LAYOUT_GENERAL};
            if (suffix == "Trace")
                infos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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

        traceSets_[frame] = sampleStorageSet(
            "Trace",
            resources.image(resourceHandles_.screenDepthPyramid, frame)
                .imageView(),
            pyramidSampler, raw);
        horizontalSets_[frame] =
            sampleStorageSet("Horizontal", history, aoSampler, temp);
        verticalSets_[frame] =
            sampleStorageSet("Vertical", temp, aoSampler, filtered);

        temporalSets_[frame] = descriptorAllocator_->allocate(
            temporalLayout_,
            {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 7},
             {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}},
            "GTAO/Frame" + std::to_string(frame) + "/Temporal");
        std::array<VkDescriptorImageInfo, 9> infos{};
        infos[0] = {aoSampler, raw, VK_IMAGE_LAYOUT_GENERAL};
        infos[1] = {depthSampler, depth,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        infos[2] = {surfaceSampler, normal,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        infos[3] = {surfaceSampler, motion,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        infos[4] = {
            depthSampler,
            resources.previousImage(resourceHandles_.surfaceDepth, frame)
                .imageView(),
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        infos[5] = {
            surfaceSampler,
            resources
                .previousImage(resourceHandles_.surfaceNormalRoughness, frame)
                .imageView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        infos[6] = {
            aoSampler,
            resources.previousImage(resourceHandles_.gtaoHistory, frame)
                .imageView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        infos[7] = {VK_NULL_HANDLE, history, VK_IMAGE_LAYOUT_GENERAL};
        infos[8] = {VK_NULL_HANDLE, debug, VK_IMAGE_LAYOUT_GENERAL};
        std::array<VkWriteDescriptorSet, 9> writes{};
        for (uint32_t binding = 0; binding < writes.size(); ++binding) {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = temporalSets_[frame];
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType =
                binding < 7 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[binding].pImageInfo = &infos[binding];
        }
        vkUpdateDescriptorSets(device_->logicalDevice(),
                               static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

void GtaoPass::freeDescriptors() {
    for (auto *sets : {&traceSets_, &temporalSets_, &horizontalSets_,
                       &verticalSets_}) {
        for (VkDescriptorSet &set : *sets) {
            if (set != VK_NULL_HANDLE)
                descriptorAllocator_->free(set);
            set = VK_NULL_HANDLE;
        }
    }
}

} // namespace vkr
