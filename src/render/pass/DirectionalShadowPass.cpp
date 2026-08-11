#include "DirectionalShadowPass.h"

#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/Pipeline.h"
#include "core/PipelineConfigBuilder.h"
#include "core/VulkanCheck.h"
#include "render/DirectionalShadow.h"
#include "render/GpuMaterialData.h"
#include "render/MaterialInstance.h"
#include "render/MaterialTemplate.h"
#include "render/Mesh.h"
#include "render/PipelineCache.h"
#include "render/PipelineKey.h"
#include "render/RenderFrame.h"
#include "render/RenderResourceRegistry.h"
#include "render/RenderView.h"
#include "render/Visibility.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"

#include <glm/glm.hpp>
#include <string>
#include <utility>

namespace vkr {

DirectionalShadowPass::DirectionalShadowPass(
    Device &device, const RenderResourceRegistry &resources,
    RenderImageHandle shadowDepth,
    VkDescriptorSetLayout globalDescriptorSetLayout,
    std::string shadowVertPath, std::string shadowMaskFragPath)
    : device_(&device), shadowDepth_(shadowDepth),
      globalDescriptorSetLayout_(globalDescriptorSetLayout),
      shadowVertPath_(std::move(shadowVertPath)),
      shadowMaskFragPath_(std::move(shadowMaskFragPath)) {
    createRenderPass(resources);
    createFramebuffers(resources);
}

DirectionalShadowPass::~DirectionalShadowPass() {
    destroyFramebuffers();
    if (renderPass_ != VK_NULL_HANDLE)
        vkDestroyRenderPass(device_->logicalDevice(), renderPass_, nullptr);
}

std::vector<RenderImageUsage>
DirectionalShadowPass::resourceUsages() const {
    return {{shadowDepth_, RenderImageAccess::DepthAttachmentWrite,
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
}

void DirectionalShadowPass::execute(const RenderFrameContext &frame,
                                    const RenderResourceRegistry &,
                                    const VisibilityFrame &visibility) {
    VKL_PROFILE_ZONE("Record DirectionalShadow");
    VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd,
                         "DirectionalShadow");

    const bool shadowActive = frame.view &&
                              frame.view->shadow.csm.enabled;
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};

    VkViewport viewport{};
    viewport.width = static_cast<float>(kDirectionalShadowMapSize);
    viewport.height = static_cast<float>(kDirectionalShadowMapSize);
    viewport.maxDepth = 1.0f;
    const VkRect2D scissor{{0, 0},
                           {kDirectionalShadowMapSize,
                            kDirectionalShadowMapSize}};

    for (uint32_t cascade = 0; cascade < kCsmCascadeCount; ++cascade) {
        ScopedGpuLabel cascadeLabel(
            device_->debugUtils(), frame.cmd,
            "Cascade " + std::to_string(cascade));
        VkRenderPassBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = renderPass_;
        beginInfo.framebuffer = framebuffers_[cascade];
        beginInfo.renderArea = {{0, 0},
                                {kDirectionalShadowMapSize,
                                 kDirectionalShadowMapSize}};
        beginInfo.clearValueCount = 1;
        beginInfo.pClearValues = &clear;
        vkCmdBeginRenderPass(frame.cmd, &beginInfo,
                             VK_SUBPASS_CONTENTS_INLINE);

        vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
        vkCmdSetScissor(frame.cmd, 0, 1, &scissor);

        if (shadowActive)
            drawCasters(frame, visibility, cascade);

        vkCmdEndRenderPass(frame.cmd);
    }
}

void DirectionalShadowPass::drawCasters(
    const RenderFrameContext &frame,
    const VisibilityFrame &visibility,
    uint32_t cascadeIndex) {
    if (!frame.pipelineCache || !frame.view)
        return;

    Pipeline *boundPipeline = nullptr;
    for (RenderItemIndex itemIndex :
         visibility.directionalShadowCasters[cascadeIndex]) {
        const RenderItem &command = visibility.items.at(itemIndex);
        if (!command.mesh || !command.material)
            continue;

        const MaterialParams &params = command.material->params();
        const bool alphaMasked = params.alphaMode == AlphaMode::Mask;
        const MaterialTemplate &materialTemplate =
            command.material->materialTemplate();
        const VkCullModeFlags cullMode =
            params.doubleSided ? VK_CULL_MODE_NONE
                               : VK_CULL_MODE_BACK_BIT;
        const VkShaderStageFlags pushStages =
            VK_SHADER_STAGE_VERTEX_BIT |
            (alphaMasked ? VK_SHADER_STAGE_FRAGMENT_BIT : 0);

        PipelineConfigBuilder builder;
        builder
            .shaders(shadowVertPath_,
                     alphaMasked ? shadowMaskFragPath_ : std::string{})
            .defaultVertexLayout()
            .rasterization(cullMode,
                           materialTemplate.pipelineConfig().frontFace)
            .depth(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
            .depthBias(true)
            .colorAttachmentCount(0)
            .msaa(VK_SAMPLE_COUNT_1_BIT)
            .descriptorLayout(globalDescriptorSetLayout_)
            .pushConstant({pushStages, 0, sizeof(GpuPushBlock)});
        if (alphaMasked)
            builder.descriptorLayout(
                materialTemplate.descriptorSetLayout());
        PipelineConfig config = builder.build();
        config.debugName =
            "Pipeline/DirectionalShadow/" +
            std::string(alphaMasked ? "Mask" : "Opaque") + "/" +
            (cullMode == VK_CULL_MODE_NONE ? "CullNone" : "CullBack");

        Pipeline &pipeline = frame.pipelineCache->getOrCreate(
            renderPass_, std::move(config));
        if (boundPipeline != &pipeline) {
            vkCmdBindPipeline(frame.cmd,
                              VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline.handle());
            vkCmdSetDepthBias(
                frame.cmd, frame.view->settings.shadowConstantBias,
                0.0f, frame.view->settings.shadowSlopeBias);
            vkCmdBindDescriptorSets(
                frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline.layout(), 0, 1,
                &frame.globalDescriptorSet, 0, nullptr);
            boundPipeline = &pipeline;
        }
        if (alphaMasked) {
            command.material->bindDescriptors(
                frame.cmd, pipeline.layout(), frame.frameIndex);
        }

        GpuPushBlock block{};
        block.model = command.world;
        block.baseColorFactor = params.baseColorFactor;
        block.roughnessAlpha.y = params.alphaCutoff;
        block.reserved.x = alphaMasked ? 1.0f : 0.0f;
        block.reserved.y = static_cast<float>(cascadeIndex);
        vkCmdPushConstants(frame.cmd, pipeline.layout(), pushStages, 0,
                           sizeof(block), &block);
        command.mesh->bind(frame.cmd);
        command.mesh->draw(frame.cmd, itemIndex);
    }
}

void DirectionalShadowPass::createRenderPass(
    const RenderResourceRegistry &resources) {
    VkAttachmentDescription depth{};
    depth.format = resources.description(shadowDepth_).format;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{
        0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthRef;

    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &depth;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = static_cast<uint32_t>(dependencies.size());
    info.pDependencies = dependencies.data();
    VK_CHECK(vkCreateRenderPass(device_->logicalDevice(), &info, nullptr,
                                &renderPass_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_RENDER_PASS, renderPass_,
        "Pass/DirectionalShadow/RenderPass");
}

void DirectionalShadowPass::createFramebuffers(
    const RenderResourceRegistry &resources) {
    const VkFormat format =
        resources.description(shadowDepth_).format;
    const Image &image = resources.image(shadowDepth_, 0);
    for (uint32_t cascade = 0; cascade < kCsmCascadeCount; ++cascade) {
            // Create image view for a single cascade layer
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType =
                VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = image.handle();
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format;
            viewInfo.subresourceRange.aspectMask =
                VK_IMAGE_ASPECT_DEPTH_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = cascade;
            viewInfo.subresourceRange.layerCount = 1;
            VK_CHECK(vkCreateImageView(
                device_->logicalDevice(), &viewInfo, nullptr,
                &cascadeViews_[cascade]));
            device_->debugUtils().setObjectName(
                VK_OBJECT_TYPE_IMAGE_VIEW,
                cascadeViews_[cascade],
                "DirectionalShadow/Cascade" + std::to_string(cascade));

            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType =
                VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = renderPass_;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments =
                &cascadeViews_[cascade];
            fbInfo.width = kDirectionalShadowMapSize;
            fbInfo.height = kDirectionalShadowMapSize;
            fbInfo.layers = 1;
            VK_CHECK(vkCreateFramebuffer(
                device_->logicalDevice(), &fbInfo, nullptr,
                &framebuffers_[cascade]));
            device_->debugUtils().setObjectName(
                VK_OBJECT_TYPE_FRAMEBUFFER,
                framebuffers_[cascade],
                "Pass/DirectionalShadow/Cascade" +
                    std::to_string(cascade));
    }
}

void DirectionalShadowPass::destroyFramebuffers() {
    for (uint32_t cascade = 0; cascade < kCsmCascadeCount; ++cascade) {
            if (framebuffers_[cascade] != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(
                    device_->logicalDevice(),
                    framebuffers_[cascade], nullptr);
                framebuffers_[cascade] = VK_NULL_HANDLE;
            }
            if (cascadeViews_[cascade] != VK_NULL_HANDLE) {
                vkDestroyImageView(
                    device_->logicalDevice(),
                    cascadeViews_[cascade], nullptr);
                cascadeViews_[cascade] = VK_NULL_HANDLE;
            }
    }
}

} // namespace vkr
