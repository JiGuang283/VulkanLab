#include "MainForwardPass.h"

#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "render/pipeline/Pipeline.h"
#include "render/pipeline/PipelineConfigBuilder.h"
#include "core/VulkanCheck.h"
#include "render/material/GpuMaterialData.h"
#include "render/material/MaterialInstance.h"
#include "render/material/MaterialSystem.h"
#include "render/material/MaterialTemplate.h"
#include "render/geometry/Mesh.h"
#include "render/pipeline/PipelineCache.h"
#include "render/pipeline/PipelineKey.h"
#include "render/frame/RenderFrame.h"
#include "render/graph/RenderGraph.h"
#include "render/graph/RenderResourcePool.h"
#include "render/shader/ShaderTypes.h"
#include "render/shader/ShaderRegistry.h"
#include "render/features/shadows_visibility/Visibility.h"
#include "render/features/lighting/ClusteredLighting.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"

#include <glm/glm.hpp>
#include <algorithm>
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

RenderImageHandle transparentColorHandle(
    const FrameRenderFeatures &features,
    const RendererResourceHandles &resources) {
    if (features.renderPath.active == RenderPathMode::Deferred)
        return features.postLightingHdr(resources);
    if (features.lightingCompositeRequired)
        return resources.compositedHdrColor;
    if (features.surfaceDataRequired || !resources.hdrMsaaColor.valid())
        return resources.hdrColor;
    return resources.hdrMsaaColor;
}

RenderImageHandle transparentResolveHandle(
    const FrameRenderFeatures &features,
    const RendererResourceHandles &resources) {
    const bool resolvesForwardMsaa =
        features.renderPath.active == RenderPathMode::Forward &&
        !features.lightingCompositeRequired &&
        !features.surfaceDataRequired && resources.hdrMsaaColor.valid();
    return resolvesForwardMsaa ? resources.hdrColor : RenderImageHandle{};
}

RenderImageHandle transparentDepthHandle(
    const FrameRenderFeatures &features,
    const RendererResourceHandles &resources) {
    if (features.renderPath.active == RenderPathMode::Deferred)
        return features.opaqueProducts.geometryDepth;
    return features.surfaceDataRequired ? resources.surfaceDepth
                                        : resources.mainDepth;
}

} // namespace

MainForwardPass::MainForwardPass(Device &device,
                                 const RenderResourcePool &,
                                 RendererResourceHandles resourceHandles,
                                 ForwardPhase phase,
                                 VkDescriptorSetLayout
                                     lightingDescriptorSetLayout,
                                 VkDescriptorSetLayout
                                     atmosphereDescriptorSetLayout,
                                 VkDescriptorSetLayout
                                     ddgiDescriptorSetLayout,
                                 ClusteredLightingResources &clusteredLighting)
    : device_(&device), resourceHandles_(resourceHandles), phase_(phase),
      lightingDescriptorSetLayout_(lightingDescriptorSetLayout),
      atmosphereDescriptorSetLayout_(atmosphereDescriptorSetLayout),
      ddgiDescriptorSetLayout_(ddgiDescriptorSetLayout),
      clusteredLighting_(&clusteredLighting) {}

MainForwardPass::~MainForwardPass() = default;

void MainForwardPass::setup(RenderGraphBuilder &builder,
                            const RenderGraphBuildContext &context) const {
    builder.addNode(std::string(name()), RgPassType::Graphics,
                    RgQueueClass::Graphics);
    const FrameRenderFeatures &features = context.features;
    if (phase_ == ForwardPhase::Opaque)
        builder.setActive(features.forwardOpaqueRequired);
    else
        builder.setActive(features.transparentRequired);
    const auto sampled = [&](RenderImageHandle handle,
                             VkImageLayout layout) {
        if (handle.valid())
            builder.useImage({handle, RenderImageAccess::SampledRead,
                              layout, layout});
    };
    if (features.directionalShadowRequired)
        sampled(resourceHandles_.directionalShadowDepth,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    if (features.pointShadowRequired) {
        const uint32_t capacity = std::clamp(
            features.pointShadowLightCount, 1u,
            static_cast<uint32_t>(
                resourceHandles_.pointShadowDepthByCapacity.size()));
        sampled(resourceHandles_.pointShadowDepthByCapacity[capacity - 1],
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    }
    if (features.spotShadowRequired) {
        const uint32_t capacity = std::clamp(
            features.spotShadowLightCount, 1u,
            static_cast<uint32_t>(
                resourceHandles_.spotShadowDepthByCapacity.size()));
        sampled(resourceHandles_.spotShadowDepthByCapacity[capacity - 1],
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    }
    if (features.atmosphereRequired) {
        sampled(resourceHandles_.atmosphereTransmittance,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        sampled(resourceHandles_.atmosphereAerialPerspective,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (features.ddgiActive) {
        sampled(resourceHandles_.ddgiIrradiance,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        sampled(resourceHandles_.ddgiDistance,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (features.clusteredLightingRequired) {
        for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
            builder.useBuffer(
                clusteredLighting_->clusterCountBuffer(frame),
                RgBufferAccess::StorageRead, 0, VK_WHOLE_SIZE, frame);
            builder.useBuffer(
                clusteredLighting_->lightIndexBuffer(frame),
                RgBufferAccess::StorageRead, 0, VK_WHOLE_SIZE, frame);
        }
    }
    if (features.ssaoActive)
        sampled(resourceHandles_.ssaoFiltered,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    else if (features.cacaoActive)
        sampled(resourceHandles_.cacaoOutput,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    else if (features.gtaoActive)
        sampled(resourceHandles_.gtaoFiltered,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    if (phase_ == ForwardPhase::Transparent) {
        const RenderImageHandle transparentColor =
            transparentColorHandle(features, resourceHandles_);
        const RenderImageHandle transparentResolve =
            transparentResolveHandle(features, resourceHandles_);
        builder.addColorAttachment(
            transparentColor,
            RenderImageAccess::ColorAttachmentReadWrite,
            transparentResolve.valid()
                ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ATTACHMENT_LOAD_OP_LOAD,
            transparentResolve.valid() ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                       : VK_ATTACHMENT_STORE_OP_STORE,
            {}, {}, transparentResolve,
            transparentResolve.valid()
                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_UNDEFINED);
        builder.addDepthAttachment(
            transparentDepthHandle(features, resourceHandles_),
            RenderImageAccess::DepthAttachmentRead,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE);
        return;
    }

    const bool multisampled = resourceHandles_.hdrMsaaColor.valid();
    builder.addColorAttachment(
        multisampled ? resourceHandles_.hdrMsaaColor
                     : resourceHandles_.hdrColor,
        RenderImageAccess::ColorAttachmentReadWrite,
        multisampled ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                     : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ATTACHMENT_LOAD_OP_LOAD,
        multisampled &&
                !(features.transparentRequired &&
                  !features.surfaceDataRequired)
            ? VK_ATTACHMENT_STORE_OP_DONT_CARE
            : VK_ATTACHMENT_STORE_OP_STORE,
        VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}}, {},
        multisampled ? resourceHandles_.hdrColor : RenderImageHandle{},
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const bool specularBaseline = features.ssrActive || features.ssgiActive;
    if (specularBaseline) {
        builder.addColorAttachment(
            multisampled ? resourceHandles_.baselineSpecularMsaa
                         : resourceHandles_.baselineSpecular,
            RenderImageAccess::ColorAttachmentWrite,
            multisampled ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                         : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ATTACHMENT_LOAD_OP_CLEAR,
            multisampled ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                         : VK_ATTACHMENT_STORE_OP_STORE,
            VkClearColorValue{{0.0f, 0.0f, 0.0f, 0.0f}}, {},
            multisampled ? resourceHandles_.baselineSpecular
                         : RenderImageHandle{},
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (features.ssgiActive && resourceHandles_.baselineDiffuse.valid()) {
        builder.addColorAttachment(
            multisampled ? resourceHandles_.baselineDiffuseMsaa
                         : resourceHandles_.baselineDiffuse,
            RenderImageAccess::ColorAttachmentWrite,
            multisampled ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                         : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ATTACHMENT_LOAD_OP_CLEAR,
            multisampled ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                         : VK_ATTACHMENT_STORE_OP_STORE,
            VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}}, {},
            multisampled ? resourceHandles_.baselineDiffuse
                         : RenderImageHandle{},
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    builder.addDepthAttachment(
        resourceHandles_.mainDepth, RenderImageAccess::DepthAttachmentWrite,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_ATTACHMENT_LOAD_OP_CLEAR,
        features.transparentRequired && !features.surfaceDataRequired
            ? VK_ATTACHMENT_STORE_OP_STORE
            : VK_ATTACHMENT_STORE_OP_DONT_CARE,
        VkClearDepthStencilValue{1.0f, 0});
}

void MainForwardPass::recordNode(RenderGraphPassContext &context, uint32_t,
                                 const VisibilityFrame &visibility) {
    const RenderFrameContext &frame = context.frame;
    const RenderResourcePool &resources = context.resources;
    VKL_PROFILE_ZONE("Record Forward Phase");
    ScopedGpuLabel phaseLabel(
        device_->debugUtils(), frame.cmd,
        phase_ == ForwardPhase::Opaque ? "MainForward/Opaque"
                                       : "MainForward/Transparent");
    const RenderImageHandle colorHandle =
        phase_ == ForwardPhase::Opaque
            ? resourceHandles_.hdrColor
            : transparentColorHandle(frame.features, resourceHandles_);
    const VkExtent2D extent = resources.extent(colorHandle);
    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(frame.cmd, 0, 1, &scissor);
    drawQueue(frame, resources, visibility);
}

void MainForwardPass::drawQueue(const RenderFrameContext &frame,
                                const RenderResourcePool &resources,
                                const VisibilityFrame &visibility) {
    if (!frame.pipelineCache || !frame.viewMode || !frame.shaderRegistry)
        return;

    const bool transparent = phase_ == ForwardPhase::Transparent;
    const auto &indices = transparent ? visibility.cameraTransparent
                                      : visibility.cameraOpaque;
    const auto drawCommands = [&]() {
        ScopedGpuLabel queueLabel(device_->debugUtils(), frame.cmd,
                                  transparent ? "Transparent" : "Opaque");
        struct CachedPipeline {
            const MaterialTemplate *materialTemplate = nullptr;
            VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
            const ShaderProgram *program = nullptr;
            Pipeline *pipeline = nullptr;
        };

        std::vector<CachedPipeline> pipelineVariants;
        pipelineVariants.reserve(2);
        Pipeline *boundPipeline = nullptr;
        const MaterialInstance *boundMaterial = nullptr;

        for (uint32_t drawIndex = 0; drawIndex < indices.size(); ++drawIndex) {
            const RenderItemIndex itemIndex = indices[drawIndex];
            const RenderItem &command = visibility.items.at(itemIndex);
            if (!command.mesh || !command.material)
                continue;

            const auto &materialTemplate = command.material->materialTemplate();
            const auto &p = command.material->params();
            const MaterialShaderPass materialPass =
                transparent ? MaterialShaderPass::ForwardTransparent
                            : MaterialShaderPass::ForwardOpaque;
            const ShaderProgram &program =
                frame.shaderRegistry->materialProgram(
                    materialTemplate.shaderFamily(), materialPass,
                    frame.viewMode);
            const VkCullModeFlags cullMode =
                p.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
            auto cached = std::find_if(
                pipelineVariants.begin(), pipelineVariants.end(),
                [&](const CachedPipeline &entry) {
                    return entry.materialTemplate == &materialTemplate &&
                           entry.cullMode == cullMode;
                });
            if (cached == pipelineVariants.end()) {
                auto pipelineConfig = materialTemplate.pipelineConfig();
                const uint32_t opaqueAttachmentCount =
                    frame.features.ssgiActive
                        ? 3u
                        : ((frame.features.ssrActive ||
                            frame.features.ssgiActive)
                               ? 2u
                               : 1u);
                pipelineConfig.colorBlendAttachments.resize(
                    transparent ? 1u : opaqueAttachmentCount);
                pipelineConfig.colorBlendAttachments[0].blendEnable =
                    transparent;
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
                    ? resources.description(transparentDepthHandle(
                          frame.features, resourceHandles_)).samples
                    : resources.description(resourceHandles_.mainDepth)
                          .samples;
                pipelineConfig.vertShaderPath =
                    program.vertSpvPath;
                pipelineConfig.fragShaderPath =
                    program.fragmentSpvPath(
                        frame.materialSystem->activeMode(),
                        transparent ? 1u : opaqueAttachmentCount);
                pipelineConfig.debugName =
                    "Pipeline/MainForward/" + program.id +
                    "/ViewMode-" + frame.viewMode->id +
                    "/" + (transparent ? "Transparent" : "Opaque") +
                    "/" +
                    (cullMode == VK_CULL_MODE_NONE ? "CullNone"
                                                  : "CullBack");
                pipelineConfig.descriptorLayouts.insert(
                    pipelineConfig.descriptorLayouts.begin(),
                    frame.globalDescriptorSetLayout);
                pipelineConfig.descriptorLayouts.push_back(
                    lightingDescriptorSetLayout_);
                if (program.usesAtmosphere || program.usesScreenSpace) {
                    pipelineConfig.descriptorLayouts.push_back(
                        atmosphereDescriptorSetLayout_);
                }
                if (program.usesScreenSpace) {
                    pipelineConfig.descriptorLayouts.push_back(
                        frame.screenSpaceDescriptorSetLayout);
                }
                if (program.usesDdgi) {
                    pipelineConfig.descriptorLayouts.push_back(
                        ddgiDescriptorSetLayout_);
                }
                if (program.usesClusteredLighting) {
                    pipelineConfig.descriptorLayouts.push_back(
                        clusteredLighting_->descriptorSetLayout());
                }

                PipelineRenderingSignature signature{};
                signature.samples = pipelineConfig.msaaSamples;
                if (transparent) {
                    const RenderImageHandle transparentColor =
                        transparentColorHandle(frame.features,
                                               resourceHandles_);
                    signature.colorAttachmentFormats = {
                        resources.description(transparentColor).format};
                    signature.depthAttachmentFormat =
                        resources.description(transparentDepthHandle(
                                     frame.features, resourceHandles_))
                            .format;
                } else {
                    const VkFormat hdrFormat =
                        resources.description(resourceHandles_.hdrColor)
                            .format;
                    signature.colorAttachmentFormats = {hdrFormat};
                    if (frame.features.ssrActive ||
                        frame.features.ssgiActive) {
                        signature.colorAttachmentFormats.push_back(
                            hdrFormat);
                    }
                    if (frame.features.ssgiActive)
                        signature.colorAttachmentFormats.push_back(hdrFormat);
                    signature.depthAttachmentFormat =
                        resources.description(resourceHandles_.mainDepth)
                            .format;
                }
                Pipeline &pipeline = frame.pipelineCache->getOrCreate(
                    std::move(signature), std::move(pipelineConfig));
                pipelineVariants.push_back(
                    {&materialTemplate, cullMode, &program, &pipeline});
                cached = std::prev(pipelineVariants.end());
            }
            Pipeline &pipeline = *cached->pipeline;
            if (boundPipeline != &pipeline) {
                vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  pipeline.handle());
                vkCmdBindDescriptorSets(
                    frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline.layout(), 0, 1, &frame.globalDescriptorSet, 0,
                    nullptr);
                frame.materialSystem->bindGlobal(frame.cmd,
                                                  pipeline.layout());
                vkCmdBindDescriptorSets(
                    frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline.layout(), 2, 1,
                    &frame.lightingDescriptorSet, 0, nullptr);
                if (cached->program->usesAtmosphere) {
                    vkCmdBindDescriptorSets(
                        frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pipeline.layout(), 3, 1,
                        &frame.atmosphereDescriptorSet, 0, nullptr);
                }
                if (cached->program->usesScreenSpace) {
                    vkCmdBindDescriptorSets(
                        frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pipeline.layout(), 4, 1,
                        &frame.screenSpaceDescriptorSet, 0, nullptr);
                }
                if (cached->program->usesDdgi) {
                    vkCmdBindDescriptorSets(
                        frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pipeline.layout(), 5, 1,
                        &frame.ddgiDescriptorSet, 0, nullptr);
                }
                if (cached->program->usesClusteredLighting) {
                    vkCmdBindDescriptorSets(
                        frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pipeline.layout(), 6, 1,
                        &frame.clusteredLightingDescriptorSet, 0, nullptr);
                }
                boundPipeline = &pipeline;
                boundMaterial = nullptr;
            }

            if (frame.materialSystem->activeMode() ==
                    MaterialBindingMode::Legacy &&
                boundMaterial != command.material) {
                command.material->bindDescriptors(
                    frame.cmd, pipeline.layout(), frame.frameIndex);
                boundMaterial = command.material;
            }

            GpuPushBlock block{};
            block.model = command.world;
            block.indices = glm::uvec4(command.materialIndex, itemIndex, 0u,
                                       0u);

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



} // namespace vkr
