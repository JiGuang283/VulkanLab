#include "render/features/shadows_visibility/ShadowCasterDrawRecorder.h"

#include "render/pipeline/Pipeline.h"
#include "render/pipeline/PipelineConfigBuilder.h"
#include "render/material/GpuMaterialData.h"
#include "render/material/MaterialInstance.h"
#include "render/material/MaterialSystem.h"
#include "render/material/MaterialTemplate.h"
#include "render/geometry/Mesh.h"
#include "render/pipeline/PipelineCache.h"
#include "render/frame/RenderFrame.h"
#include "render/frame/RenderView.h"
#include "render/features/shadows_visibility/Visibility.h"

#include <utility>

namespace vkr {

void ShadowCasterDrawRecorder::record(
    const RenderFrameContext &frame, const VisibilityFrame &visibility,
    const std::vector<RenderItemIndex> &casters,
    const ShadowCasterDrawConfig &config) {
    if (!frame.pipelineCache || !frame.view ||
        config.rendering.depthAttachmentFormat == VK_FORMAT_UNDEFINED ||
        config.sliceDescriptorLayout == VK_NULL_HANDLE ||
        config.sliceDescriptorSet == VK_NULL_HANDLE) {
        return;
    }

    Pipeline *boundPipeline = nullptr;
    const MaterialInstance *boundMaterial = nullptr;
    for (RenderItemIndex itemIndex : casters) {
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
            .shaders(config.vertexShader,
                     alphaMasked ? config.maskFragmentShader
                                 : config.opaqueFragmentShader)
            .defaultVertexLayout()
            .rasterization(
                cullMode, materialTemplate.pipelineConfig().frontFace)
            .depth(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
            .depthBias(config.rasterDepthBias)
            .colorAttachmentCount(0)
            .msaa(VK_SAMPLE_COUNT_1_BIT)
            .descriptorLayout(config.sliceDescriptorLayout)
            .pushConstant({pushStages, 0, sizeof(GpuPushBlock)});
        builder.descriptorLayout(materialTemplate.descriptorSetLayout());
        PipelineConfig pipelineConfig = builder.build();
        pipelineConfig.debugName =
            "Pipeline/" + config.pipelinePrefix + "/" +
            (alphaMasked ? "Mask" : "Opaque") + "/" +
            (cullMode == VK_CULL_MODE_NONE ? "CullNone" : "CullBack");

        Pipeline &pipeline = frame.pipelineCache->getOrCreate(
            config.rendering, std::move(pipelineConfig));
        if (boundPipeline != &pipeline) {
            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline.handle());
            if (config.rasterDepthBias) {
                vkCmdSetDepthBias(
                    frame.cmd, frame.view->settings.shadowConstantBias,
                    0.0f, frame.view->settings.shadowSlopeBias);
            }
            vkCmdBindDescriptorSets(
                frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline.layout(), 0, 1, &config.sliceDescriptorSet, 1,
                &config.dynamicOffset);
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
        block.indices =
            glm::uvec4(command.materialIndex, itemIndex, 0u, 0u);
        vkCmdPushConstants(frame.cmd, pipeline.layout(), pushStages, 0,
                           sizeof(block), &block);
        command.mesh->bind(frame.cmd);
        command.mesh->draw(frame.cmd, itemIndex);
    }
}

} // namespace vkr
