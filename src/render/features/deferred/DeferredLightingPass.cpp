#include "render/features/deferred/DeferredLightingPass.h"

#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/VulkanCheck.h"
#include "render/features/deferred/DeferredLightingResources.h"
#include "render/features/lighting/ClusteredLighting.h"
#include "render/features/surface/GBufferResources.h"
#include "render/frame/RenderFrame.h"
#include "render/graph/RenderGraph.h"
#include "render/graph/RenderResourcePool.h"
#include "render/pipeline/ComputePipeline.h"
#include "render/pipeline/ComputePipelineConfig.h"
#include "render/pipeline/PipelineCache.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace vkr {

DeferredLightingPass::DeferredLightingPass(
    Device &device, const RenderResourcePool &resources,
    RendererResourceHandles resourceHandles,
    DescriptorAllocator &descriptorAllocator,
    VkDescriptorSetLayout globalLayout,
    VkDescriptorSetLayout lightingLayout,
    VkDescriptorSetLayout atmosphereLayout,
    VkDescriptorSetLayout screenSpaceLayout,
    VkDescriptorSetLayout ddgiLayout,
    ClusteredLightingResources &clusteredLighting,
    std::string shaderPath)
    : device_(&device), resources_(resourceHandles),
      descriptorAllocator_(&descriptorAllocator), globalLayout_(globalLayout),
      lightingLayout_(lightingLayout), atmosphereLayout_(atmosphereLayout),
      screenSpaceLayout_(screenSpaceLayout), ddgiLayout_(ddgiLayout),
      clusteredLighting_(&clusteredLighting),
      shaderPath_(std::move(shaderPath)) {
    status_.supported = device.gBufferSupport().available &&
                        device.computeBloomSupport().available &&
                        deferredLightingResources(resources_).valid();
    if (!device.gBufferSupport().available)
        status_.unavailableReason = device.gBufferSupport().reason;
    else if (!device.computeBloomSupport().available)
        status_.unavailableReason = device.computeBloomSupport().reason;
    createLayout();
    createDescriptors(resources);
}

DeferredLightingPass::~DeferredLightingPass() {
    freeDescriptors();
    if (localLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_->logicalDevice(), localLayout_,
                                     nullptr);
}

void DeferredLightingPass::setup(
    RenderGraphBuilder &builder,
    const RenderGraphBuildContext &context) const {
    builder.addNode("DeferredLighting/Compute", RgPassType::Compute,
                    RgQueueClass::Compute, 0);
    builder.setActive(context.features.deferredLightingRequired);

    const auto sampled = [&](RenderImageHandle handle,
                             VkImageLayout layout =
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        if (handle.valid())
            builder.useImage({handle, RenderImageAccess::SampledRead,
                              layout, layout});
    };
    const GBufferResources gbuffer = gBufferResources(resources_);
    sampled(gbuffer.baseColorMetallic);
    sampled(gbuffer.normalRoughnessOcclusion);
    sampled(gbuffer.emissiveSurfaceFlags);
    sampled(gbuffer.depth,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    sampled(resources_.hdrColor);

    if (context.features.directionalShadowRequired)
        sampled(resources_.directionalShadowDepth,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    if (context.features.pointShadowRequired) {
        const uint32_t capacity = std::clamp(
            context.features.pointShadowLightCount, 1u,
            static_cast<uint32_t>(
                resources_.pointShadowDepthByCapacity.size()));
        sampled(resources_.pointShadowDepthByCapacity[capacity - 1],
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    }
    if (context.features.spotShadowRequired) {
        const uint32_t capacity = std::clamp(
            context.features.spotShadowLightCount, 1u,
            static_cast<uint32_t>(
                resources_.spotShadowDepthByCapacity.size()));
        sampled(resources_.spotShadowDepthByCapacity[capacity - 1],
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    }
    if (context.features.atmosphereRequired) {
        sampled(resources_.atmosphereTransmittance);
        sampled(resources_.atmosphereAerialPerspective);
    }
    if (context.features.ddgiActive) {
        sampled(resources_.ddgiIrradiance);
        sampled(resources_.ddgiDistance);
    }
    if (context.features.ssaoActive)
        sampled(resources_.ssaoFiltered);
    else if (context.features.cacaoActive)
        sampled(resources_.cacaoOutput);
    else if (context.features.gtaoActive)
        sampled(resources_.gtaoFiltered);

    if (context.features.clusteredLightingRequired) {
        for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
            builder.useBuffer(
                clusteredLighting_->clusterCountBuffer(frame),
                RgBufferAccess::StorageRead, 0, VK_WHOLE_SIZE, frame);
            builder.useBuffer(
                clusteredLighting_->lightIndexBuffer(frame),
                RgBufferAccess::StorageRead, 0, VK_WHOLE_SIZE, frame);
        }
    }

    const DeferredLightingResources output =
        deferredLightingResources(resources_);
    for (RenderImageHandle image :
         {output.hdrColor, output.baselineDiffuse,
          output.baselineSpecular}) {
        builder.useImage({image, RenderImageAccess::StorageWrite,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
}

void DeferredLightingPass::recordNode(RenderGraphPassContext &context,
                                      uint32_t,
                                      const VisibilityFrame &) {
    const RenderFrameContext &frame = context.frame;
    status_.active = frame.features.deferredLightingRequired;
    if (!status_.active || !frame.pipelineCache ||
        !resourcesReady(context.resources)) {
        return;
    }
    ensureDescriptor(context.resources, frame.frameIndex);

    ComputePipelineConfig config{};
    config.debugName = "Pipeline/Deferred/Lighting";
    config.computeShaderPath = shaderPath_;
    config.descriptorLayouts = {
        globalLayout_, localLayout_, lightingLayout_, atmosphereLayout_,
        screenSpaceLayout_, ddgiLayout_,
        clusteredLighting_->descriptorSetLayout()};
    ComputePipeline &pipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(config));
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.handle());
    const std::array<VkDescriptorSet, 7> sets = {
        frame.globalDescriptorSet,
        descriptorSets_.at(frame.frameIndex),
        frame.lightingDescriptorSet,
        frame.atmosphereDescriptorSet,
        frame.screenSpaceDescriptorSet,
        frame.ddgiDescriptorSet,
        frame.clusteredLightingDescriptorSet};
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 0,
                            static_cast<uint32_t>(sets.size()), sets.data(),
                            0, nullptr);
    const DeferredLightingResources output =
        deferredLightingResources(resources_);
    status_.extent = context.resources.extent(output.hdrColor);
    status_.dispatchX = (status_.extent.width + 7u) / 8u;
    status_.dispatchY = (status_.extent.height + 7u) / 8u;
    vkCmdDispatch(frame.cmd, status_.dispatchX, status_.dispatchY, 1);
}

void DeferredLightingPass::releaseViewportResources() {
    freeDescriptors();
    status_.active = false;
}

void DeferredLightingPass::onViewportResize(
    const RenderResourcePool &resources) {
    createDescriptors(resources);
}

void DeferredLightingPass::onResourceResidencyChanged(
    const RenderResourcePool &resources, uint32_t frameIndex,
    const std::vector<RenderImageHandle> &) {
    ensureDescriptor(resources, frameIndex);
}

bool DeferredLightingPass::resourcesReady(
    const RenderResourcePool &resources) const {
    const GBufferResources gbuffer = gBufferResources(resources_);
    const DeferredLightingResources output =
        deferredLightingResources(resources_);
    return gbuffer.valid() && output.valid() &&
           resources.resident(gbuffer.depth) &&
           resources.resident(gbuffer.baseColorMetallic) &&
           resources.resident(gbuffer.normalRoughnessOcclusion) &&
           resources.resident(gbuffer.emissiveSurfaceFlags) &&
           resources.resident(resources_.hdrColor) &&
           resources.resident(output.hdrColor) &&
           resources.resident(output.baselineDiffuse) &&
           resources.resident(output.baselineSpecular);
}

void DeferredLightingPass::createLayout() {
    std::array<VkDescriptorSetLayoutBinding, 8> bindings{};
    for (uint32_t binding = 0; binding < 5; ++binding) {
        bindings[binding] = {
            binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    for (uint32_t binding = 5; binding < bindings.size(); ++binding) {
        bindings[binding] = {
            binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &localLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, localLayout_,
        "Deferred/LightingDescriptorSetLayout");
}

void DeferredLightingPass::createDescriptors(
    const RenderResourcePool &resources) {
    if (!status_.supported || !resourcesReady(resources))
        return;
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame)
        ensureDescriptor(resources, frame);
}

void DeferredLightingPass::ensureDescriptor(
    const RenderResourcePool &resources, uint32_t frame) {
    if (!status_.supported || !resourcesReady(resources) ||
        frame >= descriptorSets_.size()) {
        return;
    }
    if (descriptorSets_[frame] == VK_NULL_HANDLE) {
        descriptorSets_[frame] = descriptorAllocator_->allocate(
            localLayout_,
            {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5},
             {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3}},
            "Deferred/LightingDescriptorSet/Frame" +
                std::to_string(frame));
    }
    updateDescriptor(resources, frame);
}

void DeferredLightingPass::updateDescriptor(
    const RenderResourcePool &resources, uint32_t frame) {
    if (descriptorSets_[frame] == VK_NULL_HANDLE)
        return;
    const GBufferResources gbuffer = gBufferResources(resources_);
    const DeferredLightingResources output =
        deferredLightingResources(resources_);
    const VkSampler sampler = resources.sampler(resources_.hdrSampler);
    std::array<VkDescriptorImageInfo, 8> infos{};
    const std::array<RenderImageHandle, 5> sampled = {
        gbuffer.baseColorMetallic, gbuffer.normalRoughnessOcclusion,
        gbuffer.emissiveSurfaceFlags, gbuffer.depth, resources_.hdrColor};
    for (uint32_t binding = 0; binding < sampled.size(); ++binding) {
        infos[binding].sampler = sampler;
        infos[binding].imageView =
            resources.image(sampled[binding], frame).imageView();
        infos[binding].imageLayout =
            binding == 3
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    const std::array<RenderImageHandle, 3> storage = {
        output.hdrColor, output.baselineDiffuse, output.baselineSpecular};
    for (uint32_t index = 0; index < storage.size(); ++index) {
        infos[index + 5].imageView =
            resources.image(storage[index], frame).imageView();
        infos[index + 5].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    std::array<VkWriteDescriptorSet, 8> writes{};
    for (uint32_t binding = 0; binding < writes.size(); ++binding) {
        writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[binding].dstSet = descriptorSets_[frame];
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType =
            binding < 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                        : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[binding].pImageInfo = &infos[binding];
    }
    vkUpdateDescriptorSets(device_->logicalDevice(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    status_.residentBytes = resources.estimatedBytes(output.hdrColor) +
                            resources.estimatedBytes(output.baselineDiffuse) +
                            resources.estimatedBytes(output.baselineSpecular);
}

void DeferredLightingPass::freeDescriptors() {
    for (VkDescriptorSet &set : descriptorSets_) {
        if (set != VK_NULL_HANDLE)
            descriptorAllocator_->free(set);
        set = VK_NULL_HANDLE;
    }
}

} // namespace vkr
