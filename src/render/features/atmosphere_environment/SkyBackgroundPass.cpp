#include "SkyBackgroundPass.h"

#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "render/pipeline/Pipeline.h"
#include "render/pipeline/PipelineConfigBuilder.h"
#include "core/VulkanCheck.h"
#include "render/pipeline/PipelineCache.h"
#include "render/frame/RenderFrame.h"
#include "render/graph/RenderGraph.h"
#include "render/graph/RenderResourcePool.h"
#include "render/frame/RenderView.h"
#include "render/shader/ShaderTypes.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"

#include <utility>

namespace vkr {

namespace {

RenderImageHandle skyTarget(const FrameRenderFeatures &features,
                            const RendererResourceHandles &resources) {
    if (features.renderPath.active == RenderPathMode::Deferred)
        return resources.hdrColor;
    return resources.hdrMsaaColor.valid() ? resources.hdrMsaaColor
                                          : resources.hdrColor;
}

} // namespace

SkyBackgroundPass::SkyBackgroundPass(
    Device &device, const RenderResourcePool &,
    RendererResourceHandles resourceHandles,
    VkDescriptorSetLayout globalDescriptorSetLayout,
    VkDescriptorSetLayout lightingDescriptorSetLayout,
    VkDescriptorSetLayout atmosphereDescriptorSetLayout,
    std::string vertexShaderPath, std::string skyboxFragmentShaderPath,
    std::string atmosphereFragmentShaderPath)
    : device_(&device), resourceHandles_(resourceHandles),
      globalDescriptorSetLayout_(globalDescriptorSetLayout),
      lightingDescriptorSetLayout_(lightingDescriptorSetLayout),
      atmosphereDescriptorSetLayout_(atmosphereDescriptorSetLayout),
      vertexShaderPath_(std::move(vertexShaderPath)),
      skyboxFragmentShaderPath_(std::move(skyboxFragmentShaderPath)),
      atmosphereFragmentShaderPath_(
          std::move(atmosphereFragmentShaderPath)) {
    VkDescriptorSetLayoutCreateInfo emptyInfo{};
    emptyInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    VK_CHECK(vkCreateDescriptorSetLayout(
        device_->logicalDevice(), &emptyInfo, nullptr,
        &emptyDescriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
        emptyDescriptorSetLayout_, "Pass/SkyBackground/EmptySetLayout");
}

SkyBackgroundPass::~SkyBackgroundPass() {
    if (emptyDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     emptyDescriptorSetLayout_, nullptr);
    }
}

void SkyBackgroundPass::setup(
    RenderGraphBuilder &builder,
    const RenderGraphBuildContext &context) const {
    builder.addNode(std::string(name()), RgPassType::Graphics,
                    RgQueueClass::Graphics);
    if (context.features.atmosphereRequired) {
        builder.useImage({resourceHandles_.atmosphereTransmittance,
                          RenderImageAccess::SampledRead,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        builder.useImage({resourceHandles_.atmosphereSkyView,
                          RenderImageAccess::SampledRead,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
    const RenderImageHandle target =
        skyTarget(context.features, resourceHandles_);
    builder.addColorAttachment(
        target, RenderImageAccess::ColorAttachmentWrite,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
        VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}});
}

void SkyBackgroundPass::recordNode(RenderGraphPassContext &context, uint32_t,
                                   const VisibilityFrame &) {
    const RenderFrameContext &frame = context.frame;
    const RenderResourcePool &resources = context.resources;
    VKL_PROFILE_ZONE("Record Sky Background");
    VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd, "Sky Background");
    const RenderImageHandle target =
        skyTarget(frame.features, resourceHandles_);
    const VkExtent2D extent = resources.extent(target);
    const bool drawAtmosphere =
        frame.view && frame.viewMode && frame.pipelineCache &&
        frame.atmosphereReady && frame.view->atmosphere.active &&
        frame.viewMode->supportsAtmosphere;
    const bool drawSkybox =
        !drawAtmosphere && frame.environmentReady && frame.view &&
        frame.view->settings.skyboxEnabled && frame.pipelineCache;
    if (drawAtmosphere || drawSkybox) {
        PipelineConfig config =
            PipelineConfigBuilder{}
                .shaders(vertexShaderPath_,
                         drawAtmosphere ? atmosphereFragmentShaderPath_
                                        : skyboxFragmentShaderPath_)
                .vertexLayout({})
                .rasterization(VK_CULL_MODE_NONE,
                               VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .depth(false, false)
                .msaa(resources.description(target).samples)
                .descriptorLayout(globalDescriptorSetLayout_)
                .descriptorLayout(emptyDescriptorSetLayout_)
                .descriptorLayout(lightingDescriptorSetLayout_)
                .build();
        if (drawAtmosphere)
            config.descriptorLayouts.push_back(
                atmosphereDescriptorSetLayout_);
        config.debugName = drawAtmosphere ? "Pipeline/SkyBackground/Atmosphere"
                                          : "Pipeline/SkyBackground/Skybox";
        PipelineRenderingSignature signature{};
        signature.colorAttachmentFormats = {
            resources.description(target).format};
        signature.samples = resources.description(target).samples;
        Pipeline &pipeline = frame.pipelineCache->getOrCreate(
            std::move(signature), std::move(config));
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline.handle());
        vkCmdBindDescriptorSets(
            frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
            0, 1, &frame.globalDescriptorSet, 0, nullptr);
        vkCmdBindDescriptorSets(
            frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
            2, 1, &frame.lightingDescriptorSet, 0, nullptr);
        if (drawAtmosphere) {
            vkCmdBindDescriptorSets(
                frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline.layout(), 3, 1,
                &frame.atmosphereDescriptorSet, 0, nullptr);
        }
        VkViewport viewport{};
        viewport.width = static_cast<float>(extent.width);
        viewport.height = static_cast<float>(extent.height);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetScissor(frame.cmd, 0, 1, &scissor);
        vkCmdDraw(frame.cmd, 3, 1, 0, 0);
    }
}

} // namespace vkr
