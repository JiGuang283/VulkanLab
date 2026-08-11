#include "render/pass/TaaPass.h"

#include "core/Buffer.h"
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
#include "render/FrameGpuData.h"
#include "render/PipelineCache.h"
#include "render/RenderFrame.h"
#include "render/RenderView.h"
#include "render/Visibility.h"

#include <array>
#include <cstring>
#include <glm/gtc/matrix_inverse.hpp>
#include <stdexcept>
#include <utility>

namespace vkr {

namespace {

constexpr uint32_t kWorkgroupSize = 8;

uint32_t dispatchCount(uint32_t value) {
    return (value + kWorkgroupSize - 1u) / kWorkgroupSize;
}

VkImageSubresourceRange colorRange() {
    return {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
}

VkImageSubresourceRange depthRange() {
    return {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
}

} // namespace

TaaPass::TaaPass(Device &device,
                 const RenderResourceRegistry &resources,
                 RendererResourceHandles resourceHandles,
                 DescriptorAllocator &descriptorAllocator,
                 std::string resolveShaderPath)
    : device_(&device), resourceHandles_(resourceHandles),
      descriptorAllocator_(&descriptorAllocator),
      resolveShaderPath_(std::move(resolveShaderPath)) {
    if (!resourceHandles_.hdrColor.valid() ||
        !resourceHandles_.surfaceDepth.valid() ||
        !resourceHandles_.surfaceNormalRoughness.valid() ||
        !resourceHandles_.surfaceMotion.valid() ||
        !resourceHandles_.taaHistory.valid() ||
        !resourceHandles_.taaDebug.valid() ||
        !resourceHandles_.taaSampler.valid()) {
        throw std::invalid_argument("TaaPass requires all temporal resources");
    }
    status_.supported = true;
    createDescriptorSetLayout();
    createFrameBuffers();
    createDescriptors(resources);
}

TaaPass::~TaaPass() {
    freeDescriptors();
    for (auto &buffer : frameUbos_)
        buffer.reset();
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     descriptorSetLayout_, nullptr);
    }
}

std::vector<RenderImageUsage> TaaPass::resourceUsages() const {
    return {
        {resourceHandles_.hdrColor, RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {resourceHandles_.surfaceDepth, RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        {resourceHandles_.surfaceNormalRoughness,
         RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {resourceHandles_.surfaceMotion, RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {resourceHandles_.surfaceDepth, RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
         RenderImageFrame::Previous},
        {resourceHandles_.surfaceNormalRoughness,
         RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         RenderImageFrame::Previous},
        {resourceHandles_.taaHistory, RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         RenderImageFrame::Previous},
        {resourceHandles_.taaHistory, RenderImageAccess::StorageWrite,
         VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {resourceHandles_.taaDebug, RenderImageAccess::StorageWrite,
         VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
}

void TaaPass::releaseViewportResources() {
    freeDescriptors();
    historyLayoutInitialized_.fill(false);
    debugLayoutInitialized_.fill(false);
    historyWritten_.fill(false);
    lastExecutionSerial_ = 0;
    status_.active = false;
    status_.historyValid = false;
    status_.lastResetReason = "viewport resized";
}

void TaaPass::onViewportResize(const RenderResourceRegistry &resources) {
    createDescriptors(resources);
}

void TaaPass::execute(const RenderFrameContext &frame,
                      const RenderResourceRegistry &resources,
                      const VisibilityFrame &visibility) {
    status_.active = frame.features.taaActive;
    status_.jitterPixels = visibility.history.currentJitterPixels;
    if (!frame.features.taaRequired || !frame.pipelineCache || !frame.view)
        return;

    VKL_PROFILE_ZONE("Record TAA");
    VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd, "TAA");
    const uint32_t previousFrame =
        (frame.frameIndex + resources.frameCount() - 1u) %
        resources.frameCount();
    const bool contiguous = lastExecutionSerial_ != 0 &&
                            lastExecutionSerial_ + 1u ==
                                frame.submissionSerial;
    const bool generationMatches =
        lastExecutionSerial_ != 0 &&
        lastHistoryGeneration_ == visibility.history.historyGeneration;
    const bool historyValid = visibility.history.globalValid && contiguous &&
                              generationMatches &&
                              historyWritten_[previousFrame];

    if (!historyValid) {
        if (!visibility.history.invalidationReason.empty()) {
            status_.lastResetReason =
                visibility.history.invalidationReason;
        } else if (!contiguous) {
            status_.lastResetReason = "TAA was not executed continuously";
        } else if (!generationMatches) {
            status_.lastResetReason = "temporal generation changed";
        } else {
            status_.lastResetReason = "history is unavailable";
        }
    } else {
        status_.lastResetReason.clear();
    }
    status_.historyValid = historyValid;
    status_.historyGeneration = visibility.history.historyGeneration;
    status_.lastFrameSerial = frame.submissionSerial;

    prepareImages(frame, resources, historyValid);

    TaaFrameUbo ubo{};
    ubo.currentInverseViewProjection =
        frame.view->globalUbo.inverseViewProjection;
    ubo.previousViewProjection = visibility.history.previousViewProjection;
    ubo.previousInverseViewProjection =
        glm::inverse(visibility.history.previousViewProjection);
    const VkExtent2D extent = resources.extent(resourceHandles_.taaHistory);
    ubo.viewportSizeInvSize =
        glm::vec4(static_cast<float>(extent.width),
                  static_cast<float>(extent.height),
                  1.0f / static_cast<float>(extent.width),
                  1.0f / static_cast<float>(extent.height));
    ubo.jitterCurrentPreviousPixels =
        glm::vec4(visibility.history.currentJitterPixels,
                  visibility.history.previousJitterPixels);
    ubo.parameters =
        glm::vec4(frame.view->settings.taaHistoryWeight,
                  frame.view->settings.taaSharpness,
                  frame.view->cameraNearPlane,
                  frame.view->cameraFarPlane);
    ubo.flags = glm::uvec4(historyValid ? 1u : 0u,
                           static_cast<uint32_t>(
                               visibility.history.historyGeneration),
                           frame.view->shadow.temporalReactive ? 1u : 0u,
                           visibility.cameraTransparent.empty() ? 0u : 1u);
    std::memcpy(frameUbos_[frame.frameIndex]->mappedData(), &ubo,
                sizeof(ubo));

    ComputePipelineConfig config{};
    config.debugName = "Pipeline/PostProcess/TAA/Resolve";
    config.computeShaderPath = resolveShaderPath_;
    config.descriptorLayouts = {descriptorSetLayout_};
    ComputePipeline &pipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(config));
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.handle());
    const VkDescriptorSet set = descriptorSets_[frame.frameIndex];
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 0, 1, &set, 0, nullptr);
    vkCmdDispatch(frame.cmd, dispatchCount(extent.width),
                  dispatchCount(extent.height), 1);

    for (RenderImageHandle handle : {resourceHandles_.taaHistory,
                                     resourceHandles_.taaDebug}) {
        const Image &image = resources.image(handle, frame.frameIndex);
        cmdImageBarrier(
            frame.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            image.handle(), VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, colorRange());
    }

    historyLayoutInitialized_[frame.frameIndex] = true;
    debugLayoutInitialized_[frame.frameIndex] = true;
    historyWritten_[frame.frameIndex] = true;
    lastExecutionSerial_ = frame.submissionSerial;
    lastHistoryGeneration_ = visibility.history.historyGeneration;
}

void TaaPass::createDescriptorSetLayout() {
    std::array<VkDescriptorSetLayoutBinding, 10> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    for (uint32_t binding = 1; binding <= 7; ++binding) {
        bindings[binding] = {
            binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    for (uint32_t binding = 8; binding <= 9; ++binding) {
        bindings[binding] = {binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                             VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &descriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, descriptorSetLayout_,
        "DescriptorLayout/TAA");
}

void TaaPass::createFrameBuffers() {
    constexpr VmaAllocationCreateFlags flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT;
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        frameUbos_[frame] = std::make_unique<Buffer>(
            *device_, sizeof(TaaFrameUbo),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            flags, "Frame/" + std::to_string(frame) + "/TaaFrameUbo");
        frameUbos_[frame]->map();
    }
}

void TaaPass::createDescriptors(
    const RenderResourceRegistry &resources) {
    const VkSampler hdrSampler = resources.sampler(resourceHandles_.hdrSampler);
    const VkSampler depthSampler =
        resources.sampler(resourceHandles_.surfaceDepthSampler);
    const VkSampler surfaceSampler =
        resources.sampler(resourceHandles_.surfaceDataSampler);
    const VkSampler historySampler =
        resources.sampler(resourceHandles_.taaSampler);

    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        descriptorSets_[frame] = descriptorAllocator_->allocate(
            descriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
             {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 7},
             {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}},
            "TAA/Frame" + std::to_string(frame));

        VkDescriptorBufferInfo bufferInfo{
            frameUbos_[frame]->handle(), 0, sizeof(TaaFrameUbo)};
        std::array<VkDescriptorImageInfo, 9> infos{};
        infos[0] = {hdrSampler,
                    resources.image(resourceHandles_.hdrColor, frame)
                        .imageView(),
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        infos[1] = {depthSampler,
                    resources.image(resourceHandles_.surfaceDepth, frame)
                        .imageView(),
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        infos[2] = {
            surfaceSampler,
            resources
                .image(resourceHandles_.surfaceNormalRoughness, frame)
                .imageView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        infos[3] = {surfaceSampler,
                    resources.image(resourceHandles_.surfaceMotion, frame)
                        .imageView(),
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        infos[4] = {
            depthSampler,
            resources.previousImage(resourceHandles_.surfaceDepth, frame)
                .imageView(),
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        infos[5] = {
            surfaceSampler,
            resources
                .previousImage(resourceHandles_.surfaceNormalRoughness,
                               frame)
                .imageView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        infos[6] = {
            historySampler,
            resources.previousImage(resourceHandles_.taaHistory, frame)
                .imageView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        infos[7] = {
            VK_NULL_HANDLE,
            resources.image(resourceHandles_.taaHistory, frame).imageView(),
            VK_IMAGE_LAYOUT_GENERAL};
        infos[8] = {
            VK_NULL_HANDLE,
            resources.image(resourceHandles_.taaDebug, frame).imageView(),
            VK_IMAGE_LAYOUT_GENERAL};

        std::array<VkWriteDescriptorSet, 10> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     descriptorSets_[frame], 0, 0, 1,
                     VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr,
                     &bufferInfo, nullptr};
        for (uint32_t binding = 1; binding <= 9; ++binding) {
            writes[binding] = {
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                descriptorSets_[frame], binding, 0, 1,
                binding <= 7
                    ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                    : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                &infos[binding - 1], nullptr, nullptr};
        }
        vkUpdateDescriptorSets(device_->logicalDevice(),
                               static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

void TaaPass::freeDescriptors() {
    for (VkDescriptorSet &set : descriptorSets_) {
        if (set != VK_NULL_HANDLE)
            descriptorAllocator_->free(set);
        set = VK_NULL_HANDLE;
    }
}

void TaaPass::prepareImages(const RenderFrameContext &frame,
                            const RenderResourceRegistry &resources,
                            bool historyValid) {
    const uint32_t previousFrame =
        (frame.frameIndex + resources.frameCount() - 1u) %
        resources.frameCount();
    if (!historyValid) {
        const Image &oldDepth =
            resources.image(resourceHandles_.surfaceDepth, previousFrame);
        cmdImageBarrier(
            frame.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
            VK_ACCESS_SHADER_READ_BIT, oldDepth.handle(),
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, depthRange());
        const Image &oldNormal = resources.image(
            resourceHandles_.surfaceNormalRoughness, previousFrame);
        cmdImageBarrier(frame.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                        VK_ACCESS_SHADER_READ_BIT, oldNormal.handle(),
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        colorRange());
    }

    if (!historyLayoutInitialized_[previousFrame]) {
        const Image &previous =
            resources.image(resourceHandles_.taaHistory, previousFrame);
        cmdImageBarrier(frame.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                        VK_ACCESS_SHADER_READ_BIT, previous.handle(),
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        colorRange());
        historyLayoutInitialized_[previousFrame] = true;
    }

    const Image &history =
        resources.image(resourceHandles_.taaHistory, frame.frameIndex);
    cmdImageBarrier(
        frame.cmd,
        historyLayoutInitialized_[frame.frameIndex]
            ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
            : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        historyLayoutInitialized_[frame.frameIndex]
            ? VK_ACCESS_SHADER_READ_BIT
            : 0u,
        VK_ACCESS_SHADER_WRITE_BIT, history.handle(),
        historyLayoutInitialized_[frame.frameIndex]
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL, colorRange());

    const Image &debug =
        resources.image(resourceHandles_.taaDebug, frame.frameIndex);
    cmdImageBarrier(
        frame.cmd,
        debugLayoutInitialized_[frame.frameIndex]
            ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
            : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        debugLayoutInitialized_[frame.frameIndex]
            ? VK_ACCESS_SHADER_READ_BIT
            : 0u,
        VK_ACCESS_SHADER_WRITE_BIT, debug.handle(),
        debugLayoutInitialized_[frame.frameIndex]
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL, colorRange());
}

} // namespace vkr
