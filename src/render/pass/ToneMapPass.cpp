#include "ToneMapPass.h"

#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/Pipeline.h"
#include "core/PipelineConfigBuilder.h"
#include "core/SwapChain.h"
#include "core/VulkanCheck.h"
#include "render/FrameGpuData.h"
#include "render/GuiSystem.h"
#include "render/PipelineCache.h"
#include "render/PipelineKey.h"
#include "render/RenderFrame.h"
#include "render/RenderResourceRegistry.h"
#include "render/RenderView.h"
#include "render/ShaderVariant.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"

#include <array>
#include <utility>

namespace vkr {

namespace {

uint32_t toneMapperValue(ToneMapper toneMapper) {
    switch (toneMapper) {
    case ToneMapper::PassThrough:
        return 0;
    case ToneMapper::Reinhard:
        return 1;
    case ToneMapper::Aces:
        return 2;
    }
    return 2;
}

bool isSrgbFormat(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_SRGB ||
           format == VK_FORMAT_R8G8B8A8_SRGB;
}

} // namespace

ToneMapPass::ToneMapPass(Device &device, SwapChain &swapChain,
                         const RenderResourceRegistry &resources,
                         RenderImageHandle hdrColor,
                         RenderSamplerHandle hdrSampler,
                         RenderImageHandle bloomColor,
                         RenderSamplerHandle bloomSampler,
                         DescriptorAllocator &descriptorAllocator,
                         std::string fullscreenVertPath,
                         std::string toneMapFragPath)
    : device_(&device), swapChain_(&swapChain), hdrColor_(hdrColor),
      hdrSampler_(hdrSampler), bloomColor_(bloomColor),
      bloomSampler_(bloomSampler),
      descriptorAllocator_(&descriptorAllocator),
      fullscreenVertPath_(std::move(fullscreenVertPath)),
      toneMapFragPath_(std::move(toneMapFragPath)) {
    createRenderPass();
    createDescriptors(resources);
    createFramebuffers();
}

ToneMapPass::~ToneMapPass() {
    destroyFramebuffers();
    for (VkDescriptorSet set : sourceDescriptorSets_)
        descriptorAllocator_->free(set);
    if (sourceDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     sourceDescriptorSetLayout_, nullptr);
    }
    if (renderPass_ != VK_NULL_HANDLE)
        vkDestroyRenderPass(device_->logicalDevice(), renderPass_, nullptr);
}

void ToneMapPass::releaseSwapChainResources() {
    destroyFramebuffers();
}

std::vector<RenderImageUsage> ToneMapPass::resourceUsages() const {
    std::vector<RenderImageUsage> usages = {
        {hdrColor_, RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    if (bloomColor_.valid()) {
        usages.push_back(
            {bloomColor_, RenderImageAccess::SampledRead,
             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
    }
    return usages;
}

void ToneMapPass::onResize(const SwapChain &,
                           const RenderResourceRegistry &resources) {
    updateDescriptors(resources);
    createFramebuffers();
}

void ToneMapPass::execute(const RenderFrameContext &frame,
                          const RenderResourceRegistry &,
                          const RenderQueue &) {
    VKL_PROFILE_ZONE("Record ToneMap And UI");
    VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd,
                         "ToneMap + UI");
    if (!frame.pipelineCache || !frame.view || !frame.shaderVariant)
        return;

    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = renderPass_;
    beginInfo.framebuffer = framebuffers_.at(frame.imageIndex);
    beginInfo.renderArea = {{0, 0}, frame.extent};
    beginInfo.clearValueCount = 1;
    beginInfo.pClearValues = &clear;
    vkCmdBeginRenderPass(frame.cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(frame.extent.width);
    viewport.height = static_cast<float>(frame.extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, frame.extent};
    vkCmdSetScissor(frame.cmd, 0, 1, &scissor);

    {
        VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd,
                             "FullscreenToneMap");
        ScopedGpuLabel label(device_->debugUtils(), frame.cmd,
                             "FullscreenToneMap");
        PipelineConfig config =
            PipelineConfigBuilder{}
                .shaders(fullscreenVertPath_, toneMapFragPath_)
                .vertexLayout(VertexLayout{})
                .rasterization(VK_CULL_MODE_NONE,
                               VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .depth(false, false)
                .blending(false)
                .msaa(VK_SAMPLE_COUNT_1_BIT)
                .descriptorLayout(sourceDescriptorSetLayout_)
                .pushConstant({VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(ToneMapPushConstants)})
                .build();
        config.debugName = "Pipeline/ToneMap/Fullscreen";

        Pipeline &pipeline =
            frame.pipelineCache->getOrCreate(renderPass_, std::move(config));
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline.handle());
        const VkDescriptorSet sourceSet =
            sourceDescriptorSets_.at(frame.frameIndex);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline.layout(), 0, 1, &sourceSet, 0,
                                nullptr);

        ToneMapPushConstants push{};
        if (frame.shaderVariant->toneMapping ==
            ShaderToneMappingPolicy::Configurable) {
            push.exposureEv = frame.view->settings.exposureEv;
            push.toneMapper = toneMapperValue(frame.view->settings.toneMapper);
            push.applyExposure = 1;
        }
        if (bloomColor_.valid() && frame.view->settings.bloomEnabled &&
            frame.shaderVariant->supportsBloom) {
            push.bloomIntensity = frame.view->settings.bloomIntensity;
            push.applyBloom = 1;
        }
        push.encodeGamma =
            isSrgbFormat(swapChain_->imageFormat()) ? 0u : 1u;
        vkCmdPushConstants(frame.cmd, pipeline.layout(),
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                           &push);
        vkCmdDraw(frame.cmd, 3, 1, 0, 0);
    }

    if (frame.gui) {
        VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd, "ImGui");
        ScopedGpuLabel label(device_->debugUtils(), frame.cmd, "ImGui");
        frame.gui->render(frame.cmd);
    }
    vkCmdEndRenderPass(frame.cmd);
}

void ToneMapPass::createRenderPass() {
    VkAttachmentDescription color{};
    color.format = swapChain_->imageFormat();
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{0,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    VK_CHECK(vkCreateRenderPass(device_->logicalDevice(), &info, nullptr,
                                &renderPass_));
    device_->debugUtils().setObjectName(VK_OBJECT_TYPE_RENDER_PASS,
                                        renderPass_,
                                        "Pass/ToneMap/RenderPass");
}

void ToneMapPass::createFramebuffers() {
    framebuffers_.resize(swapChain_->imageViews().size());
    for (size_t index = 0; index < framebuffers_.size(); ++index) {
        const VkImageView attachment = swapChain_->imageViews()[index];
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = renderPass_;
        info.attachmentCount = 1;
        info.pAttachments = &attachment;
        info.width = swapChain_->extent().width;
        info.height = swapChain_->extent().height;
        info.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_->logicalDevice(), &info, nullptr,
                                     &framebuffers_[index]));
        device_->debugUtils().setObjectName(
            VK_OBJECT_TYPE_FRAMEBUFFER, framebuffers_[index],
            "Pass/ToneMap/Framebuffer/Image" + std::to_string(index));
    }
}

void ToneMapPass::destroyFramebuffers() {
    for (VkFramebuffer framebuffer : framebuffers_)
        vkDestroyFramebuffer(device_->logicalDevice(), framebuffer, nullptr);
    framebuffers_.clear();
}

void ToneMapPass::createDescriptors(
    const RenderResourceRegistry &resources) {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t bindingIndex = 0; bindingIndex < bindings.size();
         ++bindingIndex) {
        bindings[bindingIndex].binding = bindingIndex;
        bindings[bindingIndex].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[bindingIndex].descriptorCount = 1;
        bindings[bindingIndex].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr,
                                         &sourceDescriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, sourceDescriptorSetLayout_,
        "Pass/ToneMap/SourceDescriptorSetLayout");
    for (uint32_t frame = 0; frame < sourceDescriptorSets_.size(); ++frame) {
        sourceDescriptorSets_[frame] = descriptorAllocator_->allocate(
            sourceDescriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}},
            "Pass/ToneMap/SourceDescriptorSet/Frame" +
                std::to_string(frame));
    }
    updateDescriptors(resources);
}

void ToneMapPass::updateDescriptors(
    const RenderResourceRegistry &resources) {
    for (uint32_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT;
         ++frameIndex) {
        VkDescriptorImageInfo hdrInfo{};
        hdrInfo.sampler = resources.sampler(hdrSampler_);
        hdrInfo.imageView =
            resources.image(hdrColor_, frameIndex).imageView();
        hdrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo bloomInfo{};
        if (bloomColor_.valid()) {
            bloomInfo.sampler = resources.sampler(bloomSampler_);
            bloomInfo.imageView =
                resources.image(bloomColor_, frameIndex).imageView();
            bloomInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        } else {
            bloomInfo = hdrInfo;
        }

        std::array<VkWriteDescriptorSet, 2> writes{};
        for (uint32_t bindingIndex = 0; bindingIndex < writes.size();
             ++bindingIndex) {
            writes[bindingIndex].sType =
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[bindingIndex].dstSet =
                sourceDescriptorSets_[frameIndex];
            writes[bindingIndex].dstBinding = bindingIndex;
            writes[bindingIndex].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[bindingIndex].descriptorCount = 1;
        }
        writes[0].pImageInfo = &hdrInfo;
        writes[1].pImageInfo = &bloomInfo;
        vkUpdateDescriptorSets(
            device_->logicalDevice(),
            static_cast<uint32_t>(writes.size()), writes.data(), 0,
            nullptr);
    }
}

} // namespace vkr
