#include "render/features/surface/SurfaceDrawRecorder.h"

#include "render/features/shadows_visibility/OcclusionCullPass.h"
#include "render/features/shadows_visibility/Visibility.h"
#include "render/features/surface/SurfaceFrameData.h"
#include "render/frame/RenderFrame.h"
#include "render/geometry/Mesh.h"
#include "render/graph/RenderResourcePool.h"
#include "render/material/GpuMaterialData.h"
#include "render/material/MaterialInstance.h"
#include "render/material/MaterialSystem.h"
#include "render/material/MaterialTemplate.h"
#include "render/pipeline/Pipeline.h"
#include "render/pipeline/PipelineCache.h"
#include "render/pipeline/PipelineConfigBuilder.h"
#include "render/shader/ShaderRegistry.h"

#include <algorithm>
#include <array>

namespace vkr {

void recordSurfaceDraws(const RenderFrameContext &frame,
                        const RenderResourcePool &,
                        const VisibilityFrame &visibility,
                        const SurfaceFrameData &frameData,
                        const SurfaceDrawRecordConfig &config) {
    if (!frame.shaderRegistry || !frame.pipelineCache ||
        !frame.materialSystem) {
        return;
    }

    struct CachedPipeline {
        const MaterialTemplate *materialTemplate = nullptr;
        bool alphaMasked = false;
        VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
        Pipeline *pipeline = nullptr;
    };

    std::vector<CachedPipeline> pipelineVariants;
    pipelineVariants.reserve(4);
    Pipeline *boundPipeline = nullptr;
    const MaterialInstance *boundMaterial = nullptr;
    const GpuVisibilityDrawStream *stream = frame.visibilityDrawStream;

    for (uint32_t drawIndex = 0;
         drawIndex < visibility.cameraOpaque.size(); ++drawIndex) {
        const RenderItemIndex itemIndex =
            visibility.cameraOpaque[drawIndex];
        const RenderItem &item = visibility.items.at(itemIndex);
        if (!item.mesh || !item.material)
            continue;

        const MaterialParams &params = item.material->params();
        const bool alphaMasked = params.alphaMode == AlphaMode::Mask;
        const MaterialTemplate &materialTemplate =
            item.material->materialTemplate();
        const ShaderProgram &program = frame.shaderRegistry->materialProgram(
            materialTemplate.shaderFamily(),
            alphaMasked ? config.maskPass : config.opaquePass);
        const VkCullModeFlags cullMode =
            params.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        auto cached = std::find_if(
            pipelineVariants.begin(), pipelineVariants.end(),
            [&](const CachedPipeline &entry) {
                return entry.materialTemplate == &materialTemplate &&
                       entry.alphaMasked == alphaMasked &&
                       entry.cullMode == cullMode;
            });
        if (cached == pipelineVariants.end()) {
            PipelineConfig pipelineConfig =
                PipelineConfigBuilder{}
                    .shaders(program.vertSpvPath,
                             program.fragmentSpvPath(
                                 frame.materialSystem->activeMode(),
                                 static_cast<uint32_t>(
                                     config.colorAttachmentFormats.size())))
                    .defaultVertexLayout()
                    .rasterization(
                        cullMode,
                        materialTemplate.pipelineConfig().frontFace)
                    .depth(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
                    .blending(false)
                    .colorAttachmentCount(static_cast<uint32_t>(
                        config.colorAttachmentFormats.size()))
                    .msaa(VK_SAMPLE_COUNT_1_BIT)
                    .descriptorLayout(frame.globalDescriptorSetLayout)
                    .descriptorLayout(
                        materialTemplate.descriptorSetLayout())
                    .descriptorLayout(frameData.descriptorSetLayout())
                    .pushConstant({VK_SHADER_STAGE_VERTEX_BIT |
                                       VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(GpuPushBlock)})
                    .build();
            pipelineConfig.debugName =
                "Pipeline/" + config.debugName + "/" + program.id + "/" +
                (alphaMasked ? "Mask" : "Opaque") + "/" +
                (cullMode == VK_CULL_MODE_NONE ? "CullNone" : "CullBack") +
                "/Mrt" +
                std::to_string(config.colorAttachmentFormats.size());

            PipelineRenderingSignature signature{};
            signature.colorAttachmentFormats =
                config.colorAttachmentFormats;
            signature.depthAttachmentFormat =
                config.depthAttachmentFormat;
            Pipeline &pipeline = frame.pipelineCache->getOrCreate(
                std::move(signature), std::move(pipelineConfig));
            pipelineVariants.push_back(
                {&materialTemplate, alphaMasked, cullMode, &pipeline});
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
            frame.materialSystem->bindGlobal(frame.cmd, pipeline.layout());
            const VkDescriptorSet surfaceSet =
                frameData.descriptorSet(frame.frameIndex);
            vkCmdBindDescriptorSets(
                frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline.layout(), 2, 1, &surfaceSet, 0, nullptr);
            boundPipeline = &pipeline;
            boundMaterial = nullptr;
        }
        if (frame.materialSystem->activeMode() ==
                MaterialBindingMode::Legacy &&
            boundMaterial != item.material) {
            item.material->bindDescriptors(frame.cmd, pipeline.layout(),
                                           frame.frameIndex);
            boundMaterial = item.material;
        }

        GpuPushBlock push{};
        push.model = item.world;
        push.indices = glm::uvec4(item.materialIndex, itemIndex, 0u, 0u);
        vkCmdPushConstants(frame.cmd, pipeline.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        item.mesh->bind(frame.cmd);
        if (config.useVisibilityIndirect && stream && stream->active &&
            stream->indirectBuffer != VK_NULL_HANDLE &&
            stream->frameIndex == frame.frameIndex &&
            stream->visibilityGeneration == visibility.generation &&
            drawIndex < stream->candidateCount) {
            item.mesh->drawIndirect(
                frame.cmd, stream->indirectBuffer,
                static_cast<VkDeviceSize>(drawIndex) *
                    sizeof(VkDrawIndexedIndirectCommand));
        } else {
            item.mesh->draw(frame.cmd, itemIndex);
        }
    }
}

} // namespace vkr
