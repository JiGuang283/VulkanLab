#include "MainForwardPass.h"

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
#include "render/RenderResourceRegistry.h"
#include "render/ShaderVariant.h"
#include "render/Visibility.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"

#include <array>
#include <glm/glm.hpp>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

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
                                 ForwardPhase phase,
                                 VkDescriptorSetLayout
                                     lightingDescriptorSetLayout,
                                 VkDescriptorSetLayout
                                     atmosphereDescriptorSetLayout,
                                 VkDescriptorSetLayout
                                     ddgiDescriptorSetLayout)
    : device_(&device), resourceHandles_(resourceHandles), phase_(phase),
      lightingDescriptorSetLayout_(lightingDescriptorSetLayout),
      atmosphereDescriptorSetLayout_(atmosphereDescriptorSetLayout),
      ddgiDescriptorSetLayout_(ddgiDescriptorSetLayout) {
    createRenderPass(resources);
    createFramebuffers(resources);
}

MainForwardPass::~MainForwardPass() {
    destroyFramebuffers();
    if (renderPass_ != VK_NULL_HANDLE)
        vkDestroyRenderPass(device_->logicalDevice(), renderPass_, nullptr);
}

void MainForwardPass::releaseViewportResources() {
    destroyFramebuffers();
}

std::vector<RenderImageUsage> MainForwardPass::resourceUsages() const {
    std::vector<RenderImageUsage> usages = {
        {resourceHandles_.directionalShadowDepth,
         RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
    usages.push_back({resourceHandles_.atmosphereTransmittance,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    usages.push_back({resourceHandles_.atmosphereAerialPerspective,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    usages.push_back({resourceHandles_.ddgiIrradiance,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    usages.push_back({resourceHandles_.ddgiDistance,
                      RenderImageAccess::SampledRead,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    if (resourceHandles_.ssaoFiltered.valid()) {
        usages.push_back({resourceHandles_.ssaoFiltered,
                          RenderImageAccess::SampledRead,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
    if (phase_ == ForwardPhase::Transparent) {
        usages.push_back({resourceHandles_.surfaceDepth,
                          RenderImageAccess::DepthAttachmentRead,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL});
        usages.push_back({resourceHandles_.compositedHdrColor,
                          RenderImageAccess::ColorAttachmentReadWrite,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        return usages;
    }
    if (resourceHandles_.hdrMsaaColor.valid()) {
        usages.push_back({resourceHandles_.hdrMsaaColor,
                          RenderImageAccess::ColorAttachmentReadWrite,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    }
    usages.push_back({resourceHandles_.mainDepth,
                      RenderImageAccess::DepthAttachmentWrite,
                      VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL});
    usages.push_back(
        {resourceHandles_.hdrColor,
         resourceHandles_.hdrMsaaColor.valid()
             ? RenderImageAccess::ColorAttachmentWrite
             : RenderImageAccess::ColorAttachmentReadWrite,
         resourceHandles_.hdrMsaaColor.valid()
             ? VK_IMAGE_LAYOUT_UNDEFINED
             : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    if (resourceHandles_.baselineSpecularMsaa.valid()) {
        usages.push_back({resourceHandles_.baselineSpecularMsaa,
                          RenderImageAccess::ColorAttachmentWrite,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    }
    usages.push_back({resourceHandles_.baselineSpecular,
                      RenderImageAccess::ColorAttachmentWrite,
                      VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    if (resourceHandles_.baselineDiffuseMsaa.valid()) {
        usages.push_back({resourceHandles_.baselineDiffuseMsaa,
                          RenderImageAccess::ColorAttachmentWrite,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    }
    if (resourceHandles_.baselineDiffuse.valid()) {
        usages.push_back({resourceHandles_.baselineDiffuse,
                          RenderImageAccess::ColorAttachmentWrite,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
    return usages;
}

void MainForwardPass::onViewportResize(
    const RenderResourceRegistry &resources) {
    createFramebuffers(resources);
}

void MainForwardPass::execute(const RenderFrameContext &frame,
                              const RenderResourceRegistry &resources,
                              const VisibilityFrame &visibility) {
    VKL_PROFILE_ZONE("Record Forward Phase");
    ScopedGpuLabel phaseLabel(
        device_->debugUtils(), frame.cmd,
        phase_ == ForwardPhase::Opaque ? "MainForward/Opaque"
                                       : "MainForward/Transparent");
    begin(frame.cmd, frame.frameIndex, resources);
    drawQueue(frame, resources, visibility);
    vkCmdEndRenderPass(frame.cmd);
}

void MainForwardPass::begin(VkCommandBuffer cmd, uint32_t frameIndex,
                            const RenderResourceRegistry &resources) {
    const RenderImageHandle colorHandle =
        phase_ == ForwardPhase::Opaque ? resourceHandles_.hdrColor
                                       : resourceHandles_.compositedHdrColor;
    const VkExtent2D extent = resources.extent(colorHandle);
    std::array<VkClearValue, 4> clearValues{};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[1].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    const bool hasBaselineDiffuse =
        resourceHandles_.baselineDiffuse.valid();
    if (hasBaselineDiffuse)
        clearValues[2].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[hasBaselineDiffuse ? 3u : 2u].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = renderPass_;
    beginInfo.framebuffer = framebuffers_.at(frameIndex);
    beginInfo.renderArea.offset = {0, 0};
    beginInfo.renderArea.extent = extent;
    beginInfo.clearValueCount = phase_ == ForwardPhase::Opaque
                                   ? (hasBaselineDiffuse ? 4u : 3u)
                                   : 0u;
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
                                const VisibilityFrame &visibility) {
    if (!frame.pipelineCache || !frame.shaderVariant)
        return;

    const bool transparent = phase_ == ForwardPhase::Transparent;
    const auto &indices = transparent ? visibility.cameraTransparent
                                      : visibility.cameraOpaque;
    const auto drawCommands = [&]() {
        ScopedGpuLabel queueLabel(device_->debugUtils(), frame.cmd,
                                  transparent ? "Transparent" : "Opaque");
        Pipeline *boundPipeline = nullptr;

        for (uint32_t drawIndex = 0; drawIndex < indices.size(); ++drawIndex) {
            const RenderItemIndex itemIndex = indices[drawIndex];
            const RenderItem &command = visibility.items.at(itemIndex);
            if (!command.mesh || !command.material)
                continue;

            const auto &materialTemplate = command.material->materialTemplate();
            const auto &p = command.material->params();
            const VkCullModeFlags cullMode =
                p.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
            auto pipelineConfig = materialTemplate.pipelineConfig();
            const uint32_t opaqueAttachmentCount =
                resourceHandles_.baselineDiffuse.valid() ? 3u : 2u;
            pipelineConfig.colorBlendAttachments.resize(
                transparent ? 1u : opaqueAttachmentCount);
            pipelineConfig.colorBlendAttachments[0].blendEnable = transparent;
            if (!transparent) {
                for (uint32_t attachment = 1;
                     attachment < opaqueAttachmentCount; ++attachment) {
                    pipelineConfig.colorBlendAttachments[attachment]
                        .blendEnable = false;
                }
            }
            pipelineConfig.depthTest = true;
            pipelineConfig.depthWrite = !transparent;
            pipelineConfig.cullMode = cullMode;
            pipelineConfig.msaaSamples = transparent
                ? VK_SAMPLE_COUNT_1_BIT
                : resources.description(resourceHandles_.mainDepth).samples;
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
                lightingDescriptorSetLayout_);
            if (frame.shaderVariant->supportsAtmosphere ||
                frame.shaderVariant->supportsScreenSpace) {
                pipelineConfig.descriptorLayouts.push_back(
                    atmosphereDescriptorSetLayout_);
            }
            if (frame.shaderVariant->supportsScreenSpace) {
                pipelineConfig.descriptorLayouts.push_back(
                    frame.screenSpaceDescriptorSetLayout);
            }
            if (frame.shaderVariant->supportsDdgi) {
                pipelineConfig.descriptorLayouts.push_back(
                    ddgiDescriptorSetLayout_);
            }

            Pipeline &pipeline = frame.pipelineCache->getOrCreate(
                renderPass_, std::move(pipelineConfig));
            if (boundPipeline != &pipeline) {
                vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  pipeline.handle());
                vkCmdBindDescriptorSets(
                    frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline.layout(), 0, 1, &frame.globalDescriptorSet, 0,
                    nullptr);
                vkCmdBindDescriptorSets(
                    frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline.layout(), 2, 1,
                    &frame.lightingDescriptorSet, 0, nullptr);
                if (frame.shaderVariant->supportsAtmosphere) {
                    vkCmdBindDescriptorSets(
                        frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pipeline.layout(), 3, 1,
                        &frame.atmosphereDescriptorSet, 0, nullptr);
                }
                if (frame.shaderVariant->supportsScreenSpace) {
                    vkCmdBindDescriptorSets(
                        frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pipeline.layout(), 4, 1,
                        &frame.screenSpaceDescriptorSet, 0, nullptr);
                }
                if (frame.shaderVariant->supportsDdgi) {
                    vkCmdBindDescriptorSets(
                        frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pipeline.layout(), 5, 1,
                        &frame.ddgiDescriptorSet, 0, nullptr);
                }
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
            const GpuVisibilityDrawStream *stream =
                frame.visibilityDrawStream;
            if (!transparent && stream && stream->active &&
                stream->indirectBuffer != VK_NULL_HANDLE &&
                stream->frameIndex == frame.frameIndex &&
                stream->visibilityGeneration == visibility.generation &&
                drawIndex < stream->candidateCount) {
                command.mesh->drawIndirect(
                    frame.cmd, stream->indirectBuffer,
                    static_cast<VkDeviceSize>(drawIndex) *
                        sizeof(VkDrawIndexedIndirectCommand));
            } else {
                command.mesh->draw(frame.cmd, itemIndex);
            }
        }
    };
    drawCommands();
}

void MainForwardPass::createRenderPass(
    const RenderResourceRegistry &resources) {
    if (phase_ == ForwardPhase::Transparent) {
        const auto &colorDesc =
            resources.description(resourceHandles_.compositedHdrColor);
        const auto &depthDesc =
            resources.description(resourceHandles_.surfaceDepth);

        VkAttachmentDescription color{};
        color.format = colorDesc.format;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentDescription depth{};
        depth.format = depthDesc.format;
        depth.samples = VK_SAMPLE_COUNT_1_BIT;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        const VkAttachmentReference colorRef{
            0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const VkAttachmentReference depthRef{
            1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;

        std::array<VkSubpassDependency, 2> dependencies{};
        dependencies[0] = {VK_SUBPASS_EXTERNAL, 0,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                           VK_ACCESS_SHADER_WRITE_BIT |
                               VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                           VK_DEPENDENCY_BY_REGION_BIT};
        dependencies[1] = {0, VK_SUBPASS_EXTERNAL,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT,
                           VK_DEPENDENCY_BY_REGION_BIT};

        const std::array<VkAttachmentDescription, 2> attachments = {
            color, depth};
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = static_cast<uint32_t>(dependencies.size());
        info.pDependencies = dependencies.data();
        VK_CHECK(vkCreateRenderPass(device_->logicalDevice(), &info, nullptr,
                                    &renderPass_));
        device_->debugUtils().setObjectName(
            VK_OBJECT_TYPE_RENDER_PASS, renderPass_,
            "Pass/MainForwardTransparent/RenderPass");
        return;
    }

    const RenderImageDesc &hdrDesc =
        resources.description(resourceHandles_.hdrColor);
    const RenderImageDesc &depthDesc =
        resources.description(resourceHandles_.mainDepth);
    const bool multisampled = resourceHandles_.hdrMsaaColor.valid();
    const bool hasBaselineDiffuse =
        resourceHandles_.baselineDiffuse.valid();

    VkAttachmentDescription color{};
    color.format = hdrDesc.format;
    color.samples = multisampled ? depthDesc.samples
                                 : VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = multisampled ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                 : VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout = multisampled
                            ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription baselineSpecular = color;
    baselineSpecular.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    baselineSpecular.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkAttachmentDescription baselineDiffuse = baselineSpecular;

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

    VkAttachmentDescription baselineResolve = resolve;

    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> colorRefs;
    std::vector<VkAttachmentReference> resolveRefs;
    const auto addColor = [&](const VkAttachmentDescription &attachment) {
        const uint32_t index = static_cast<uint32_t>(attachments.size());
        attachments.push_back(attachment);
        colorRefs.push_back(
            {index, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    };
    addColor(color);
    addColor(baselineSpecular);
    if (hasBaselineDiffuse)
        addColor(baselineDiffuse);
    const uint32_t depthIndex = static_cast<uint32_t>(attachments.size());
    attachments.push_back(depth);
    const VkAttachmentReference depthRef{
        depthIndex, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    if (multisampled) {
        const uint32_t resolveBase =
            static_cast<uint32_t>(attachments.size());
        attachments.push_back(resolve);
        attachments.push_back(baselineResolve);
        if (hasBaselineDiffuse)
            attachments.push_back(baselineResolve);
        for (uint32_t index = 0;
             index < static_cast<uint32_t>(colorRefs.size()); ++index) {
            resolveRefs.push_back(
                {resolveBase + index,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
        }
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount =
        static_cast<uint32_t>(colorRefs.size());
    subpass.pColorAttachments = colorRefs.data();
    subpass.pDepthStencilAttachment = &depthRef;
    subpass.pResolveAttachments = multisampled ? resolveRefs.data() : nullptr;

    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstAccessMask =
                                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask =
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
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
    if (phase_ == ForwardPhase::Transparent) {
        const VkExtent2D extent =
            resources.extent(resourceHandles_.compositedHdrColor);
        for (uint32_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT;
             ++frameIndex) {
            const std::array<VkImageView, 2> attachments = {
                resources.image(resourceHandles_.compositedHdrColor,
                                frameIndex).imageView(),
                resources.image(resourceHandles_.surfaceDepth,
                                frameIndex).imageView()};
            VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            info.renderPass = renderPass_;
            info.attachmentCount = static_cast<uint32_t>(attachments.size());
            info.pAttachments = attachments.data();
            info.width = extent.width;
            info.height = extent.height;
            info.layers = 1;
            VK_CHECK(vkCreateFramebuffer(device_->logicalDevice(), &info,
                                         nullptr,
                                         &framebuffers_[frameIndex]));
            device_->debugUtils().setObjectName(
                VK_OBJECT_TYPE_FRAMEBUFFER, framebuffers_[frameIndex],
                "Pass/MainForwardTransparent/Framebuffer/Frame" +
                    std::to_string(frameIndex));
        }
        return;
    }

    const bool multisampled = resourceHandles_.hdrMsaaColor.valid();
    const bool hasBaselineDiffuse =
        resourceHandles_.baselineDiffuse.valid();
    const VkExtent2D extent = resources.extent(resourceHandles_.hdrColor);
    for (uint32_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT;
         ++frameIndex) {
        std::vector<VkImageView> attachments;
        attachments.reserve(multisampled
                                ? (hasBaselineDiffuse ? 7u : 5u)
                                : (hasBaselineDiffuse ? 4u : 3u));
        attachments.push_back(
            multisampled
                ? resources
                      .image(resourceHandles_.hdrMsaaColor, frameIndex)
                      .imageView()
                : resources.image(resourceHandles_.hdrColor, frameIndex)
                      .imageView());
        attachments.push_back(
            multisampled
                ? resources.image(resourceHandles_.baselineSpecularMsaa,
                                  frameIndex).imageView()
                : resources.image(resourceHandles_.baselineSpecular,
                                  frameIndex).imageView());
        if (hasBaselineDiffuse) {
            attachments.push_back(
                multisampled
                    ? resources
                          .image(resourceHandles_.baselineDiffuseMsaa,
                                 frameIndex)
                          .imageView()
                    : resources
                          .image(resourceHandles_.baselineDiffuse,
                                 frameIndex)
                          .imageView());
        }
        attachments.push_back(
            resources.image(resourceHandles_.mainDepth, frameIndex)
                .imageView());
        if (multisampled) {
            attachments.push_back(
                resources.image(resourceHandles_.hdrColor, frameIndex)
                    .imageView());
            attachments.push_back(
                resources.image(resourceHandles_.baselineSpecular,
                                frameIndex)
                    .imageView());
            if (hasBaselineDiffuse) {
                attachments.push_back(
                    resources.image(resourceHandles_.baselineDiffuse,
                                    frameIndex)
                        .imageView());
            }
        }

        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = renderPass_;
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
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
