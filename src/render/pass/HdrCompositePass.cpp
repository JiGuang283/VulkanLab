#include "render/pass/HdrCompositePass.h"

#include "core/Device.h"
#include "core/ComputePipeline.h"
#include "core/ComputePipelineConfig.h"
#include "core/DescriptorAllocator.h"
#include "core/GpuBarrier.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/VulkanCheck.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"
#include "render/RenderFrame.h"
#include "render/RenderResourceRegistry.h"
#include "render/RenderView.h"
#include "render/PipelineCache.h"

#include <glm/glm.hpp>

namespace vkr {

namespace {
VkImageSubresourceRange colorRange() {
    return {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
}
} // namespace

HdrCompositePass::HdrCompositePass(Device &device,
                                   const RenderResourceRegistry &registry,
                                   RendererResourceHandles resources,
                                   DescriptorAllocator &descriptorAllocator,
                                   std::string shaderPath)
    : device_(&device), resources_(resources),
      descriptorAllocator_(&descriptorAllocator),
      shaderPath_(std::move(shaderPath)) {
    createLayout();
    createDescriptors(registry);
}

HdrCompositePass::~HdrCompositePass() {
    freeDescriptors();
    if (descriptorLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     descriptorLayout_, nullptr);
}

std::vector<RenderImageUsage> HdrCompositePass::resourceUsages() const {
    std::vector<RenderImageUsage> usages = {
        {resources_.hdrColor, RenderImageAccess::TransferRead,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {resources_.compositedHdrColor, RenderImageAccess::TransferWrite,
         VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
    };
    if (resources_.baselineSpecular.valid())
        usages.push_back({resources_.baselineSpecular,
                          RenderImageAccess::SampledRead,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    if (resources_.ssrFiltered.valid())
        usages.push_back({resources_.ssrFiltered,
                          RenderImageAccess::SampledRead,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    return usages;
}

void HdrCompositePass::releaseViewportResources() {
    freeDescriptors();
    initialized_.fill(false);
}

void HdrCompositePass::onViewportResize(
    const RenderResourceRegistry &resources) {
    createDescriptors(resources);
}

void HdrCompositePass::execute(const RenderFrameContext &frame,
                               const RenderResourceRegistry &resources,
                               const VisibilityFrame &) {
    VKL_PROFILE_ZONE("Record HDR Composite");
    VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd,
                         "ReflectionComposite");
    ScopedGpuLabel label(device_->debugUtils(), frame.cmd,
                         frame.features.ssrActive
                             ? "ReflectionComposite/SSR"
                             : "ReflectionComposite/CopyBaseline");
    const Image &source = resources.image(resources_.hdrColor,
                                          frame.frameIndex);
    const Image &destination = resources.image(
        resources_.compositedHdrColor, frame.frameIndex);
    const auto range = colorRange();
    if (frame.features.ssrActive && frame.pipelineCache &&
        resources_.ssrFiltered.valid()) {
        cmdImageBarrier(frame.cmd,
                        initialized_[frame.frameIndex]
                            ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                            : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        initialized_[frame.frameIndex]
                            ? VK_ACCESS_SHADER_READ_BIT |
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                            : 0u,
                        VK_ACCESS_SHADER_WRITE_BIT, destination.handle(),
                        initialized_[frame.frameIndex]
                            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                            : VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_GENERAL, range);
        struct CompositePush { glm::uvec4 dimensions{}; } push;
        const VkExtent2D extent = resources.extent(resources_.hdrColor);
        push.dimensions = {1u, extent.width, extent.height, 0u};
        ComputePipelineConfig config{};
        config.debugName = "Pipeline/ScreenSpace/ReflectionComposite";
        config.computeShaderPath = shaderPath_;
        config.descriptorLayouts = {descriptorLayout_};
        config.pushConstants = {{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                 sizeof(CompositePush)}};
        ComputePipeline &pipeline =
            frame.pipelineCache->getOrCreateCompute(std::move(config));
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipeline.handle());
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline.layout(), 0, 1,
                                &descriptorSets_[frame.frameIndex], 0,
                                nullptr);
        vkCmdPushConstants(frame.cmd, pipeline.layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                           &push);
        vkCmdDispatch(frame.cmd, (extent.width + 7u) / 8u,
                      (extent.height + 7u) / 8u, 1);
        cmdImageBarrier(frame.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        destination.handle(), VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, range);
        initialized_[frame.frameIndex] = true;
        return;
    }

    cmdImageBarrier(frame.cmd,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, source.handle(),
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, range);
    cmdImageBarrier(frame.cmd,
                    initialized_[frame.frameIndex]
                        ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                        : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    initialized_[frame.frameIndex]
                        ? VK_ACCESS_SHADER_READ_BIT |
                              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                        : 0u,
                    VK_ACCESS_TRANSFER_WRITE_BIT, destination.handle(),
                    initialized_[frame.frameIndex]
                        ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                        : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, range);

    const VkExtent2D extent = resources.extent(resources_.hdrColor);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {extent.width, extent.height, 1};
    vkCmdCopyImage(frame.cmd, source.handle(),
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   destination.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &copy);

    cmdImageBarrier(frame.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                    source.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, range);
    cmdImageBarrier(frame.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    destination.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, range);
    initialized_[frame.frameIndex] = true;
}

void HdrCompositePass::createLayout() {
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    for (uint32_t i = 0; i < 3; ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = uint32_t(bindings.size());
    info.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &descriptorLayout_));
}

void HdrCompositePass::createDescriptors(
    const RenderResourceRegistry &registry) {
    if (!resources_.ssrFiltered.valid())
        return;
    const VkSampler hdrSampler = registry.sampler(resources_.hdrSampler);
    const VkSampler ssrSampler = registry.sampler(resources_.ssrSampler);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        descriptorSets_[frame] = descriptorAllocator_->allocate(
            descriptorLayout_,
            {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
             {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}},
            "ReflectionComposite/Frame" + std::to_string(frame));
        std::array<VkDescriptorImageInfo, 4> infos = {{
            {hdrSampler, registry.image(resources_.hdrColor, frame).imageView(),
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {hdrSampler, registry.image(resources_.baselineSpecular, frame).imageView(),
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {ssrSampler, registry.image(resources_.ssrFiltered, frame).imageView(),
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE,
             registry.image(resources_.compositedHdrColor, frame).imageView(),
             VK_IMAGE_LAYOUT_GENERAL}}};
        std::array<VkWriteDescriptorSet, 4> writes{};
        for (uint32_t i = 0; i < writes.size(); ++i) {
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = descriptorSets_[frame];
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i < 3
                ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device_->logicalDevice(), uint32_t(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

void HdrCompositePass::freeDescriptors() {
    for (VkDescriptorSet &set : descriptorSets_) {
        if (set != VK_NULL_HANDLE) descriptorAllocator_->free(set);
        set = VK_NULL_HANDLE;
    }
}

} // namespace vkr
