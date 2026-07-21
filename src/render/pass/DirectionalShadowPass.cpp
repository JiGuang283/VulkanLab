#include "DirectionalShadowPass.h"

#include "core/Device.h"
#include "core/Image.h"
#include "core/Pipeline.h"
#include "core/PipelineConfigBuilder.h"
#include "core/VulkanCheck.h"
#include "render/DirectionalShadow.h"
#include "render/FrameRenderTargets.h"
#include "render/GpuMaterialData.h"
#include "render/MaterialInstance.h"
#include "render/MaterialTemplate.h"
#include "render/Mesh.h"
#include "render/PipelineCache.h"
#include "render/PipelineKey.h"
#include "render/RenderFrame.h"
#include "render/RenderQueue.h"
#include "render/RenderSettings.h"

#include <glm/glm.hpp>
#include <utility>

namespace vkr {

DirectionalShadowPass::DirectionalShadowPass(
    Device &device, FrameRenderTargets &targets,
    VkDescriptorSetLayout globalDescriptorSetLayout,
    std::string shadowVertPath, std::string shadowMaskFragPath)
    : device_(&device), targets_(&targets),
      globalDescriptorSetLayout_(globalDescriptorSetLayout),
      shadowVertPath_(std::move(shadowVertPath)),
      shadowMaskFragPath_(std::move(shadowMaskFragPath)) {
    createRenderPass();
    createFramebuffers();
}

DirectionalShadowPass::~DirectionalShadowPass() {
    for (VkFramebuffer framebuffer : framebuffers_) {
        if (framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(device_->logicalDevice(), framebuffer,
                                 nullptr);
    }
    if (renderPass_ != VK_NULL_HANDLE)
        vkDestroyRenderPass(device_->logicalDevice(), renderPass_, nullptr);
}

void DirectionalShadowPass::execute(const RenderFrameContext &frame,
                                    const RenderQueue &queue) {
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = renderPass_;
    beginInfo.framebuffer = framebuffers_.at(frame.frameIndex);
    beginInfo.renderArea = {{0, 0},
                            {kDirectionalShadowMapSize,
                             kDirectionalShadowMapSize}};
    beginInfo.clearValueCount = 1;
    beginInfo.pClearValues = &clear;
    vkCmdBeginRenderPass(frame.cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(kDirectionalShadowMapSize);
    viewport.height = static_cast<float>(kDirectionalShadowMapSize);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0},
                           {kDirectionalShadowMapSize,
                            kDirectionalShadowMapSize}};
    vkCmdSetScissor(frame.cmd, 0, 1, &scissor);

    if (frame.shadow && frame.shadow->enabled)
        drawCasters(frame, queue);
    vkCmdEndRenderPass(frame.cmd);
}

void DirectionalShadowPass::drawCasters(const RenderFrameContext &frame,
                                        const RenderQueue &queue) {
    if (!frame.pipelineCache || !frame.settings)
        return;

    Pipeline *boundPipeline = nullptr;
    for (const RenderCommand &command : queue.opaque()) {
        if (!command.mesh || !command.material)
            continue;

        const MaterialParams &params = command.material->params();
        const bool alphaMasked = params.alphaMode == AlphaMode::Mask;
        const MaterialTemplate &materialTemplate =
            command.material->materialTemplate();
        const VkCullModeFlags cullMode =
            params.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        const VkShaderStageFlags pushStages =
            VK_SHADER_STAGE_VERTEX_BIT |
            (alphaMasked ? VK_SHADER_STAGE_FRAGMENT_BIT : 0);

        PipelineConfigBuilder builder;
        builder.shaders(shadowVertPath_,
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
            builder.descriptorLayout(materialTemplate.descriptorSetLayout());
        PipelineConfig config = builder.build();

        PipelineKey key{};
        key.materialTemplate = &materialTemplate;
        key.pass = PassId::DirectionalShadow;
        key.cullMode = cullMode;
        key.renderPass = renderPass_;
        key.samples = VK_SAMPLE_COUNT_1_BIT;
        key.alphaMasked = alphaMasked;

        Pipeline &pipeline = frame.pipelineCache->getOrCreate(key, config);
        if (boundPipeline != &pipeline) {
            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline.handle());
            vkCmdSetDepthBias(frame.cmd,
                              frame.settings->shadowConstantBias, 0.0f,
                              frame.settings->shadowSlopeBias);
            vkCmdBindDescriptorSets(frame.cmd,
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipeline.layout(), 0, 1,
                                    &frame.globalDescriptorSet, 0, nullptr);
            boundPipeline = &pipeline;
        }
        if (alphaMasked) {
            command.material->bindDescriptors(frame.cmd, pipeline.layout(),
                                               frame.frameIndex);
        }

        GpuPushBlock block{};
        block.model = command.world;
        block.baseColorFactor = params.baseColorFactor;
        block.roughnessAlpha.y = params.alphaCutoff;
        block.reserved.x = alphaMasked ? 1.0f : 0.0f;
        vkCmdPushConstants(frame.cmd, pipeline.layout(), pushStages, 0,
                           sizeof(block), &block);
        command.mesh->bind(frame.cmd);
        command.mesh->draw(frame.cmd);
    }
}

void DirectionalShadowPass::createRenderPass() {
    VkAttachmentDescription depth{};
    depth.format = targets_->shadowDepthFormat();
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
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
}

void DirectionalShadowPass::createFramebuffers() {
    for (uint32_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT;
         ++frameIndex) {
        const VkImageView attachment =
            targets_->frame(frameIndex).shadowDepth->imageView();
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = renderPass_;
        info.attachmentCount = 1;
        info.pAttachments = &attachment;
        info.width = kDirectionalShadowMapSize;
        info.height = kDirectionalShadowMapSize;
        info.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_->logicalDevice(), &info, nullptr,
                                     &framebuffers_[frameIndex]));
    }
}

} // namespace vkr
