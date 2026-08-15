#include "AtmosphereLutPass.h"

#include "core/ComputePipeline.h"
#include "core/ComputePipelineConfig.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/VulkanCheck.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"
#include "render/PipelineCache.h"
#include "render/RenderFrame.h"
#include "render/RenderGraph.h"
#include "render/RenderResourceRegistry.h"
#include "render/RenderView.h"

#include <array>
#include <stdexcept>
#include <utility>

namespace vkr {
namespace {

uint32_t dispatchCount(uint32_t extent, uint32_t workgroupSize) {
    return (extent + workgroupSize - 1u) / workgroupSize;
}

} // namespace

AtmosphereLutPass::AtmosphereLutPass(
    Device &device, const RenderResourceRegistry &resources,
    RendererResourceHandles resourceHandles,
    DescriptorAllocator &descriptorAllocator,
    VkDescriptorSetLayout atmosphereDescriptorSetLayout,
    std::string transmittanceShaderPath,
    std::string multipleScatteringShaderPath,
    std::string skyViewShaderPath,
    std::string aerialPerspectiveShaderPath)
    : device_(&device), resourceHandles_(resourceHandles),
      descriptorAllocator_(&descriptorAllocator),
      atmosphereDescriptorSetLayout_(atmosphereDescriptorSetLayout),
      transmittanceShaderPath_(std::move(transmittanceShaderPath)),
      multipleScatteringShaderPath_(std::move(multipleScatteringShaderPath)),
      skyViewShaderPath_(std::move(skyViewShaderPath)),
      aerialPerspectiveShaderPath_(std::move(aerialPerspectiveShaderPath)) {
    if (!device.atmosphereSupport().available)
        throw std::invalid_argument(
            "AtmosphereLutPass requires atmosphere compute support");
    createStorageDescriptorLayout();
    createStorageDescriptors(resources);
    status_.supported = true;
}

AtmosphereLutPass::~AtmosphereLutPass() {
    freeStorageDescriptors();
    if (storageDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     storageDescriptorSetLayout_, nullptr);
    }
}

void AtmosphereLutPass::setup(
    RenderGraphBuilder &builder,
    const RenderGraphBuildContext &) const {
    builder.addNode("Atmosphere/Transmittance", RgPassType::Compute,
                    RgQueueClass::Compute, 0);
    builder.useImage(
        {resourceHandles_.atmosphereTransmittance,
         RenderImageAccess::StorageWrite, VK_IMAGE_LAYOUT_GENERAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});

    builder.addNode("Atmosphere/MultipleScattering", RgPassType::Compute,
                    RgQueueClass::Compute, 1);
    builder.useImage(
        {resourceHandles_.atmosphereTransmittance,
         RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    builder.useImage(
        {resourceHandles_.atmosphereMultipleScattering,
         RenderImageAccess::StorageWrite, VK_IMAGE_LAYOUT_GENERAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});

    builder.addNode("Atmosphere/SkyView", RgPassType::Compute,
                    RgQueueClass::Compute, 2);
    builder.useImage(
        {resourceHandles_.atmosphereTransmittance,
         RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    builder.useImage(
        {resourceHandles_.atmosphereMultipleScattering,
         RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    builder.useImage(
        {resourceHandles_.atmosphereSkyView,
         RenderImageAccess::StorageWrite, VK_IMAGE_LAYOUT_GENERAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});

    builder.addNode("Atmosphere/AerialPerspective", RgPassType::Compute,
                    RgQueueClass::Compute, 3);
    builder.useImage(
        {resourceHandles_.atmosphereTransmittance,
         RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    builder.useImage(
        {resourceHandles_.atmosphereMultipleScattering,
         RenderImageAccess::SampledRead,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    builder.useImage(
        {resourceHandles_.atmosphereAerialPerspective,
         RenderImageAccess::StorageWrite, VK_IMAGE_LAYOUT_GENERAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
}

void AtmosphereLutPass::recordNode(
    RenderGraphPassContext &context, uint32_t localNodeIndex,
    const VisibilityFrame &) {
    if (localNodeIndex == 0)
        updateFrameState(context.frame);
    recordStage(context.frame, context.resources, localNodeIndex);
}

void AtmosphereLutPass::updateFrameState(
    const RenderFrameContext &frame) {
    status_.componentPresent =
        frame.view && frame.view->atmosphere.componentPresent;
    status_.active = frame.view && frame.view->atmosphere.active;
    buildStaticThisFrame_ = false;
    if (!frame.view)
        return;
    status_.componentEntity = frame.view->atmosphere.componentEntity;
    status_.sunEntity = frame.view->atmosphere.sunEntity;
    status_.sunBufferIndex = frame.view->atmosphere.sunBufferIndex;
    status_.cameraAltitudeKm = frame.view->atmosphere.cameraAltitudeKm;
    if (!frame.view->atmosphere.active)
        return;
    const uint64_t requestedKey = frame.view->atmosphere.staticLutKey;
    const auto now = std::chrono::steady_clock::now();
    if (requestedKey == currentStaticLutKey_)
        return;
    status_.staticLutDirty = true;
    if (pendingStaticLutKey_ != requestedKey) {
        pendingStaticLutKey_ = requestedKey;
        pendingSince_ = now;
    }
    if (now - pendingSince_ >= std::chrono::milliseconds(100)) {
        buildStaticThisFrame_ = true;
        staticBuildStarted_ = now;
    }
}

void AtmosphereLutPass::recordStage(
    const RenderFrameContext &frame,
    const RenderResourceRegistry &resources, uint32_t stage) {
    if (!frame.pipelineCache || !frame.view ||
        !frame.view->atmosphere.active || stage > 3)
        return;
    if (stage < 2 && !buildStaticThisFrame_)
        return;
    if (stage >= 2 && !readyFor(frame.view->atmosphere.staticLutKey))
        return;
    switch (stage) {
    case 0:
        dispatch(frame, "Pipeline/Atmosphere/Transmittance",
                 transmittanceShaderPath_, transmittanceStorageSet_,
                 resources.extent(
                     resourceHandles_.atmosphereTransmittance),
                 1);
        break;
    case 1:
        dispatch(frame, "Pipeline/Atmosphere/MultipleScattering",
                 multipleScatteringShaderPath_,
                 multipleScatteringStorageSet_,
                 resources.extent(
                     resourceHandles_.atmosphereMultipleScattering),
                 1);
        currentStaticLutKey_ = frame.view->atmosphere.staticLutKey;
        pendingStaticLutKey_ = 0;
        status_.staticLutReady = true;
        status_.staticLutDirty = false;
        ++status_.lutGeneration;
        status_.lastUpdateMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - staticBuildStarted_)
                .count();
        break;
    case 2:
        dispatch(frame, "Pipeline/Atmosphere/SkyView", skyViewShaderPath_,
                 skyViewStorageSets_.at(frame.frameIndex),
                 resources.extent(resourceHandles_.atmosphereSkyView), 1);
        break;
    case 3:
        dispatch(
            frame, "Pipeline/Atmosphere/AerialPerspective",
            aerialPerspectiveShaderPath_,
            aerialStorageSets_.at(frame.frameIndex),
            resources.extent(
                resourceHandles_.atmosphereAerialPerspective),
            32);
        break;
    }
}

bool AtmosphereLutPass::readyFor(uint64_t staticLutKey) const {
    return status_.staticLutReady && currentStaticLutKey_ == staticLutKey;
}

void AtmosphereLutPass::createStorageDescriptorLayout() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 1;
    info.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr,
                                         &storageDescriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, storageDescriptorSetLayout_,
        "Pass/Atmosphere/StorageDescriptorSetLayout");
}

void AtmosphereLutPass::createStorageDescriptors(
    const RenderResourceRegistry &resources) {
    const auto create = [&](RenderImageHandle handle, uint32_t frameIndex,
                            std::string name) {
        VkDescriptorSet set = descriptorAllocator_->allocate(
            storageDescriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}}, name);
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageView = resources.image(handle, frameIndex).imageView();
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device_->logicalDevice(), 1, &write, 0,
                               nullptr);
        return set;
    };
    transmittanceStorageSet_ = create(
        resourceHandles_.atmosphereTransmittance, 0,
        "Pass/Atmosphere/TransmittanceStorageSet");
    multipleScatteringStorageSet_ = create(
        resourceHandles_.atmosphereMultipleScattering, 0,
        "Pass/Atmosphere/MultipleScatteringStorageSet");
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        skyViewStorageSets_[frame] = create(
            resourceHandles_.atmosphereSkyView, frame,
            "Pass/Atmosphere/SkyViewStorageSet/Frame" +
                std::to_string(frame));
        aerialStorageSets_[frame] = create(
            resourceHandles_.atmosphereAerialPerspective, frame,
            "Pass/Atmosphere/AerialStorageSet/Frame" +
                std::to_string(frame));
    }
}

void AtmosphereLutPass::freeStorageDescriptors() {
    const auto free = [this](VkDescriptorSet &set) {
        if (set != VK_NULL_HANDLE)
            descriptorAllocator_->free(set);
        set = VK_NULL_HANDLE;
    };
    free(transmittanceStorageSet_);
    free(multipleScatteringStorageSet_);
    for (VkDescriptorSet &set : skyViewStorageSets_)
        free(set);
    for (VkDescriptorSet &set : aerialStorageSets_)
        free(set);
}

void AtmosphereLutPass::dispatch(
    const RenderFrameContext &frame, std::string_view debugName,
    const std::string &shaderPath, VkDescriptorSet storageSet,
    VkExtent2D extent, uint32_t layers) const {
    ComputePipelineConfig config{};
    config.debugName = std::string(debugName);
    config.computeShaderPath = shaderPath;
    config.descriptorLayouts = {atmosphereDescriptorSetLayout_,
                                storageDescriptorSetLayout_};
    ComputePipeline &pipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(config));
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.handle());
    const std::array<VkDescriptorSet, 2> sets = {
        frame.atmosphereDescriptorSet, storageSet};
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 0,
                            static_cast<uint32_t>(sets.size()), sets.data(),
                            0, nullptr);
    const uint32_t workgroupSize = layers > 1 ? 4u : 8u;
    vkCmdDispatch(frame.cmd, dispatchCount(extent.width, workgroupSize),
                  dispatchCount(extent.height, workgroupSize),
                  dispatchCount(layers, workgroupSize));
}

} // namespace vkr
