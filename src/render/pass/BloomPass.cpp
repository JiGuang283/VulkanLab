#include "BloomPass.h"

#include "core/ComputePipeline.h"
#include "core/ComputePipelineConfig.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/VulkanCheck.h"
#include "render/FrameGpuData.h"
#include "render/PipelineCache.h"
#include "render/RenderFrame.h"
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

VkImageMemoryBarrier imageBarrier(
    VkImage image, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
    VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    return barrier;
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

std::vector<RenderImageUsage> BloomPass::resourceUsages() const {
    std::vector<RenderImageUsage> usages;
    usages.reserve(2 + kLevelCount * 4);
    usages.push_back(
        {resourceHandles_.hdrColor, RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});

    for (uint32_t level = 0; level < kLevelCount; ++level) {
        if (level > 0) {
            usages.push_back(
                {resourceHandles_.bloomLevels[level - 1],
                 RenderImageAccess::SampledRead, VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_GENERAL});
        }
        usages.push_back(
            {resourceHandles_.bloomLevels[level],
             RenderImageAccess::StorageWrite, VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_GENERAL});
    }
    for (uint32_t level = kLevelCount - 1; level > 0; --level) {
        usages.push_back(
            {resourceHandles_.bloomLevels[level],
             RenderImageAccess::SampledRead, VK_IMAGE_LAYOUT_GENERAL,
             VK_IMAGE_LAYOUT_GENERAL});
        usages.push_back(
            {resourceHandles_.bloomLevels[level - 1],
             RenderImageAccess::StorageReadWrite, VK_IMAGE_LAYOUT_GENERAL,
             VK_IMAGE_LAYOUT_GENERAL});
    }
    return usages;
}

void BloomPass::releaseViewportResources() {
    freeDescriptors();
    initialized_.fill(false);
}

void BloomPass::onViewportResize(
    const RenderResourceRegistry &resources) {
    createDescriptors(resources);
    initialized_.fill(false);
}

void BloomPass::execute(const RenderFrameContext &frame,
                        const RenderResourceRegistry &resources,
                        const RenderQueue &) {
    VKL_PROFILE_ZONE("Record Bloom");
    if (!frame.pipelineCache || !frame.view || !frame.shaderVariant)
        return;

    initializeImages(frame, resources);
    const bool active = frame.view->settings.bloomEnabled &&
                        frame.shaderVariant->supportsBloom;
    if (!active)
        return;
    VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd, "Bloom");

    prepareImagesForCompute(frame, resources);

    VkImageMemoryBarrier hdrBarrier = imageBarrier(
        resources.image(resourceHandles_.hdrColor, frame.frameIndex).handle(),
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkCmdPipelineBarrier(
        frame.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0,
        nullptr, 0, nullptr, 1, &hdrBarrier);

    const VkPushConstantRange pushRange{
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BloomPushConstants)};
    ComputePipelineConfig downsampleConfig{};
    downsampleConfig.debugName = "Pipeline/Bloom/Downsample";
    downsampleConfig.computeShaderPath = downsampleShaderPath_;
    downsampleConfig.descriptorLayouts = {descriptorSetLayout_};
    downsampleConfig.pushConstants = {pushRange};
    ComputePipeline &downsamplePipeline =
        frame.pipelineCache->getOrCreateCompute(
            std::move(downsampleConfig));

    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      downsamplePipeline.handle());
    const uint32_t levels = activeLevelCount(resources);
    {
        VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd,
                             "Bloom Downsample");
        for (uint32_t level = 0; level < levels; ++level) {
            ScopedGpuLabel label(device_->debugUtils(), frame.cmd,
                                 "Downsample L" +
                                     std::to_string(level));
            const VkDescriptorSet set =
                downsampleSets_[frame.frameIndex][level];
            vkCmdBindDescriptorSets(
                frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                downsamplePipeline.layout(), 0, 1, &set, 0, nullptr);

            BloomPushConstants push{};
            push.threshold = frame.view->settings.bloomThreshold;
            push.softKnee = frame.view->settings.bloomSoftKnee;
            push.applyThreshold = level == 0 ? 1u : 0u;
            vkCmdPushConstants(frame.cmd, downsamplePipeline.layout(),
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                               &push);

            const VkExtent2D extent =
                resources.extent(resourceHandles_.bloomLevels[level]);
            vkCmdDispatch(frame.cmd, dispatchCount(extent.width),
                          dispatchCount(extent.height), 1);

            VkImageMemoryBarrier barrier = imageBarrier(
                resources
                    .image(resourceHandles_.bloomLevels[level],
                           frame.frameIndex)
                    .handle(),
                VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            vkCmdPipelineBarrier(
                frame.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_DEPENDENCY_BY_REGION_BIT, 0, nullptr, 0, nullptr, 1,
                &barrier);
        }
    }

    if (levels > 1) {
        VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd,
                             "Bloom Upsample");
        ComputePipelineConfig upsampleConfig{};
        upsampleConfig.debugName = "Pipeline/Bloom/Upsample";
        upsampleConfig.computeShaderPath = upsampleShaderPath_;
        upsampleConfig.descriptorLayouts = {descriptorSetLayout_};
        upsampleConfig.pushConstants = {pushRange};
        ComputePipeline &upsamplePipeline =
            frame.pipelineCache->getOrCreateCompute(
                std::move(upsampleConfig));
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          upsamplePipeline.handle());

        for (uint32_t sourceLevel = levels - 1; sourceLevel > 0;
             --sourceLevel) {
            const uint32_t destinationLevel = sourceLevel - 1;
            ScopedGpuLabel label(
                device_->debugUtils(), frame.cmd,
                "Upsample L" + std::to_string(sourceLevel) + " -> L" +
                    std::to_string(destinationLevel));
            const VkDescriptorSet set =
                upsampleSets_[frame.frameIndex][destinationLevel];
            vkCmdBindDescriptorSets(
                frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                upsamplePipeline.layout(), 0, 1, &set, 0, nullptr);

            BloomPushConstants push{};
            push.filterRadius = 1.0f;
            vkCmdPushConstants(frame.cmd, upsamplePipeline.layout(),
                               VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(push), &push);

            const VkExtent2D extent =
                resources.extent(
                    resourceHandles_.bloomLevels[destinationLevel]);
            vkCmdDispatch(frame.cmd, dispatchCount(extent.width),
                          dispatchCount(extent.height), 1);

            VkImageMemoryBarrier barrier = imageBarrier(
                resources
                    .image(resourceHandles_.bloomLevels[destinationLevel],
                           frame.frameIndex)
                    .handle(),
                VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            vkCmdPipelineBarrier(
                frame.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_DEPENDENCY_BY_REGION_BIT, 0, nullptr, 0, nullptr, 1,
                &barrier);
        }
    }

    VkImageMemoryBarrier toneMapBarrier = imageBarrier(
        resources
            .image(resourceHandles_.bloomLevels.front(), frame.frameIndex)
            .handle(),
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdPipelineBarrier(
        frame.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_DEPENDENCY_BY_REGION_BIT, 0, nullptr, 0, nullptr, 1,
        &toneMapBarrier);
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

void BloomPass::initializeImages(
    const RenderFrameContext &frame,
    const RenderResourceRegistry &resources) {
    if (initialized_[frame.frameIndex])
        return;

    std::array<VkImageMemoryBarrier, kLevelCount> barriers{};
    for (uint32_t level = 0; level < kLevelCount; ++level) {
        barriers[level] = imageBarrier(
            resources
                .image(resourceHandles_.bloomLevels[level],
                       frame.frameIndex)
                .handle(),
            0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    }
    vkCmdPipelineBarrier(
        frame.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
        static_cast<uint32_t>(barriers.size()), barriers.data());
    initialized_[frame.frameIndex] = true;
}

void BloomPass::prepareImagesForCompute(
    const RenderFrameContext &frame,
    const RenderResourceRegistry &resources) const {
    std::array<VkImageMemoryBarrier, kLevelCount> barriers{};
    for (uint32_t level = 0; level < kLevelCount; ++level) {
        barriers[level] = imageBarrier(
            resources
                .image(resourceHandles_.bloomLevels[level],
                       frame.frameIndex)
                .handle(),
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    }
    vkCmdPipelineBarrier(
        frame.cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
        static_cast<uint32_t>(barriers.size()), barriers.data());
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
