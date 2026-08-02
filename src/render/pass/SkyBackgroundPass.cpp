#include "SkyBackgroundPass.h"

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
#include "render/ShaderVariant.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"

#include <array>
#include <utility>

namespace vkr {

SkyBackgroundPass::SkyBackgroundPass(
    Device &device, const RenderResourceRegistry &resources,
    RendererResourceHandles resourceHandles,
    VkDescriptorSetLayout globalDescriptorSetLayout,
    VkDescriptorSetLayout lightingDescriptorSetLayout,
    VkDescriptorSetLayout atmosphereDescriptorSetLayout,
    std::string vertexShaderPath, std::string skyboxFragmentShaderPath,
    std::string atmosphereFragmentShaderPath)
    : device_(&device), resourceHandles_(resourceHandles),
      globalDescriptorSetLayout_(globalDescriptorSetLayout),
      lightingDescriptorSetLayout_(lightingDescriptorSetLayout),
      atmosphereDescriptorSetLayout_(atmosphereDescriptorSetLayout),
      vertexShaderPath_(std::move(vertexShaderPath)),
      skyboxFragmentShaderPath_(std::move(skyboxFragmentShaderPath)),
      atmosphereFragmentShaderPath_(
          std::move(atmosphereFragmentShaderPath)) {
    VkDescriptorSetLayoutCreateInfo emptyInfo{};
    emptyInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    VK_CHECK(vkCreateDescriptorSetLayout(
        device_->logicalDevice(), &emptyInfo, nullptr,
        &emptyDescriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
        emptyDescriptorSetLayout_, "Pass/SkyBackground/EmptySetLayout");
    createRenderPass(resources);
    createFramebuffers(resources);
}

SkyBackgroundPass::~SkyBackgroundPass() {
    destroyFramebuffers();
    if (renderPass_ != VK_NULL_HANDLE)
        vkDestroyRenderPass(device_->logicalDevice(), renderPass_, nullptr);
    if (emptyDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     emptyDescriptorSetLayout_, nullptr);
    }
}

std::vector<RenderImageUsage> SkyBackgroundPass::resourceUsages() const {
    const RenderImageHandle target =
        resourceHandles_.hdrMsaaColor.valid()
            ? resourceHandles_.hdrMsaaColor
            : resourceHandles_.hdrColor;
    return {{resourceHandles_.atmosphereTransmittance,
             RenderImageAccess::SampledRead,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {resourceHandles_.atmosphereSkyView,
             RenderImageAccess::SampledRead,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {target, RenderImageAccess::ColorAttachmentWrite,
             VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
}

void SkyBackgroundPass::releaseViewportResources() {
    destroyFramebuffers();
}

void SkyBackgroundPass::onViewportResize(
    const RenderResourceRegistry &resources) {
    createFramebuffers(resources);
}

void SkyBackgroundPass::execute(const RenderFrameContext &frame,
                         const RenderResourceRegistry &resources,
                         const RenderQueue &) {
    VKL_PROFILE_ZONE("Record Sky Background");
    VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd, "Sky Background");
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

    const bool drawAtmosphere =
        frame.view && frame.shaderVariant && frame.pipelineCache &&
        frame.atmosphereReady && frame.view->atmosphere.active &&
        frame.shaderVariant->supportsAtmosphere;
    const bool drawSkybox =
        !drawAtmosphere && frame.environmentReady && frame.view &&
        frame.view->settings.skyboxEnabled && frame.pipelineCache;
    if (drawAtmosphere || drawSkybox) {
        PipelineConfig config =
            PipelineConfigBuilder{}
                .shaders(vertexShaderPath_,
                         drawAtmosphere ? atmosphereFragmentShaderPath_
                                        : skyboxFragmentShaderPath_)
                .vertexLayout({})
                .rasterization(VK_CULL_MODE_NONE,
                               VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .depth(false, false)
                .msaa(resources.description(target).samples)
                .descriptorLayout(globalDescriptorSetLayout_)
                .descriptorLayout(emptyDescriptorSetLayout_)
                .descriptorLayout(lightingDescriptorSetLayout_)
                .build();
        if (drawAtmosphere)
            config.descriptorLayouts.push_back(
                atmosphereDescriptorSetLayout_);
        config.debugName = drawAtmosphere ? "Pipeline/SkyBackground/Atmosphere"
                                          : "Pipeline/SkyBackground/Skybox";
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
        if (drawAtmosphere) {
            vkCmdBindDescriptorSets(
                frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline.layout(), 3, 1,
                &frame.atmosphereDescriptorSet, 0, nullptr);
        }
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

void SkyBackgroundPass::createRenderPass(
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
                                        "Pass/SkyBackground/RenderPass");
}

void SkyBackgroundPass::createFramebuffers(
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
            "Pass/SkyBackground/Framebuffer/Frame" +
                std::to_string(frameIndex));
    }
}

void SkyBackgroundPass::destroyFramebuffers() {
    for (VkFramebuffer &framebuffer : framebuffers_) {
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_->logicalDevice(), framebuffer,
                                 nullptr);
            framebuffer = VK_NULL_HANDLE;
        }
    }
}

} // namespace vkr
