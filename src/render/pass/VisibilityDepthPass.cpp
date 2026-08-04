#include "render/pass/VisibilityDepthPass.h"

#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/Pipeline.h"
#include "core/PipelineConfigBuilder.h"
#include "core/VulkanCheck.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"
#include "render/GpuMaterialData.h"
#include "render/MaterialInstance.h"
#include "render/MaterialTemplate.h"
#include "render/Mesh.h"
#include "render/PipelineCache.h"
#include "render/RenderFrame.h"
#include "render/RenderQueue.h"
#include "render/RenderResourceRegistry.h"
#include "render/RenderView.h"
#include "render/Visibility.h"

#include <array>
#include <stdexcept>
#include <utility>

namespace vkr {

VisibilityDepthPass::VisibilityDepthPass(
    Device &device, const RenderResourceRegistry &resources,
    RenderImageHandle visibilityDepth,
    VkDescriptorSetLayout globalDescriptorSetLayout,
    std::string depthVertPath, std::string depthMaskFragPath)
    : device_(&device), visibilityDepth_(visibilityDepth),
      globalDescriptorSetLayout_(globalDescriptorSetLayout),
      depthVertPath_(std::move(depthVertPath)),
      depthMaskFragPath_(std::move(depthMaskFragPath)) {
    if (!visibilityDepth_.valid())
        throw std::invalid_argument("VisibilityDepthPass requires depth image");
    createRenderPass(resources);
    createFramebuffers(resources);
}

VisibilityDepthPass::~VisibilityDepthPass() {
    destroyFramebuffers();
    if (renderPass_ != VK_NULL_HANDLE)
        vkDestroyRenderPass(device_->logicalDevice(), renderPass_, nullptr);
}

std::vector<RenderImageUsage> VisibilityDepthPass::resourceUsages() const {
    return {{visibilityDepth_, RenderImageAccess::DepthAttachmentWrite,
             VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
}

void VisibilityDepthPass::releaseViewportResources() {
    destroyFramebuffers();
}

void VisibilityDepthPass::onViewportResize(
    const RenderResourceRegistry &resources) {
    createFramebuffers(resources);
}

void VisibilityDepthPass::execute(const RenderFrameContext &frame,
                                  const RenderResourceRegistry &resources,
                                  const VisibilityFrame &visibility) {
    if (!frame.view || !frame.view->settings.culling.occlusionEnabled ||
        visibility.depthPrepass.opaque().empty()) {
        return;
    }
    VKL_PROFILE_ZONE("Record VisibilityDepth");
    VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd, "VisibilityDepth");
    const VkExtent2D extent = resources.extent(visibilityDepth_);
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = renderPass_;
    begin.framebuffer = framebuffers_.at(frame.frameIndex);
    begin.renderArea = {{0, 0}, extent};
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    vkCmdBeginRenderPass(frame.cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(frame.cmd, 0, 1, &scissor);
    draw(frame, visibility.depthPrepass);
    vkCmdEndRenderPass(frame.cmd);
}

void VisibilityDepthPass::draw(const RenderFrameContext &frame,
                               const RenderQueue &queue) {
    if (!frame.pipelineCache)
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
            (alphaMasked ? VK_SHADER_STAGE_FRAGMENT_BIT : 0u);

        PipelineConfigBuilder builder;
        builder.shaders(depthVertPath_,
                        alphaMasked ? depthMaskFragPath_ : std::string{})
            .defaultVertexLayout()
            .rasterization(cullMode,
                           materialTemplate.pipelineConfig().frontFace)
            .depth(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
            .colorAttachmentCount(0)
            .msaa(VK_SAMPLE_COUNT_1_BIT)
            .descriptorLayout(globalDescriptorSetLayout_)
            .pushConstant({pushStages, 0, sizeof(GpuPushBlock)});
        if (alphaMasked)
            builder.descriptorLayout(materialTemplate.descriptorSetLayout());
        PipelineConfig config = builder.build();
        config.debugName =
            "Pipeline/VisibilityDepth/" +
            std::string(alphaMasked ? "Mask" : "Opaque") + "/" +
            (cullMode == VK_CULL_MODE_NONE ? "CullNone" : "CullBack");
        Pipeline &pipeline = frame.pipelineCache->getOrCreate(
            renderPass_, std::move(config));
        if (boundPipeline != &pipeline) {
            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline.handle());
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
        vkCmdPushConstants(frame.cmd, pipeline.layout(), pushStages, 0,
                           sizeof(block), &block);
        command.mesh->bind(frame.cmd);
        command.mesh->draw(frame.cmd);
    }
}

void VisibilityDepthPass::createRenderPass(
    const RenderResourceRegistry &resources) {
    VkAttachmentDescription depth{};
    depth.format = resources.description(visibilityDepth_).format;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    const VkAttachmentReference depthRef{
        0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthRef;
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].dstAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    dependencies[1].srcAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
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
    device_->debugUtils().setObjectName(VK_OBJECT_TYPE_RENDER_PASS,
                                        renderPass_,
                                        "RenderPass/VisibilityDepth");
}

void VisibilityDepthPass::createFramebuffers(
    const RenderResourceRegistry &resources) {
    const VkExtent2D extent = resources.extent(visibilityDepth_);
    for (uint32_t frame = 0; frame < framebuffers_.size(); ++frame) {
        const VkImageView attachment =
            resources.image(visibilityDepth_, frame).imageView();
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = renderPass_;
        info.attachmentCount = 1;
        info.pAttachments = &attachment;
        info.width = extent.width;
        info.height = extent.height;
        info.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_->logicalDevice(), &info,
                                     nullptr, &framebuffers_[frame]));
        device_->debugUtils().setObjectName(
            VK_OBJECT_TYPE_FRAMEBUFFER, framebuffers_[frame],
            "Framebuffer/VisibilityDepth/Frame" + std::to_string(frame));
    }
}

void VisibilityDepthPass::destroyFramebuffers() {
    for (VkFramebuffer &framebuffer : framebuffers_) {
        if (framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(device_->logicalDevice(), framebuffer,
                                 nullptr);
        framebuffer = VK_NULL_HANDLE;
    }
}

} // namespace vkr
