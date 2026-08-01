#include "SkyboxPass.h"

#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/Pipeline.h"
#include "core/PipelineConfigBuilder.h"
#include "core/VulkanCheck.h"
#include "render/PipelineCache.h"
#include "render/RenderFrame.h"
#include "render/RenderResourceRegistry.h"
#include "render/RenderView.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"

#include <array>
#include <utility>

namespace vkr {

SkyboxPass::SkyboxPass(
    Device &device, const RenderResourceRegistry &resources,
    RendererResourceHandles resourceHandles,
    VkDescriptorSetLayout globalDescriptorSetLayout,
    VkDescriptorSetLayout lightingDescriptorSetLayout,
    std::string vertexShaderPath, std::string fragmentShaderPath)
    : device_(&device), resourceHandles_(resourceHandles),
      globalDescriptorSetLayout_(globalDescriptorSetLayout),
      lightingDescriptorSetLayout_(lightingDescriptorSetLayout),
      vertexShaderPath_(std::move(vertexShaderPath)),
      fragmentShaderPath_(std::move(fragmentShaderPath)) {
    VkDescriptorSetLayoutCreateInfo emptyInfo{};
    emptyInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    VK_CHECK(vkCreateDescriptorSetLayout(
        device_->logicalDevice(), &emptyInfo, nullptr,
        &emptyDescriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
        emptyDescriptorSetLayout_, "Pass/Skybox/EmptySetLayout");
    createRenderPass(resources);
    createFramebuffers(resources);
}

SkyboxPass::~SkyboxPass() {
    destroyFramebuffers();
    if (renderPass_ != VK_NULL_HANDLE)
        vkDestroyRenderPass(device_->logicalDevice(), renderPass_, nullptr);
    if (emptyDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     emptyDescriptorSetLayout_, nullptr);
    }
}

std::vector<RenderImageUsage> SkyboxPass::resourceUsages() const {
    const RenderImageHandle target =
        resourceHandles_.hdrMsaaColor.valid()
            ? resourceHandles_.hdrMsaaColor
            : resourceHandles_.hdrColor;
    return {{target, RenderImageAccess::ColorAttachmentWrite,
             VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
}

void SkyboxPass::releaseViewportResources() {
    destroyFramebuffers();
}

void SkyboxPass::onViewportResize(
    const RenderResourceRegistry &resources) {
    createFramebuffers(resources);
}

void SkyboxPass::execute(const RenderFrameContext &frame,
                         const RenderResourceRegistry &resources,
                         const RenderQueue &) {
    VKL_PROFILE_ZONE("Record Skybox");
    VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd, "Skybox");
    const RenderImageHandle target =
        resourceHandles_.hdrMsaaColor.valid()
            ? resourceHandles_.hdrMsaaColor
            : resourceHandles_.hdrColor;
    const VkExtent2D extent = resources.extent(target);
    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = renderPass_;
    begin.framebuffer = framebuffers_.at(frame.frameIndex);
    begin.renderArea.extent = extent;
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    vkCmdBeginRenderPass(frame.cmd, &begin,
                         VK_SUBPASS_CONTENTS_INLINE);

    if (frame.environmentReady && frame.view &&
        frame.view->settings.skyboxEnabled && frame.pipelineCache) {
        PipelineConfig config =
            PipelineConfigBuilder{}
                .shaders(vertexShaderPath_, fragmentShaderPath_)
                .vertexLayout({})
                .rasterization(VK_CULL_MODE_NONE,
                               VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .depth(false, false)
                .msaa(resources.description(target).samples)
                .descriptorLayout(globalDescriptorSetLayout_)
                .descriptorLayout(emptyDescriptorSetLayout_)
                .descriptorLayout(lightingDescriptorSetLayout_)
                .build();
        config.debugName = "Pipeline/Skybox";
        Pipeline &pipeline =
            frame.pipelineCache->getOrCreate(renderPass_, std::move(config));
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline.handle());
        vkCmdBindDescriptorSets(
            frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
            0, 1, &frame.globalDescriptorSet, 0, nullptr);
        vkCmdBindDescriptorSets(
            frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
            2, 1, &frame.lightingDescriptorSet, 0, nullptr);
        VkViewport viewport{};
        viewport.width = static_cast<float>(extent.width);
        viewport.height = static_cast<float>(extent.height);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetScissor(frame.cmd, 0, 1, &scissor);
        vkCmdDraw(frame.cmd, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(frame.cmd);
}

void SkyboxPass::createRenderPass(
    const RenderResourceRegistry &resources) {
    const RenderImageHandle target =
        resourceHandles_.hdrMsaaColor.valid()
            ? resourceHandles_.hdrMsaaColor
            : resourceHandles_.hdrColor;
    const RenderImageDesc &desc = resources.description(target);
    VkAttachmentDescription color{};
    color.format = desc.format;
    color.samples = desc.samples;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colorRef{
        0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependency.dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
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
                                        "Pass/Skybox/RenderPass");
}

void SkyboxPass::createFramebuffers(
    const RenderResourceRegistry &resources) {
    const RenderImageHandle target =
        resourceHandles_.hdrMsaaColor.valid()
            ? resourceHandles_.hdrMsaaColor
            : resourceHandles_.hdrColor;
    const VkExtent2D extent = resources.extent(target);
    for (uint32_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT;
         ++frameIndex) {
        const VkImageView attachment =
            resources.image(target, frameIndex).imageView();
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = renderPass_;
        info.attachmentCount = 1;
        info.pAttachments = &attachment;
        info.width = extent.width;
        info.height = extent.height;
        info.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_->logicalDevice(), &info,
                                     nullptr,
                                     &framebuffers_[frameIndex]));
        device_->debugUtils().setObjectName(
            VK_OBJECT_TYPE_FRAMEBUFFER, framebuffers_[frameIndex],
            "Pass/Skybox/Framebuffer/Frame" +
                std::to_string(frameIndex));
    }
}

void SkyboxPass::destroyFramebuffers() {
    for (VkFramebuffer &framebuffer : framebuffers_) {
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_->logicalDevice(), framebuffer,
                                 nullptr);
            framebuffer = VK_NULL_HANDLE;
        }
    }
}

} // namespace vkr
