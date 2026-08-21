#include "PresentPass.h"

#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "render/pipeline/Pipeline.h"
#include "render/pipeline/PipelineConfigBuilder.h"
#include "core/SwapChain.h"
#include "core/VulkanCheck.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"
#include "render/pipeline/PipelineCache.h"
#include "render/frame/RenderFrame.h"
#include "render/graph/RenderGraph.h"
#include "render/graph/RenderResourcePool.h"

#include <utility>

namespace vkr {

PresentPass::PresentPass(Device &device, SwapChain &swapChain,
                         const RenderResourcePool &resources,
                         RenderImageHandle viewportColor,
                         RenderSamplerHandle viewportSampler,
                         DescriptorAllocator &descriptorAllocator,
                         std::string fullscreenVertPath,
                         std::string presentFragPath)
    : device_(&device), swapChain_(&swapChain),
      viewportColor_(viewportColor), viewportSampler_(viewportSampler),
      descriptorAllocator_(&descriptorAllocator),
      fullscreenVertPath_(std::move(fullscreenVertPath)),
      presentFragPath_(std::move(presentFragPath)) {
    createDescriptors(resources);
}

PresentPass::~PresentPass() {
    for (VkDescriptorSet set : sourceDescriptorSets_)
        descriptorAllocator_->free(set);
    if (sourceDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     sourceDescriptorSetLayout_, nullptr);
    }
}

void PresentPass::setup(RenderGraphBuilder &builder,
                        const RenderGraphBuildContext &) const {
    builder.addNode(std::string(name()), RgPassType::Graphics,
                    RgQueueClass::Graphics);
    builder.useImage({viewportColor_, RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    builder.addSwapchainColorAttachment(
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
        VkClearColorValue{{0.025f, 0.025f, 0.03f, 1.0f}});
}

void PresentPass::onViewportResize(
    const RenderResourcePool &resources) {
    updateDescriptors(resources);
}

void PresentPass::recordNode(RenderGraphPassContext &context, uint32_t,
                             const VisibilityFrame &) {
    const RenderFrameContext &frame = context.frame;
    VKL_PROFILE_ZONE("Record Present And UI");
    VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd, "Present + UI");
    if (!frame.pipelineCache)
        return;

    VkViewport viewport{};
    viewport.width = static_cast<float>(frame.swapchainExtent.width);
    viewport.height = static_cast<float>(frame.swapchainExtent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, frame.swapchainExtent};
    vkCmdSetScissor(frame.cmd, 0, 1, &scissor);

    if (frame.drawUi) {
        VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd, "ImGui");
        ScopedGpuLabel label(device_->debugUtils(), frame.cmd, "ImGui");
        frame.drawUi(frame.cmd);
    } else {
        VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd,
                             "FullscreenPresent");
        ScopedGpuLabel label(device_->debugUtils(), frame.cmd,
                             "FullscreenPresent");
        PipelineConfig config =
            PipelineConfigBuilder{}
                .shaders(fullscreenVertPath_, presentFragPath_)
                .vertexLayout(VertexLayout{})
                .rasterization(VK_CULL_MODE_NONE,
                               VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .depth(false, false)
                .blending(false)
                .msaa(VK_SAMPLE_COUNT_1_BIT)
                .descriptorLayout(sourceDescriptorSetLayout_)
                .build();
        config.debugName = "Pipeline/Present/Fullscreen";
        PipelineRenderingSignature signature{};
        signature.colorAttachmentFormats = {swapChain_->imageFormat()};
        Pipeline &pipeline = frame.pipelineCache->getOrCreate(
            std::move(signature), std::move(config));
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline.handle());
        const VkDescriptorSet sourceSet =
            sourceDescriptorSets_.at(frame.frameIndex);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline.layout(), 0, 1, &sourceSet, 0,
                                nullptr);
        vkCmdDraw(frame.cmd, 3, 1, 0, 0);
    }

}

void PresentPass::createDescriptors(
    const RenderResourcePool &resources) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 1;
    info.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr,
                                         &sourceDescriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, sourceDescriptorSetLayout_,
        "Pass/Present/SourceDescriptorSetLayout");
    for (uint32_t frame = 0; frame < sourceDescriptorSets_.size(); ++frame) {
        sourceDescriptorSets_[frame] = descriptorAllocator_->allocate(
            sourceDescriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}},
            "Pass/Present/SourceDescriptorSet/Frame" +
                std::to_string(frame));
    }
    updateDescriptors(resources);
}

void PresentPass::updateDescriptors(
    const RenderResourcePool &resources) {
    for (uint32_t frame = 0; frame < sourceDescriptorSets_.size(); ++frame) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = resources.sampler(viewportSampler_);
        imageInfo.imageView = resources.image(viewportColor_, frame).imageView();
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = sourceDescriptorSets_[frame];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device_->logicalDevice(), 1, &write, 0,
                               nullptr);
    }
}

} // namespace vkr
