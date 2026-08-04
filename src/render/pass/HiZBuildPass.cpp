#include "render/pass/HiZBuildPass.h"

#include "core/ComputePipeline.h"
#include "core/ComputePipelineConfig.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/VulkanCheck.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"
#include "render/PipelineCache.h"
#include "render/RenderFrame.h"
#include "render/RenderResourceRegistry.h"
#include "render/RenderView.h"
#include "render/Visibility.h"

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
                           const RenderResourceRegistry &resources,
                           RendererResourceHandles resourceHandles,
                           DescriptorAllocator &descriptorAllocator,
                           std::string initShaderPath,
                           std::string reduceShaderPath)
    : device_(&device), resourceHandles_(resourceHandles),
      descriptorAllocator_(&descriptorAllocator),
      initShaderPath_(std::move(initShaderPath)),
      reduceShaderPath_(std::move(reduceShaderPath)) {
    if (!resourceHandles_.visibilityDepth.valid() ||
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

std::vector<RenderImageUsage> HiZBuildPass::resourceUsages() const {
    return {
        {resourceHandles_.visibilityDepth, RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        {resourceHandles_.visibilityHiZ,
         RenderImageAccess::StorageWrite, VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_GENERAL},
        {resourceHandles_.visibilityHiZ,
         RenderImageAccess::StorageReadWrite, VK_IMAGE_LAYOUT_GENERAL,
         VK_IMAGE_LAYOUT_GENERAL}};
}

void HiZBuildPass::releaseViewportResources() {
    freeDescriptors();
    initialized_.fill(false);
}

void HiZBuildPass::onViewportResize(
    const RenderResourceRegistry &resources) {
    createDescriptors(resources);
    initialized_.fill(false);
}

void HiZBuildPass::execute(const RenderFrameContext &frame,
                           const RenderResourceRegistry &resources,
                           const VisibilityFrame &visibility) {
    if (!frame.pipelineCache || !frame.view ||
        !frame.view->settings.culling.occlusionEnabled ||
        visibility.stats.occlusionCandidates == 0) {
        return;
    }
    VKL_PROFILE_ZONE("Record HiZBuild");
    VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd, "HiZBuild");
    const uint32_t mipCount =
        resources.mipLevelCount(resourceHandles_.visibilityHiZ);
    const Image &hiZ = resources.image(resourceHandles_.visibilityHiZ,
                                       frame.frameIndex);
    VkImageMemoryBarrier transition{};
    transition.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    transition.srcAccessMask = initialized_[frame.frameIndex]
                                   ? VK_ACCESS_SHADER_READ_BIT |
                                         VK_ACCESS_SHADER_WRITE_BIT
                                   : 0u;
    transition.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    transition.oldLayout = initialized_[frame.frameIndex]
                               ? VK_IMAGE_LAYOUT_GENERAL
                               : VK_IMAGE_LAYOUT_UNDEFINED;
    transition.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    transition.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    transition.image = hiZ.handle();
    transition.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    transition.subresourceRange.levelCount = mipCount;
    transition.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(
        frame.cmd,
        initialized_[frame.frameIndex] ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                       : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &transition);
    initialized_[frame.frameIndex] = true;

    const VkPushConstantRange pushRange{
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(HiZPush)};
    ComputePipelineConfig initConfig{};
    initConfig.debugName = "Pipeline/Visibility/HiZInit";
    initConfig.computeShaderPath = initShaderPath_;
    initConfig.descriptorLayouts = {descriptorSetLayout_};
    initConfig.pushConstants = {pushRange};
    ComputePipelineConfig reduceConfig{};
    reduceConfig.debugName = "Pipeline/Visibility/HiZReduce";
    reduceConfig.computeShaderPath = reduceShaderPath_;
    reduceConfig.descriptorLayouts = {descriptorSetLayout_};
    reduceConfig.pushConstants = {pushRange};
    ComputePipeline &initPipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(initConfig));
    ComputePipeline &reducePipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(reduceConfig));

    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        ScopedGpuLabel label(device_->debugUtils(), frame.cmd,
                             "HiZ Mip " + std::to_string(mip));
        ComputePipeline &pipeline = mip == 0 ? initPipeline : reducePipeline;
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipeline.handle());
        const VkDescriptorSet set = sets_[frame.frameIndex].at(mip);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline.layout(), 0, 1, &set, 0, nullptr);
        const VkExtent2D destination =
            resources.mipExtent(resourceHandles_.visibilityHiZ, mip);
        const VkExtent2D source =
            mip == 0
                ? resources.extent(resourceHandles_.visibilityDepth)
                : resources.mipExtent(resourceHandles_.visibilityHiZ,
                                      mip - 1u);
        const HiZPush push{{source.width, source.height,
                            destination.width, destination.height}};
        vkCmdPushConstants(frame.cmd, pipeline.layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        vkCmdDispatch(frame.cmd, dispatchCount(destination.width),
                      dispatchCount(destination.height), 1);

        VkImageMemoryBarrier ready{};
        ready.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ready.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        ready.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        ready.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        ready.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ready.image = hiZ.handle();
        ready.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ready.subresourceRange.baseMipLevel = mip;
        ready.subresourceRange.levelCount = 1;
        ready.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(frame.cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &ready);
    }
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
    const RenderResourceRegistry &resources) {
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
                    resourceHandles_.visibilityDepthSampler);
                source.imageView =
                    resources.image(resourceHandles_.visibilityDepth, frame)
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
