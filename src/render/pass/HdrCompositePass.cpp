#include "render/pass/HdrCompositePass.h"

#include "core/Device.h"
#include "core/ComputePipeline.h"
#include "core/ComputePipelineConfig.h"
#include "core/DescriptorAllocator.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/VulkanCheck.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"
#include "render/RenderFrame.h"
#include "render/RenderGraph.h"
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

void HdrCompositePass::setup(RenderGraphBuilder &builder,
                             const RenderGraphBuildContext &context) const {
    const bool composite = context.features.ssrActive ||
                           context.features.ssgiActive;
    builder.addNode("ScreenSpaceLightingComposite/Compute",
                    RgPassType::Compute, RgQueueClass::Compute, 0);
    builder.setActive(composite);
    builder.useImage({resources_.hdrColor, RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    if (context.features.ssrActive) {
        for (RenderImageHandle handle :
             {resources_.baselineSpecular, resources_.ssrFiltered}) {
            if (handle.valid()) {
                builder.useImage({handle, RenderImageAccess::SampledRead,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            }
        }
    }
    if (context.features.ssgiActive) {
        for (RenderImageHandle handle :
             {resources_.baselineDiffuse, resources_.ssgiFiltered}) {
            if (handle.valid()) {
                builder.useImage({handle, RenderImageAccess::SampledRead,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            }
        }
    }
    builder.useImage({resources_.compositedHdrColor,
                      RenderImageAccess::StorageWrite,
                      VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});

    builder.addNode("ScreenSpaceLightingComposite/Copy",
                    RgPassType::Transfer, RgQueueClass::Transfer, 1);
    builder.setActive(!composite);
    builder.useImage({resources_.hdrColor, RenderImageAccess::TransferRead,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    builder.useImage({resources_.compositedHdrColor,
                      RenderImageAccess::TransferWrite,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
}

void HdrCompositePass::recordNode(RenderGraphPassContext &context,
                                  uint32_t localNodeIndex,
                                  const VisibilityFrame &) {
    if (localNodeIndex == 0)
        recordComposite(context.frame, context.resources);
    else if (localNodeIndex == 1)
        recordCopy(context.frame, context.resources);
}

void HdrCompositePass::recordComposite(
    const RenderFrameContext &frame,
    const RenderResourceRegistry &resources) {
    const bool ssrActive = frame.features.ssrActive &&
                           resources_.ssrFiltered.valid();
    const bool ssgiActive = frame.features.ssgiActive &&
                            resources_.ssgiFiltered.valid();
    if (!frame.pipelineCache || (!ssrActive && !ssgiActive))
        return;
    updateDescriptor(resources, frame.frameIndex, ssrActive, ssgiActive);
    struct CompositePush { glm::uvec4 dimensions{}; } push;
    const VkExtent2D extent = resources.extent(resources_.hdrColor);
    push.dimensions = {ssrActive ? 1u : 0u, ssgiActive ? 1u : 0u,
                       extent.width, extent.height};
    ComputePipelineConfig config{};
    config.debugName = "Pipeline/ScreenSpace/LightingComposite";
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
                            &descriptorSets_[frame.frameIndex], 0, nullptr);
    vkCmdPushConstants(frame.cmd, pipeline.layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(frame.cmd, (extent.width + 7u) / 8u,
                  (extent.height + 7u) / 8u, 1);
}

void HdrCompositePass::recordCopy(const RenderFrameContext &frame,
                                  const RenderResourceRegistry &resources) {
    const VkExtent2D extent = resources.extent(resources_.hdrColor);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {extent.width, extent.height, 1};
    vkCmdCopyImage(
        frame.cmd,
        resources.image(resources_.hdrColor, frame.frameIndex).handle(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        resources.image(resources_.compositedHdrColor,
                        frame.frameIndex).handle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

void HdrCompositePass::releaseViewportResources() {
    freeDescriptors();
}

void HdrCompositePass::onViewportResize(
    const RenderResourceRegistry &resources) {
    createDescriptors(resources);
}

void HdrCompositePass::createLayout() {
    std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
    for (uint32_t i = 0; i < 5; ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
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
    if (!resources_.ssrFiltered.valid() && !resources_.ssgiFiltered.valid())
        return;
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        descriptorSets_[frame] = descriptorAllocator_->allocate(
            descriptorLayout_,
            {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5},
             {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}},
            "ScreenSpaceLightingComposite/Frame" + std::to_string(frame));
        updateDescriptor(registry, frame, false, false);
    }
}

void HdrCompositePass::updateDescriptor(
    const RenderResourceRegistry &registry, uint32_t frame,
    bool ssrActive, bool ssgiActive) {
    const VkSampler hdrSampler = registry.sampler(resources_.hdrSampler);
    const VkSampler ssrSampler = resources_.ssrSampler.valid()
        ? registry.sampler(resources_.ssrSampler) : hdrSampler;
    const VkSampler ssgiSampler = resources_.ssgiSampler.valid()
        ? registry.sampler(resources_.ssgiSampler) : hdrSampler;
    const VkImageView fallback =
        registry.image(resources_.hdrColor, frame).imageView();
    const VkImageView baselineSpecular = resources_.baselineSpecular.valid()
        ? registry.image(resources_.baselineSpecular, frame).imageView()
        : fallback;
    const VkImageView ssr = ssrActive && resources_.ssrFiltered.valid()
        ? registry.image(resources_.ssrFiltered, frame).imageView()
        : fallback;
    const VkImageView baselineDiffuse = resources_.baselineDiffuse.valid()
        ? registry.image(resources_.baselineDiffuse, frame).imageView()
        : fallback;
    const VkImageView ssgi = ssgiActive && resources_.ssgiFiltered.valid()
        ? registry.image(resources_.ssgiFiltered, frame).imageView()
        : fallback;
    std::array<VkDescriptorImageInfo, 6> infos = {{
        {hdrSampler, fallback, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {hdrSampler, baselineSpecular, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {ssrActive ? ssrSampler : hdrSampler, ssr,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {hdrSampler, baselineDiffuse, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {ssgiActive ? ssgiSampler : hdrSampler, ssgi,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE,
         registry.image(resources_.compositedHdrColor, frame).imageView(),
         VK_IMAGE_LAYOUT_GENERAL}}};
    std::array<VkWriteDescriptorSet, 6> writes{};
    for (uint32_t index = 0; index < writes.size(); ++index) {
        writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[index].dstSet = descriptorSets_[frame];
        writes[index].dstBinding = index;
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = index < 5
            ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[index].pImageInfo = &infos[index];
    }
    vkUpdateDescriptorSets(device_->logicalDevice(), uint32_t(writes.size()),
                           writes.data(), 0, nullptr);
}

void HdrCompositePass::freeDescriptors() {
    for (VkDescriptorSet &set : descriptorSets_) {
        if (set != VK_NULL_HANDLE) descriptorAllocator_->free(set);
        set = VK_NULL_HANDLE;
    }
}

} // namespace vkr
