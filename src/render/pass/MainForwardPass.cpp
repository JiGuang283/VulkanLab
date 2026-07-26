#include "MainForwardPass.h"

#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/Pipeline.h"
#include "core/PipelineConfigBuilder.h"
#include "core/VulkanCheck.h"
#include "render/GpuMaterialData.h"
#include "render/MaterialInstance.h"
#include "render/MaterialTemplate.h"
#include "render/Mesh.h"
#include "render/PipelineCache.h"
#include "render/PipelineKey.h"
#include "render/RenderFrame.h"
#include "render/RenderQueue.h"
#include "render/RenderResourceRegistry.h"
#include "render/ShaderVariant.h"

#include <array>
#include <glm/glm.hpp>
#include <stdexcept>
#include <utility>

namespace vkr {

namespace {

float alphaModeToShaderValue(AlphaMode mode) {
    switch (mode) {
    case AlphaMode::Mask:
        return 1.0f;
    case AlphaMode::Blend:
        return 2.0f;
    case AlphaMode::Opaque:
    default:
        return 0.0f;
    }
}

} // namespace

MainForwardPass::MainForwardPass(Device &device,
                                 const RenderResourceRegistry &resources,
                                 RendererResourceHandles resourceHandles,
                                 DescriptorAllocator &descriptorAllocator)
    : device_(&device), resourceHandles_(resourceHandles),
      descriptorAllocator_(&descriptorAllocator) {
    createRenderPass(resources);
    createFramebuffers(resources);
    createShadowDescriptors(resources);
}

MainForwardPass::~MainForwardPass() {
    destroyFramebuffers();
    for (VkDescriptorSet set : shadowDescriptorSets_)
        descriptorAllocator_->free(set);
    if (shadowDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     shadowDescriptorSetLayout_, nullptr);
    }
    if (renderPass_ != VK_NULL_HANDLE)
        vkDestroyRenderPass(device_->logicalDevice(), renderPass_, nullptr);
}

void MainForwardPass::releaseSwapChainResources() {
    destroyFramebuffers();
}

std::vector<RenderImageUsage> MainForwardPass::resourceUsages() const {
    std::vector<RenderImageUsage> usages = {
        {resourceHandles_.directionalShadowDepth,
         RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
    if (resourceHandles_.hdrMsaaColor.valid()) {
        usages.push_back({resourceHandles_.hdrMsaaColor,
                          RenderImageAccess::ColorAttachmentWrite,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    }
    usages.push_back({resourceHandles_.mainDepth,
                      RenderImageAccess::DepthAttachmentWrite,
                      VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL});
    usages.push_back({resourceHandles_.hdrColor,
                      RenderImageAccess::ColorAttachmentWrite,
                      VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    return usages;
}

void MainForwardPass::onResize(
    const SwapChain &, const RenderResourceRegistry &resources) {
    createFramebuffers(resources);
}

void MainForwardPass::execute(const RenderFrameContext &frame,
                              const RenderResourceRegistry &resources,
                              const RenderQueue &queue) {
    begin(frame.cmd, frame.frameIndex, resources);
    drawQueue(frame, resources, queue);
    vkCmdEndRenderPass(frame.cmd);
}

void MainForwardPass::begin(VkCommandBuffer cmd, uint32_t frameIndex,
                            const RenderResourceRegistry &resources) {
    const VkExtent2D extent = resources.extent(resourceHandles_.hdrColor);
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = renderPass_;
    beginInfo.framebuffer = framebuffers_.at(frameIndex);
    beginInfo.renderArea.offset = {0, 0};
    beginInfo.renderArea.extent = extent;
    beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    beginInfo.pClearValues = clearValues.data();
    vkCmdBeginRenderPass(cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void MainForwardPass::drawQueue(const RenderFrameContext &frame,
                                const RenderResourceRegistry &resources,
                                const RenderQueue &queue) {
    if (!frame.pipelineCache || !frame.shaderVariant)
        return;

    const auto drawCommands = [&](const std::vector<RenderCommand> &commands,
                                  RenderQueueType queueType) {
        const bool transparent = queueType == RenderQueueType::Transparent;
        ScopedGpuLabel queueLabel(device_->debugUtils(), frame.cmd,
                                  transparent ? "Transparent" : "Opaque");
        Pipeline *boundPipeline = nullptr;

        for (const auto &command : commands) {
            if (!command.mesh || !command.material)
                continue;

            const auto &materialTemplate = command.material->materialTemplate();
            const auto &p = command.material->params();
            const VkCullModeFlags cullMode =
                p.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
            auto pipelineConfig = materialTemplate.pipelineConfig();
            if (pipelineConfig.colorBlendAttachments.empty())
                pipelineConfig.colorBlendAttachments.resize(1);
            pipelineConfig.colorBlendAttachments[0].blendEnable = transparent;
            pipelineConfig.depthTest = true;
            pipelineConfig.depthWrite = !transparent;
            pipelineConfig.cullMode = cullMode;
            pipelineConfig.msaaSamples =
                resources.description(resourceHandles_.mainDepth).samples;
            pipelineConfig.vertShaderPath =
                frame.shaderVariant->vertSpvPath;
            pipelineConfig.fragShaderPath =
                frame.shaderVariant->fragSpvPath;
            pipelineConfig.debugName =
                "Pipeline/MainForward/" + frame.shaderVariant->id + "/" +
                (transparent ? "Transparent" : "Opaque") + "/" +
                (cullMode == VK_CULL_MODE_NONE ? "CullNone" : "CullBack");
            pipelineConfig.descriptorLayouts.insert(
                pipelineConfig.descriptorLayouts.begin(),
                frame.globalDescriptorSetLayout);
            pipelineConfig.descriptorLayouts.push_back(
                shadowDescriptorSetLayout_);

            Pipeline &pipeline = frame.pipelineCache->getOrCreate(
                renderPass_, std::move(pipelineConfig));
            if (boundPipeline != &pipeline) {
                vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  pipeline.handle());
                vkCmdBindDescriptorSets(
                    frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline.layout(), 0, 1, &frame.globalDescriptorSet, 0,
                    nullptr);
                const VkDescriptorSet shadowSet =
                    shadowDescriptorSets_.at(frame.frameIndex);
                vkCmdBindDescriptorSets(
                    frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline.layout(), 2, 1, &shadowSet, 0, nullptr);
                boundPipeline = &pipeline;
            }

            command.material->bindDescriptors(frame.cmd, pipeline.layout(),
                                               frame.frameIndex);

            GpuPushBlock block{};
            block.model = command.world;
            block.baseColorFactor = p.baseColorFactor;
            block.emissiveMetallic =
                glm::vec4(p.emissiveFactor * p.emissiveStrength,
                          p.metallicFactor);
            block.roughnessAlpha =
                glm::vec4(p.roughnessFactor, p.alphaCutoff,
                          glm::clamp(p.occlusionStrength, 0.0f, 1.0f),
                          p.occlusionTexCoord == 1 ? 1.0f : 0.0f);
            block.reserved =
                glm::vec4(alphaModeToShaderValue(p.alphaMode),
                          glm::clamp(p.transmissionFactor, 0.0f, 1.0f),
                          glm::max(p.normalScale, 0.0f), 0.0f);

            vkCmdPushConstants(frame.cmd, pipeline.layout(),
                               VK_SHADER_STAGE_VERTEX_BIT |
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(block), &block);
            command.mesh->bind(frame.cmd);
            command.mesh->draw(frame.cmd);
        }
    };

    drawCommands(queue.opaque(), RenderQueueType::Opaque);
    drawCommands(queue.transparent(), RenderQueueType::Transparent);
}

void MainForwardPass::createRenderPass(
    const RenderResourceRegistry &resources) {
    const RenderImageDesc &hdrDesc =
        resources.description(resourceHandles_.hdrColor);
    const RenderImageDesc &depthDesc =
        resources.description(resourceHandles_.mainDepth);
    const bool multisampled = resourceHandles_.hdrMsaaColor.valid();

    VkAttachmentDescription color{};
    color.format = hdrDesc.format;
    color.samples = multisampled ? depthDesc.samples
                                 : VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = multisampled ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                 : VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = multisampled
                            ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription depth{};
    depth.format = depthDesc.format;
    depth.samples = depthDesc.samples;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription resolve{};
    resolve.format = hdrDesc.format;
    resolve.samples = VK_SAMPLE_COUNT_1_BIT;
    resolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    resolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    resolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    resolve.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{0,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1,
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolveRef{2,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;
    subpass.pResolveAttachments = multisampled ? &resolveRef : nullptr;

    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    const std::array<VkAttachmentDescription, 3> allAttachments = {
        color, depth, resolve};
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = multisampled ? 3u : 2u;
    info.pAttachments = allAttachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = static_cast<uint32_t>(dependencies.size());
    info.pDependencies = dependencies.data();
    VK_CHECK(vkCreateRenderPass(device_->logicalDevice(), &info, nullptr,
                                &renderPass_));
    device_->debugUtils().setObjectName(VK_OBJECT_TYPE_RENDER_PASS,
                                        renderPass_,
                                        "Pass/MainForward/RenderPass");
}

void MainForwardPass::createFramebuffers(
    const RenderResourceRegistry &resources) {
    const bool multisampled = resourceHandles_.hdrMsaaColor.valid();
    const VkExtent2D extent = resources.extent(resourceHandles_.hdrColor);
    for (uint32_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT;
         ++frameIndex) {
        std::array<VkImageView, 3> attachments{};
        attachments[0] =
            multisampled
                ? resources
                      .image(resourceHandles_.hdrMsaaColor, frameIndex)
                      .imageView()
                : resources.image(resourceHandles_.hdrColor, frameIndex)
                      .imageView();
        attachments[1] =
            resources.image(resourceHandles_.mainDepth, frameIndex)
                .imageView();
        attachments[2] =
            resources.image(resourceHandles_.hdrColor, frameIndex)
                .imageView();

        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = renderPass_;
        info.attachmentCount = multisampled ? 3u : 2u;
        info.pAttachments = attachments.data();
        info.width = extent.width;
        info.height = extent.height;
        info.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_->logicalDevice(), &info, nullptr,
                                     &framebuffers_[frameIndex]));
        device_->debugUtils().setObjectName(
            VK_OBJECT_TYPE_FRAMEBUFFER, framebuffers_[frameIndex],
            "Pass/MainForward/Framebuffer/Frame" +
                std::to_string(frameIndex));
    }
}

void MainForwardPass::createShadowDescriptors(
    const RenderResourceRegistry &resources) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &layoutInfo,
                                         nullptr,
                                         &shadowDescriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, shadowDescriptorSetLayout_,
        "Pass/MainForward/ShadowDescriptorSetLayout");

    for (uint32_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT;
         ++frameIndex) {
        shadowDescriptorSets_[frameIndex] = descriptorAllocator_->allocate(
            shadowDescriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}},
            "Pass/MainForward/ShadowDescriptorSet/Frame" +
                std::to_string(frameIndex));
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler =
            resources.sampler(resourceHandles_.shadowSampler);
        imageInfo.imageView =
            resources
                .image(resourceHandles_.directionalShadowDepth, frameIndex)
                .imageView();
        imageInfo.imageLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = shadowDescriptorSets_[frameIndex];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device_->logicalDevice(), 1, &write, 0,
                               nullptr);
    }
}

void MainForwardPass::destroyFramebuffers() {
    for (VkFramebuffer &framebuffer : framebuffers_) {
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_->logicalDevice(), framebuffer,
                                 nullptr);
            framebuffer = VK_NULL_HANDLE;
        }
    }
}

} // namespace vkr
