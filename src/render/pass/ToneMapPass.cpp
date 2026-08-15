#include "ToneMapPass.h"

#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "render/pipeline/Pipeline.h"
#include "render/pipeline/PipelineConfigBuilder.h"
#include "core/VulkanCheck.h"
#include "render/FrameGpuData.h"
#include "render/PipelineCache.h"
#include "render/PipelineKey.h"
#include "render/RenderFrame.h"
#include "render/RenderGraph.h"
#include "render/RenderResourceRegistry.h"
#include "render/RenderView.h"
#include "render/ShaderVariant.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"

#include <array>
#include <utility>

namespace vkr {

namespace {

uint32_t toneMapperValue(ToneMapper toneMapper) {
    switch (toneMapper) {
    case ToneMapper::PassThrough:
        return 0;
    case ToneMapper::Reinhard:
        return 1;
    case ToneMapper::Aces:
        return 2;
    }
    return 2;
}

bool isSrgbFormat(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_SRGB ||
           format == VK_FORMAT_R8G8B8A8_SRGB;
}

uint32_t surfaceDebugModeValue(SurfaceDebugView view) {
    switch (view) {
    case SurfaceDebugView::None:
        return 0;
    case SurfaceDebugView::Normal:
        return 1;
    case SurfaceDebugView::Roughness:
        return 2;
    case SurfaceDebugView::Motion:
        return 3;
    case SurfaceDebugView::HistoryValidity:
        return 4;
    }
    return 0;
}

uint32_t screenDebugModeValue(ScreenSpaceDebugView view) {
    switch (view) {
    case ScreenSpaceDebugView::None:
        return 0;
    case ScreenSpaceDebugView::NearestDepth:
        return 1;
    case ScreenSpaceDebugView::SceneColor:
        return 2;
    case ScreenSpaceDebugView::SsaoRaw:
        return 3;
    case ScreenSpaceDebugView::SsaoFiltered:
        return 4;
    case ScreenSpaceDebugView::CacaoOutput:
        return 5;
    case ScreenSpaceDebugView::GtaoRaw:
        return 6;
    case ScreenSpaceDebugView::GtaoTemporal:
        return 7;
    case ScreenSpaceDebugView::GtaoFiltered:
        return 8;
    case ScreenSpaceDebugView::GtaoRejection:
        return 9;
    case ScreenSpaceDebugView::GtaoHistoryWeight:
        return 10;
    case ScreenSpaceDebugView::TaaHistory:
        return 11;
    case ScreenSpaceDebugView::TaaRejection:
        return 12;
    case ScreenSpaceDebugView::TaaHistoryWeight:
        return 13;
    case ScreenSpaceDebugView::SsrRaw:
        return 14;
    case ScreenSpaceDebugView::SsrTemporal:
        return 15;
    case ScreenSpaceDebugView::SsrFiltered:
        return 16;
    case ScreenSpaceDebugView::SsrConfidence:
        return 17;
    case ScreenSpaceDebugView::SsrRejection:
        return 18;
    case ScreenSpaceDebugView::SsgiRaw:
        return 19;
    case ScreenSpaceDebugView::SsgiTemporal:
        return 20;
    case ScreenSpaceDebugView::SsgiFiltered:
        return 21;
    case ScreenSpaceDebugView::SsgiConfidence:
        return 22;
    case ScreenSpaceDebugView::SsgiVariance:
        return 23;
    case ScreenSpaceDebugView::SsgiRejection:
        return 24;
    }
    return 0;
}

} // namespace

ToneMapPass::ToneMapPass(Device &device,
                const RenderResourceRegistry &resources,
                RenderImageHandle hdrColor,
                RenderImageHandle compositedHdrColor,
                         RenderSamplerHandle hdrSampler,
                         RenderImageHandle bloomColor,
                         RenderSamplerHandle bloomSampler,
                         RenderImageHandle viewportColor,
                         RenderImageHandle surfaceNormalRoughness,
                         RenderImageHandle surfaceMotion,
                         RenderSamplerHandle surfaceSampler,
                         RenderImageHandle screenDepthPyramid,
                         RenderImageHandle sceneColorPyramid,
                         RenderImageHandle ssaoRaw,
                         RenderImageHandle ssaoFiltered,
                         RenderImageHandle cacaoOutput,
                         RenderImageHandle gtaoRaw,
                         RenderImageHandle gtaoHistory,
                         RenderImageHandle gtaoFiltered,
                         RenderImageHandle gtaoDebug,
                         RenderImageHandle taaHistory,
                         RenderImageHandle taaDebug,
                         RenderImageHandle ssrRaw,
                         RenderImageHandle ssrHistory,
                         RenderImageHandle ssrFiltered,
                         RenderImageHandle ssrDebug,
                         RenderImageHandle ssgiRaw,
                         RenderImageHandle ssgiHistory,
                         RenderImageHandle ssgiFiltered,
                         RenderImageHandle ssgiDebug,
                         RenderSamplerHandle screenPyramidSampler,
                         RenderSamplerHandle ssaoSampler,
                         RenderSamplerHandle taaSampler,
                         RenderSamplerHandle ssrSampler,
                         RenderSamplerHandle ssgiSampler,
                         DescriptorAllocator &descriptorAllocator,
                         std::string fullscreenVertPath,
                         std::string toneMapFragPath)
    : device_(&device), hdrColor_(hdrColor),
      compositedHdrColor_(compositedHdrColor), hdrSampler_(hdrSampler),
      bloomColor_(bloomColor), bloomSampler_(bloomSampler),
      viewportColor_(viewportColor),
      surfaceNormalRoughness_(surfaceNormalRoughness),
      surfaceMotion_(surfaceMotion), surfaceSampler_(surfaceSampler),
      screenDepthPyramid_(screenDepthPyramid),
      sceneColorPyramid_(sceneColorPyramid), ssaoRaw_(ssaoRaw),
      ssaoFiltered_(ssaoFiltered), cacaoOutput_(cacaoOutput),
      gtaoRaw_(gtaoRaw), gtaoHistory_(gtaoHistory),
      gtaoFiltered_(gtaoFiltered), gtaoDebug_(gtaoDebug),
      taaHistory_(taaHistory), taaDebug_(taaDebug),
      ssrRaw_(ssrRaw), ssrHistory_(ssrHistory),
      ssrFiltered_(ssrFiltered), ssrDebug_(ssrDebug),
      ssgiRaw_(ssgiRaw), ssgiHistory_(ssgiHistory),
      ssgiFiltered_(ssgiFiltered), ssgiDebug_(ssgiDebug),
      screenPyramidSampler_(screenPyramidSampler),
      ssaoSampler_(ssaoSampler), taaSampler_(taaSampler),
      ssrSampler_(ssrSampler), ssgiSampler_(ssgiSampler),
      descriptorAllocator_(&descriptorAllocator),
      fullscreenVertPath_(std::move(fullscreenVertPath)),
      toneMapFragPath_(std::move(toneMapFragPath)) {
    createDescriptors(resources);
}

ToneMapPass::~ToneMapPass() {
    for (VkDescriptorSet set : sourceDescriptorSets_)
        descriptorAllocator_->free(set);
    if (sourceDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     sourceDescriptorSetLayout_, nullptr);
    }
}

void ToneMapPass::setup(RenderGraphBuilder &builder,
                        const RenderGraphBuildContext &context) const {
    builder.addNode(std::string(name()), RgPassType::Graphics,
                    RgQueueClass::Graphics);
    const auto sampled = [&](RenderImageHandle handle,
                             VkImageLayout layout =
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        if (handle.valid())
            builder.useImage({handle, RenderImageAccess::SampledRead,
                              layout, layout});
    };
    const FrameRenderFeatures &features = context.features;
    sampled(context.features.lightingCompositeRequired
                ? compositedHdrColor_
                : hdrColor_);
    if (features.bloomRequired)
        sampled(bloomColor_, VK_IMAGE_LAYOUT_GENERAL);
    if (features.surfaceNormalsRequired)
        sampled(surfaceNormalRoughness_);
    if (features.surfaceMotionRequired)
        sampled(surfaceMotion_);
    if (features.screenDepthPyramidRequired)
        sampled(screenDepthPyramid_);
    if (features.sceneColorPyramidRequired)
        sampled(sceneColorPyramid_);
    if (features.ssaoRequired) {
        sampled(ssaoRaw_);
        sampled(ssaoFiltered_);
    }
    if (features.cacaoRequired)
        sampled(cacaoOutput_);
    if (features.gtaoRequired) {
        for (RenderImageHandle handle :
             {gtaoRaw_, gtaoHistory_, gtaoFiltered_, gtaoDebug_})
            sampled(handle);
    }
    if (features.taaRequired) {
        sampled(taaHistory_);
        sampled(taaDebug_);
    }
    if (features.ssrRequired) {
        for (RenderImageHandle handle :
             {ssrRaw_, ssrHistory_, ssrFiltered_, ssrDebug_})
            sampled(handle);
    }
    if (features.ssgiRequired) {
        for (RenderImageHandle handle :
             {ssgiRaw_, ssgiHistory_, ssgiFiltered_, ssgiDebug_})
            sampled(handle);
    }
    builder.addColorAttachment(
        viewportColor_, RenderImageAccess::ColorAttachmentWrite,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
        VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}});
}

void ToneMapPass::onViewportResize(
    const RenderResourceRegistry &resources) {
    updateDescriptors(resources);
}

void ToneMapPass::recordNode(RenderGraphPassContext &context, uint32_t,
                             const VisibilityFrame &) {
    const RenderFrameContext &frame = context.frame;
    const RenderResourceRegistry &resources = context.resources;
    VKL_PROFILE_ZONE("Record ToneMap");
    VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd, "ToneMap");
    if (!frame.pipelineCache || !frame.view || !frame.shaderVariant)
        return;
    updateScreenDescriptors(resources, frame.frameIndex, frame.features,
                            frame.view->settings.screenSpaceDebugView);

    VkViewport viewport{};
    viewport.width = static_cast<float>(frame.viewportExtent.width);
    viewport.height = static_cast<float>(frame.viewportExtent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, frame.viewportExtent};
    vkCmdSetScissor(frame.cmd, 0, 1, &scissor);

    {
        VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd,
                             "FullscreenToneMap");
        ScopedGpuLabel label(device_->debugUtils(), frame.cmd,
                             "FullscreenToneMap");
        PipelineConfig config =
            PipelineConfigBuilder{}
                .shaders(fullscreenVertPath_, toneMapFragPath_)
                .vertexLayout(VertexLayout{})
                .rasterization(VK_CULL_MODE_NONE,
                               VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .depth(false, false)
                .blending(false)
                .msaa(VK_SAMPLE_COUNT_1_BIT)
                .descriptorLayout(sourceDescriptorSetLayout_)
                .pushConstant({VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(ToneMapPushConstants)})
                .build();
        config.debugName = "Pipeline/ToneMap/Fullscreen";

        PipelineRenderingSignature signature{};
        signature.colorAttachmentFormats = {
            resources.description(viewportColor_).format};
        Pipeline &pipeline = frame.pipelineCache->getOrCreate(
            std::move(signature), std::move(config));
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline.handle());
        const VkDescriptorSet sourceSet =
            sourceDescriptorSets_.at(frame.frameIndex);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline.layout(), 0, 1, &sourceSet, 0,
                                nullptr);

        ToneMapPushConstants push{};
        const bool surfaceDebug =
            frame.view->settings.surfaceDebugView != SurfaceDebugView::None &&
            surfaceNormalRoughness_.valid() && surfaceMotion_.valid();
        const ScreenSpaceDebugView requestedScreenDebug =
            frame.view->settings.screenSpaceDebugView;
        const bool screenDebug =
            (requestedScreenDebug == ScreenSpaceDebugView::NearestDepth &&
             screenDepthPyramid_.valid()) ||
            (requestedScreenDebug == ScreenSpaceDebugView::SceneColor &&
             sceneColorPyramid_.valid()) ||
            ((requestedScreenDebug == ScreenSpaceDebugView::SsaoRaw ||
              requestedScreenDebug ==
                   ScreenSpaceDebugView::SsaoFiltered) &&
              ssaoRaw_.valid()) ||
            (requestedScreenDebug == ScreenSpaceDebugView::CacaoOutput &&
             cacaoOutput_.valid() && frame.features.cacaoRequired) ||
            ((requestedScreenDebug == ScreenSpaceDebugView::GtaoRaw ||
              requestedScreenDebug == ScreenSpaceDebugView::GtaoTemporal ||
              requestedScreenDebug == ScreenSpaceDebugView::GtaoFiltered ||
              requestedScreenDebug == ScreenSpaceDebugView::GtaoRejection ||
              requestedScreenDebug ==
                  ScreenSpaceDebugView::GtaoHistoryWeight) &&
             gtaoRaw_.valid() && frame.features.gtaoRequired) ||
            ((requestedScreenDebug == ScreenSpaceDebugView::TaaHistory ||
              requestedScreenDebug == ScreenSpaceDebugView::TaaRejection ||
              requestedScreenDebug ==
                  ScreenSpaceDebugView::TaaHistoryWeight) &&
             taaHistory_.valid() && frame.features.taaRequired);
        const bool sceneColorDebug =
            screenDebug &&
            (requestedScreenDebug == ScreenSpaceDebugView::SceneColor ||
             requestedScreenDebug == ScreenSpaceDebugView::TaaHistory);
        const bool configurableOutput =
            sceneColorDebug ||
            (!surfaceDebug && !screenDebug &&
             frame.shaderVariant->toneMapping ==
                 ShaderToneMappingPolicy::Configurable);
        if (configurableOutput) {
            push.exposureEv = frame.view->settings.exposureEv;
            push.toneMapper = toneMapperValue(frame.view->settings.toneMapper);
            push.applyExposure = 1;
        }
        if (!surfaceDebug && !screenDebug && bloomColor_.valid() &&
            frame.view->settings.bloomEnabled &&
            frame.shaderVariant->supportsBloom) {
            push.bloomIntensity = frame.view->settings.bloomIntensity;
            push.applyBloom = 1;
        }
        push.encodeGamma =
            isSrgbFormat(resources.description(viewportColor_).format)
                ? 0u
                : 1u;
        push.surfaceDebugMode = surfaceDebug
                                    ? surfaceDebugModeValue(
                                          frame.view->settings.surfaceDebugView)
                                    : 0u;
        push.motionDebugScale =
            frame.view->settings.surfaceMotionDebugScale;
        push.screenDebugMode =
            !surfaceDebug && screenDebug
                ? screenDebugModeValue(requestedScreenDebug)
                : 0u;
        push.screenDebugMip = frame.view->settings.screenSpaceDebugMip;
        push.cameraNear = frame.view->cameraNearPlane;
        push.cameraFar = frame.view->cameraFarPlane;
        vkCmdPushConstants(frame.cmd, pipeline.layout(),
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                           &push);
        vkCmdDraw(frame.cmd, 3, 1, 0, 0);
    }

}

void ToneMapPass::createDescriptors(
    const RenderResourceRegistry &resources) {
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    for (uint32_t bindingIndex = 0; bindingIndex < bindings.size();
         ++bindingIndex) {
        bindings[bindingIndex].binding = bindingIndex;
        bindings[bindingIndex].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[bindingIndex].descriptorCount = 1;
        bindings[bindingIndex].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr,
                                         &sourceDescriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, sourceDescriptorSetLayout_,
        "Pass/ToneMap/SourceDescriptorSetLayout");
    for (uint32_t frame = 0; frame < sourceDescriptorSets_.size(); ++frame) {
        sourceDescriptorSets_[frame] = descriptorAllocator_->allocate(
            sourceDescriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5}},
            "Pass/ToneMap/SourceDescriptorSet/Frame" +
                std::to_string(frame));
    }
    updateDescriptors(resources);
}

void ToneMapPass::updateDescriptors(
    const RenderResourceRegistry &resources) {
    for (uint32_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT;
         ++frameIndex) {
        VkDescriptorImageInfo hdrInfo{};
        hdrInfo.sampler = resources.sampler(hdrSampler_);
        hdrInfo.imageView =
            resources.image(hdrColor_, frameIndex).imageView();
        hdrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo bloomInfo = hdrInfo;

        VkDescriptorImageInfo normalRoughnessInfo = hdrInfo;
        VkDescriptorImageInfo motionInfo = hdrInfo;
        VkDescriptorImageInfo debugInfo = hdrInfo;

        std::array<VkWriteDescriptorSet, 5> writes{};
        for (uint32_t bindingIndex = 0; bindingIndex < writes.size();
             ++bindingIndex) {
            writes[bindingIndex].sType =
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[bindingIndex].dstSet =
                sourceDescriptorSets_[frameIndex];
            writes[bindingIndex].dstBinding = bindingIndex;
            writes[bindingIndex].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[bindingIndex].descriptorCount = 1;
        }
        writes[0].pImageInfo = &hdrInfo;
        writes[1].pImageInfo = &bloomInfo;
        writes[2].pImageInfo = &normalRoughnessInfo;
        writes[3].pImageInfo = &motionInfo;
        writes[4].pImageInfo = &debugInfo;
        vkUpdateDescriptorSets(
            device_->logicalDevice(),
            static_cast<uint32_t>(writes.size()), writes.data(), 0,
            nullptr);
    }
}

void ToneMapPass::updateScreenDescriptors(
    const RenderResourceRegistry &resources, uint32_t frameIndex,
    const FrameRenderFeatures &features, ScreenSpaceDebugView debugView) {
    VkDescriptorImageInfo fallback{};
    fallback.sampler = resources.sampler(hdrSampler_);
    const RenderImageHandle primaryHdr =
        features.lightingCompositeRequired ? compositedHdrColor_ : hdrColor_;
    fallback.imageView = resources.image(primaryHdr, frameIndex).imageView();
    fallback.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo bloomInfo = fallback;
    if (features.bloomRequired && bloomColor_.valid() &&
        resources.resident(bloomColor_)) {
        bloomInfo = {resources.sampler(bloomSampler_),
                     resources.image(bloomColor_, frameIndex).imageView(),
                     VK_IMAGE_LAYOUT_GENERAL};
    }

    VkDescriptorImageInfo primary = fallback;
    if (features.taaActive && taaHistory_.valid() &&
        resources.resident(taaHistory_)) {
        primary = {resources.sampler(taaSampler_),
                   resources.image(taaHistory_, frameIndex).imageView(),
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    VkDescriptorImageInfo normalInfo = fallback;
    VkDescriptorImageInfo motionInfo = fallback;
    if (features.surfaceNormalsRequired &&
        surfaceNormalRoughness_.valid() &&
        resources.resident(surfaceNormalRoughness_)) {
        const VkSampler sampler = resources.sampler(surfaceSampler_);
        normalInfo = {
            sampler,
            resources.image(surfaceNormalRoughness_, frameIndex).imageView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    if (features.surfaceMotionRequired && surfaceMotion_.valid() &&
        resources.resident(surfaceMotion_)) {
        const VkSampler sampler = resources.sampler(surfaceSampler_);
        motionInfo = {
            sampler, resources.image(surfaceMotion_, frameIndex).imageView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    VkDescriptorImageInfo debugInfo = fallback;
    const auto sampled = [&](RenderImageHandle image,
                             RenderSamplerHandle sampler) {
        if (!image.valid() || !resources.resident(image))
            return fallback;
        return VkDescriptorImageInfo{
            resources.sampler(sampler),
            resources.image(image, frameIndex).imageView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    };
    switch (debugView) {
    case ScreenSpaceDebugView::NearestDepth:
        if (features.screenDepthPyramidRequired &&
            screenDepthPyramid_.valid())
            debugInfo = sampled(screenDepthPyramid_, screenPyramidSampler_);
        break;
    case ScreenSpaceDebugView::SceneColor:
        if (features.sceneColorPyramidRequired && sceneColorPyramid_.valid())
            debugInfo = sampled(sceneColorPyramid_, screenPyramidSampler_);
        break;
    case ScreenSpaceDebugView::SsaoRaw:
        if (features.ssaoRequired && ssaoRaw_.valid())
            debugInfo = sampled(ssaoRaw_, ssaoSampler_);
        break;
    case ScreenSpaceDebugView::SsaoFiltered:
        if (features.ssaoRequired && ssaoFiltered_.valid())
            debugInfo = sampled(ssaoFiltered_, ssaoSampler_);
        break;
    case ScreenSpaceDebugView::CacaoOutput:
        if (features.cacaoRequired && cacaoOutput_.valid())
            debugInfo = sampled(cacaoOutput_, ssaoSampler_);
        break;
    case ScreenSpaceDebugView::GtaoRaw:
        if (features.gtaoRequired && gtaoRaw_.valid())
            debugInfo = sampled(gtaoRaw_, ssaoSampler_);
        break;
    case ScreenSpaceDebugView::GtaoTemporal:
        if (features.gtaoRequired && gtaoHistory_.valid())
            debugInfo = sampled(gtaoHistory_, ssaoSampler_);
        break;
    case ScreenSpaceDebugView::GtaoFiltered:
        if (features.gtaoRequired && gtaoFiltered_.valid())
            debugInfo = sampled(gtaoFiltered_, ssaoSampler_);
        break;
    case ScreenSpaceDebugView::GtaoRejection:
    case ScreenSpaceDebugView::GtaoHistoryWeight:
        if (features.gtaoRequired && gtaoDebug_.valid())
            debugInfo = sampled(gtaoDebug_, ssaoSampler_);
        break;
    case ScreenSpaceDebugView::TaaHistory:
        if (features.taaRequired && taaHistory_.valid())
            debugInfo = sampled(taaHistory_, taaSampler_);
        break;
    case ScreenSpaceDebugView::TaaRejection:
    case ScreenSpaceDebugView::TaaHistoryWeight:
        if (features.taaRequired && taaDebug_.valid())
            debugInfo = sampled(taaDebug_, taaSampler_);
        break;
    case ScreenSpaceDebugView::SsrRaw:
        if (features.ssrRequired && ssrRaw_.valid())
            debugInfo = sampled(ssrRaw_, ssrSampler_);
        break;
    case ScreenSpaceDebugView::SsrTemporal:
        if (features.ssrRequired && ssrHistory_.valid())
            debugInfo = sampled(ssrHistory_, ssrSampler_);
        break;
    case ScreenSpaceDebugView::SsrFiltered:
        if (features.ssrRequired && ssrFiltered_.valid())
            debugInfo = sampled(ssrFiltered_, ssrSampler_);
        break;
    case ScreenSpaceDebugView::SsrConfidence:
    case ScreenSpaceDebugView::SsrRejection:
        if (features.ssrRequired && ssrDebug_.valid())
            debugInfo = sampled(ssrDebug_, ssrSampler_);
        break;
    case ScreenSpaceDebugView::SsgiRaw:
        if (features.ssgiRequired && ssgiRaw_.valid())
            debugInfo = sampled(ssgiRaw_, ssgiSampler_);
        break;
    case ScreenSpaceDebugView::SsgiTemporal:
        if (features.ssgiRequired && ssgiHistory_.valid())
            debugInfo = sampled(ssgiHistory_, ssgiSampler_);
        break;
    case ScreenSpaceDebugView::SsgiFiltered:
    case ScreenSpaceDebugView::SsgiConfidence:
        if (features.ssgiRequired && ssgiFiltered_.valid())
            debugInfo = sampled(ssgiFiltered_, ssgiSampler_);
        break;
    case ScreenSpaceDebugView::SsgiVariance:
    case ScreenSpaceDebugView::SsgiRejection:
        if (features.ssgiRequired && ssgiDebug_.valid())
            debugInfo = sampled(ssgiDebug_, ssgiSampler_);
        break;
    case ScreenSpaceDebugView::None:
        break;
    }

    std::array<VkDescriptorImageInfo, 2> primaryInfos = {primary, bloomInfo};
    std::array<VkWriteDescriptorSet, 2> primaryWrites{};
    for (uint32_t index = 0; index < primaryWrites.size(); ++index) {
        primaryWrites[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        primaryWrites[index].dstSet = sourceDescriptorSets_.at(frameIndex);
        primaryWrites[index].dstBinding = index;
        primaryWrites[index].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        primaryWrites[index].descriptorCount = 1;
        primaryWrites[index].pImageInfo = &primaryInfos[index];
    }
    vkUpdateDescriptorSets(device_->logicalDevice(),
                           static_cast<uint32_t>(primaryWrites.size()),
                           primaryWrites.data(), 0, nullptr);

    std::array<VkDescriptorImageInfo, 3> infos = {
        normalInfo, motionInfo, debugInfo};
    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t index = 0; index < writes.size(); ++index) {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = sourceDescriptorSets_.at(frameIndex);
        writes[index].dstBinding = index + 2u;
        writes[index].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[index].descriptorCount = 1;
        writes[index].pImageInfo = &infos[index];
    }
    vkUpdateDescriptorSets(device_->logicalDevice(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

} // namespace vkr
