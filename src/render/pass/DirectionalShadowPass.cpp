#include "DirectionalShadowPass.h"

#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "render/pipeline/Pipeline.h"
#include "render/pipeline/PipelineConfigBuilder.h"
#include "core/VulkanCheck.h"
#include "render/DirectionalShadow.h"
#include "render/GpuMaterialData.h"
#include "render/MaterialInstance.h"
#include "render/MaterialSystem.h"
#include "render/MaterialTemplate.h"
#include "render/Mesh.h"
#include "render/PipelineCache.h"
#include "render/PipelineKey.h"
#include "render/RenderFrame.h"
#include "render/RenderGraph.h"
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
    depthFormat_ = resources.description(shadowDepth_).format;
}

DirectionalShadowPass::~DirectionalShadowPass() = default;

void DirectionalShadowPass::setup(
    RenderGraphBuilder &builder,
    const RenderGraphBuildContext &context) const {
    for (uint32_t cascade = 0; cascade < kCsmCascadeCount; ++cascade) {
        builder.addNode("DirectionalShadow/Cascade" +
                            std::to_string(cascade),
                        RgPassType::Graphics, RgQueueClass::Graphics,
                        cascade);
        builder.setActive(
            cascade < context.features.directionalShadowCascadeCount);
        builder.addDepthAttachment(
            shadowDepth_, RenderImageAccess::DepthAttachmentWrite,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
            {1.0f, 0},
            {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, cascade, 1});
    }
}

void DirectionalShadowPass::recordNode(
    RenderGraphPassContext &context, uint32_t localNodeIndex,
    const VisibilityFrame &visibility) {
    recordCascade(context.frame, visibility, localNodeIndex);
}

void DirectionalShadowPass::recordCascade(
    const RenderFrameContext &frame, const VisibilityFrame &visibility,
    uint32_t cascade) {
    if (cascade >= kCsmCascadeCount || !frame.view ||
        !frame.view->shadow.csm.enabled)
        return;
    VkViewport viewport{};
    viewport.width = static_cast<float>(kDirectionalShadowMapSize);
    viewport.height = static_cast<float>(kDirectionalShadowMapSize);
    viewport.maxDepth = 1.0f;
    const VkRect2D scissor{{0, 0},
                           {kDirectionalShadowMapSize,
                            kDirectionalShadowMapSize}};

    vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
    vkCmdSetScissor(frame.cmd, 0, 1, &scissor);
    drawCasters(frame, visibility, cascade);
}

void DirectionalShadowPass::drawCasters(
    const RenderFrameContext &frame,
    const VisibilityFrame &visibility,
    uint32_t cascadeIndex) {
    if (!frame.pipelineCache || !frame.view)
        return;

    Pipeline *boundPipeline = nullptr;
    const MaterialInstance *boundMaterial = nullptr;
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
        builder.descriptorLayout(materialTemplate.descriptorSetLayout());
        PipelineConfig config = builder.build();
        config.debugName =
            "Pipeline/DirectionalShadow/" +
            std::string(alphaMasked ? "Mask" : "Opaque") + "/" +
            (cullMode == VK_CULL_MODE_NONE ? "CullNone" : "CullBack");

        PipelineRenderingSignature rendering{};
        rendering.depthAttachmentFormat = depthFormat_;
        rendering.samples = VK_SAMPLE_COUNT_1_BIT;
        Pipeline &pipeline = frame.pipelineCache->getOrCreate(
            std::move(rendering), std::move(config));
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
            frame.materialSystem->bindGlobal(frame.cmd, pipeline.layout());
            boundPipeline = &pipeline;
            boundMaterial = nullptr;
        }
        if (alphaMasked &&
            frame.materialSystem->activeMode() ==
                MaterialBindingMode::Legacy &&
            boundMaterial != command.material) {
            command.material->bindDescriptors(
                frame.cmd, pipeline.layout(), frame.frameIndex);
            boundMaterial = command.material;
        }

        GpuPushBlock block{};
        block.model = command.world;
        block.indices = glm::uvec4(command.materialIndex, itemIndex,
                                   cascadeIndex, 0u);
        vkCmdPushConstants(frame.cmd, pipeline.layout(), pushStages, 0,
                           sizeof(block), &block);
        command.mesh->bind(frame.cmd);
        command.mesh->draw(frame.cmd, itemIndex);
    }
}

} // namespace vkr
