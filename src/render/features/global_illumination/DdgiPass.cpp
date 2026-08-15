#include "render/features/global_illumination/DdgiPass.h"

#include "core/Buffer.h"
#include "render/pipeline/ComputePipeline.h"
#include "render/pipeline/ComputePipelineConfig.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuBarrier.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/UploadContext.h"
#include "core/VulkanCheck.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"
#include "render/pipeline/PipelineCache.h"
#include "render/features/global_illumination/RayTracingScene.h"
#include "render/frame/RenderFrame.h"
#include "render/graph/RenderGraph.h"
#include "render/frame/RenderView.h"
#include "render/features/shadows_visibility/Visibility.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace vkr {
namespace {

uint32_t nextCapacity(uint32_t required) {
    uint32_t result = 256;
    while (result < required)
        result *= 2;
    return result;
}

uint64_t hashBytes(uint64_t seed, const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t index = 0; index < size; ++index) {
        seed ^= bytes[index];
        seed *= 1099511628211ull;
    }
    return seed;
}

uint64_t volumeSignature(const DdgiFrameData &volume,
                         uint64_t sceneGeneration) {
    uint64_t result = 1469598103934665603ull;
    result = hashBytes(result, &sceneGeneration, sizeof(sceneGeneration));
    result = hashBytes(result, &volume.componentEntity,
                       sizeof(volume.componentEntity));
    result = hashBytes(result, &volume.localToWorld,
                       sizeof(volume.localToWorld));
    result = hashBytes(result, &volume.parameters.probeCounts,
                       sizeof(volume.parameters.probeCounts));
    result = hashBytes(result, &volume.parameters.probeSpacing,
                       sizeof(volume.parameters.probeSpacing));
    result = hashBytes(result, &volume.parameters.maxRayDistance,
                       sizeof(volume.parameters.maxRayDistance));
    result = hashBytes(result, &volume.parameters.relocationEnabled,
                       sizeof(volume.parameters.relocationEnabled));
    result = hashBytes(result, &volume.parameters.classificationEnabled,
                       sizeof(volume.parameters.classificationEnabled));
    return result;
}

} // namespace

DdgiPass::DdgiPass(Device &device,
                   const RenderResourcePool &resources,
                   RendererResourceHandles handles,
                   DescriptorAllocator &descriptorAllocator,
                   VkDescriptorSetLayout globalDescriptorSetLayout,
                   RayTracingScene &rayTracingScene,
                   std::string traceShaderPath,
                   std::string updateShaderPath)
    : device_(&device), handles_(handles),
      descriptorAllocator_(&descriptorAllocator),
      globalDescriptorSetLayout_(globalDescriptorSetLayout),
      rayTracingScene_(&rayTracingScene),
      traceShaderPath_(std::move(traceShaderPath)),
      updateShaderPath_(std::move(updateShaderPath)) {
    status_.supported = device.ddgiSupport().available;
    status_.unavailableReason = device.ddgiSupport().reason;
    createDescriptorLayouts();
    createPersistentResources(resources);
    createSamplingDescriptors(resources);
}

DdgiPass::~DdgiPass() {
    freeDescriptors();
    probeStates_.reset();
    if (computeDescriptorSetLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     computeDescriptorSetLayout_, nullptr);
    if (samplingDescriptorSetLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     samplingDescriptorSetLayout_, nullptr);
}

void DdgiPass::prepareGraph(const RenderFrameContext &frame,
                            const RenderResourcePool &resources,
                            const VisibilityFrame &visibility) {
    status_.componentPresent = frame.view && frame.view->ddgi.componentPresent;
    status_.componentEntity = status_.componentPresent
                                  ? frame.view->ddgi.componentEntity
                                  : PersistentEntityId{};
    status_.probeCount = status_.componentPresent
                             ? frame.view->ddgi.probeCount
                             : 0u;
    status_.raysPerProbe = status_.componentPresent
                               ? frame.view->ddgi.parameters.raysPerProbe
                               : 0u;
    preparedActive_ = status_.supported && frame.features.ddgiRequired &&
                      frame.view && frame.pipelineCache &&
                      rayTracingScene_->preparedInstanceCount(
                          frame.frameIndex) > 0;
    status_.active = false;
    if (!preparedActive_) {
        disableSampling(frame.frameIndex, resources);
        status_.tracedInstanceCount = 0;
        preparedUpdateCount_ = 0;
        preparedRayCount_ = 0;
        return;
    }

    const DdgiFrameData &volume = frame.view->ddgi;
    if (resources.residency(handles_.ddgiIrradiance) ==
            RenderResourceResidency::Unallocated ||
        resources.residency(handles_.ddgiDistance) ==
            RenderResourceResidency::Unallocated) {
        resetPending_ = true;
    }
    const auto &settings = volume.parameters;
    const uint64_t signature =
        volumeSignature(volume, visibility.history.sceneGeneration);
    if (volumeSignature_ != signature) {
        volumeEntity_ = volume.componentEntity;
        volumeSignature_ = signature;
        resetPending_ = true;
    }
    preparedFrameIndex_ = frame.frameIndex;
    preparedUpdateCount_ =
        std::min(settings.probesUpdatedPerFrame, volume.probeCount);
    preparedRayCount_ = preparedUpdateCount_ * settings.raysPerProbe;
    ensureRayCapacity(frame.frameIndex, preparedRayCount_);
    preparedReset_ = resetPending_;

    DdgiGpuParams gpu{};
    gpu.localToWorld = volume.localToWorld;
    gpu.worldToLocal = volume.worldToLocal;
    gpu.probeCounts = glm::uvec4(settings.probeCounts, volume.probeCount);
    gpu.probeSpacingMaxDistance =
        glm::vec4(settings.probeSpacing, settings.maxRayDistance);
    gpu.updateParameters =
        glm::vec4(settings.hysteresis, settings.normalBias,
                  settings.viewBias, settings.intensity);
    gpu.runtimeParameters =
        glm::uvec4(settings.raysPerProbe, preparedUpdateCount_,
                   settings.relocationEnabled ? 1u : 0u,
                   settings.classificationEnabled ? 1u : 0u);
    gpu.updateWindow =
        glm::uvec4(updateCursor_, preparedUpdateCount_,
                   static_cast<uint32_t>(frame.submissionSerial),
                   static_cast<uint32_t>(frame.view->settings.ddgi.debugView));
    gpu.traceParameters =
        glm::vec4(frame.view->settings.ddgi.radianceClamp,
                  preparedReset_ ? 1.0f : 0.0f, 0.0f, 0.0f);
    std::memcpy(frames_[frame.frameIndex].parameters->mappedData(), &gpu,
                sizeof(gpu));
}

void DdgiPass::prepareFrame(const RenderFrameContext &frame,
                            const RenderResourcePool &resources,
                            const VisibilityFrame &) {
    if (preparedActive_ && preparedFrameIndex_ == frame.frameIndex) {
        updateComputeDescriptor(frame.frameIndex, resources);
        updateSamplingDescriptor(frame.frameIndex, resources, true);
    }
}

uint64_t DdgiPass::topologySignature() const {
    uint64_t value = preparedActive_ ? 1u : 0u;
    value ^= preparedReset_ ? 0x100u : 0u;
    if (preparedActive_) {
        value ^= reinterpret_cast<uint64_t>(
            frames_[preparedFrameIndex_].rayResults->handle());
        value ^= reinterpret_cast<uint64_t>(probeStates_->handle()) << 1u;
        value ^= reinterpret_cast<uint64_t>(
            rayTracingScene_->allocatedMetadataBuffer(preparedFrameIndex_))
                 << 2u;
    }
    return value;
}

void DdgiPass::setup(RenderGraphBuilder &builder,
                     const RenderGraphBuildContext &) const {
    if (preparedReset_) {
        builder.addNode("DDGI/Reset", RgPassType::Transfer,
                        RgQueueClass::Transfer, 0);
        builder.setActive(preparedActive_);
        builder.useBuffer(probeStates_->handle(),
                          RgBufferAccess::TransferWrite);
        for (RenderImageHandle handle :
             {handles_.ddgiIrradiance, handles_.ddgiDistance}) {
            builder.useImage({handle, RenderImageAccess::TransferWrite,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_GENERAL});
        }
    }

    builder.addNode("DDGI/Trace", RgPassType::Compute,
                    RgQueueClass::Compute, 1);
    builder.setActive(preparedActive_);
    if (preparedActive_) {
        const FrameStorage &storage = frames_[preparedFrameIndex_];
        builder.useBuffer(
            rayTracingScene_->allocatedMetadataBuffer(preparedFrameIndex_),
            RgBufferAccess::StorageRead, 0, VK_WHOLE_SIZE,
            preparedFrameIndex_);
        builder.useBuffer(storage.rayResults->handle(),
                          RgBufferAccess::StorageWrite, 0, VK_WHOLE_SIZE,
                          preparedFrameIndex_);
        builder.useBuffer(probeStates_->handle(), RgBufferAccess::StorageRead);
        builder.useBuffer(storage.parameters->handle(),
                          RgBufferAccess::UniformRead, 0,
                          sizeof(DdgiGpuParams), preparedFrameIndex_);
    }

    builder.addNode("DDGI/Update", RgPassType::Compute,
                    RgQueueClass::Compute, 2);
    builder.setActive(preparedActive_);
    if (preparedActive_) {
        const FrameStorage &storage = frames_[preparedFrameIndex_];
        builder.useBuffer(storage.rayResults->handle(),
                          RgBufferAccess::StorageRead, 0, VK_WHOLE_SIZE,
                          preparedFrameIndex_);
        builder.useBuffer(probeStates_->handle(),
                          RgBufferAccess::StorageReadWrite);
        builder.useBuffer(storage.parameters->handle(),
                          RgBufferAccess::UniformRead, 0,
                          sizeof(DdgiGpuParams), preparedFrameIndex_);
        for (RenderImageHandle handle :
             {handles_.ddgiIrradiance, handles_.ddgiDistance}) {
            builder.useImage({handle, RenderImageAccess::StorageReadWrite,
                              VK_IMAGE_LAYOUT_GENERAL,
                              VK_IMAGE_LAYOUT_GENERAL});
        }
    }

    builder.addNode("DDGI/Finalize", RgPassType::Compute,
                    RgQueueClass::Compute, 3);
    builder.setActive(preparedActive_);
    if (preparedActive_) {
        builder.useBuffer(probeStates_->handle(), RgBufferAccess::StorageRead);
        for (RenderImageHandle handle :
             {handles_.ddgiIrradiance, handles_.ddgiDistance}) {
            builder.useImage({handle, RenderImageAccess::SampledRead,
                              VK_IMAGE_LAYOUT_GENERAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        }
    }
}

void DdgiPass::recordNode(RenderGraphPassContext &context,
                          uint32_t localNodeIndex,
                          const VisibilityFrame &) {
    switch (localNodeIndex) {
    case 0: resetVolume(context.frame.cmd, context.resources); break;
    case 1: recordTrace(context.frame); break;
    case 2: recordUpdate(context.frame); break;
    case 3: finishFrame(context.frame); break;
    default: break;
    }
}

void DdgiPass::createDescriptorLayouts() {
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    std::array<VkDescriptorSetLayoutBinding, 4> sampling{};
    sampling[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    sampling[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    sampling[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    sampling[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    info.bindingCount = static_cast<uint32_t>(sampling.size());
    info.pBindings = sampling.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr,
                                         &samplingDescriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, samplingDescriptorSetLayout_,
        "DDGI/SamplingDescriptorSetLayout");
    if (!status_.supported)
        return;

    std::array<VkDescriptorSetLayoutBinding, 7> compute{};
    const std::array<VkDescriptorType, 7> computeTypes = {
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    for (uint32_t index = 0; index < compute.size(); ++index) {
        compute[index].binding = index;
        compute[index].descriptorType = computeTypes[index];
        compute[index].descriptorCount = 1;
        compute[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    info.bindingCount = static_cast<uint32_t>(compute.size());
    info.pBindings = compute.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr,
                                         &computeDescriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, computeDescriptorSetLayout_,
        "DDGI/ComputeDescriptorSetLayout");
}

void DdgiPass::createPersistentResources(
    const RenderResourcePool &resources) {
    probeStates_ = std::make_unique<Buffer>(
        *device_, sizeof(GpuDdgiProbeState) * kMaxDdgiProbes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, "DDGI/ProbeStates");
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        FrameStorage &storage = frames_[frame];
        storage.parameters = std::make_unique<Buffer>(
            *device_, sizeof(DdgiGpuParams),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
            "DDGI/Frame" + std::to_string(frame) + "/Parameters");
        storage.parameters->map();
        std::memset(storage.parameters->mappedData(), 0,
                    sizeof(DdgiGpuParams));
        if (status_.supported)
            ensureRayCapacity(frame, 256);
    }

    UploadContext upload(*device_, nullptr, 64 * 1024,
                         "DDGI/Initialize");
    VkCommandBuffer cmd = upload.commandBuffer();
    vkCmdFillBuffer(cmd, probeStates_->handle(), 0, VK_WHOLE_SIZE, 0);
    for (RenderImageHandle handle :
         {handles_.ddgiIrradianceFallback,
          handles_.ddgiDistanceFallback}) {
        const Image &image = resources.image(handle, 0);
        const RenderImageDesc &desc = resources.description(handle);
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.handle();
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                    desc.arrayLayers};
        cmdPipelineBarrier2Compat(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &barrier);
        VkClearColorValue clear{};
        vkCmdClearColorImage(cmd, image.handle(),
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear,
                             1, &barrier.subresourceRange);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cmdPipelineBarrier2Compat(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
    }
    upload.finish();
}

void DdgiPass::disableSampling(
    uint32_t frameIndex, const RenderResourcePool &resources) {
    if (frameIndex >= frames_.size() || !frames_[frameIndex].parameters)
        return;
    std::memset(frames_[frameIndex].parameters->mappedData(), 0,
                sizeof(DdgiGpuParams));
    updateSamplingDescriptor(frameIndex, resources, false);
    status_.active = false;
}

void DdgiPass::ensureRayCapacity(uint32_t frameIndex, uint32_t required) {
    FrameStorage &storage = frames_.at(frameIndex);
    if (storage.rayResults && storage.rayCapacity >= required)
        return;
    if (storage.computeSet != VK_NULL_HANDLE) {
        descriptorAllocator_->free(storage.computeSet);
        storage.computeSet = VK_NULL_HANDLE;
    }
    storage.rayCapacity = nextCapacity(std::max(required, 1u));
    storage.rayResults = std::make_unique<Buffer>(
        *device_, sizeof(GpuDdgiRayResult) * storage.rayCapacity,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
        "DDGI/Frame" + std::to_string(frameIndex) + "/RayResults");
}

void DdgiPass::createSamplingDescriptors(
    const RenderResourcePool &resources) {
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        samplingSets_[frame] = descriptorAllocator_->allocate(
            samplingDescriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
             {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
             {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}},
            "DDGI/Frame" + std::to_string(frame) + "/SamplingSet");
        updateSamplingDescriptor(frame, resources, false);
    }
}

void DdgiPass::updateSamplingDescriptor(
    uint32_t frameIndex, const RenderResourcePool &resources,
    bool active) {
    const VkSampler sampler = resources.sampler(handles_.ddgiSampler);
    const RenderImageHandle irradiance =
        active ? handles_.ddgiIrradiance
               : handles_.ddgiIrradianceFallback;
    const RenderImageHandle distance =
        active ? handles_.ddgiDistance : handles_.ddgiDistanceFallback;
    VkDescriptorBufferInfo params{frames_[frameIndex].parameters->handle(), 0,
                                  sizeof(DdgiGpuParams)};
    VkDescriptorBufferInfo states{probeStates_->handle(), 0, VK_WHOLE_SIZE};
    std::array<VkDescriptorImageInfo, 2> images = {{
        {sampler, resources.image(irradiance, 0).imageView(),
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler, resources.image(distance, 0).imageView(),
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
    std::array<VkWriteDescriptorSet, 4> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                 samplingSets_[frameIndex], 0, 0, 1,
                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &params,
                 nullptr};
    for (uint32_t index = 0; index < images.size(); ++index) {
        writes[index + 1] = {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            samplingSets_[frameIndex], index + 1, 0, 1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[index],
            nullptr, nullptr};
    }
    writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                 samplingSets_[frameIndex], 3, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &states,
                 nullptr};
    vkUpdateDescriptorSets(device_->logicalDevice(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

void DdgiPass::updateComputeDescriptor(
    uint32_t frameIndex, const RenderResourcePool &resources) {
    FrameStorage &storage = frames_.at(frameIndex);
    if (storage.computeSet == VK_NULL_HANDLE) {
        storage.computeSet = descriptorAllocator_->allocate(
            computeDescriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
             {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
             {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
             {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}},
            "DDGI/Frame" + std::to_string(frameIndex) + "/ComputeSet");
    }
    VkAccelerationStructureKHR tlas =
        rayTracingScene_->allocatedHandle(frameIndex);
    VkWriteDescriptorSetAccelerationStructureKHR acceleration{};
    acceleration.sType =
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    acceleration.accelerationStructureCount = 1;
    acceleration.pAccelerationStructures = &tlas;
    VkDescriptorBufferInfo metadata{
        rayTracingScene_->allocatedMetadataBuffer(frameIndex), 0,
        VK_WHOLE_SIZE};
    VkDescriptorBufferInfo rays{storage.rayResults->handle(), 0,
                                VK_WHOLE_SIZE};
    VkDescriptorBufferInfo states{probeStates_->handle(), 0,
                                  VK_WHOLE_SIZE};
    VkDescriptorBufferInfo params{storage.parameters->handle(), 0,
                                  sizeof(DdgiGpuParams)};
    std::array<VkDescriptorImageInfo, 2> images{};
    images[0] = {VK_NULL_HANDLE,
                 resources.image(handles_.ddgiIrradiance, 0).imageView(),
                 VK_IMAGE_LAYOUT_GENERAL};
    images[1] = {VK_NULL_HANDLE,
                 resources.image(handles_.ddgiDistance, 0).imageView(),
                 VK_IMAGE_LAYOUT_GENERAL};
    std::array<VkWriteDescriptorSet, 7> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &acceleration,
                 storage.computeSet, 0, 0, 1,
                 VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr,
                 nullptr, nullptr};
    const std::array<VkDescriptorBufferInfo *, 4> buffers = {
        &metadata, &rays, &states, &params};
    for (uint32_t index = 0; index < 2; ++index) {
        writes[index + 1] = {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            storage.computeSet, index + 1, 0, 1,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, buffers[index],
            nullptr};
    }
    for (uint32_t index = 0; index < 2; ++index) {
        writes[index + 3] = {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            storage.computeSet, index + 3, 0, 1,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &images[index], nullptr,
            nullptr};
    }
    writes[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                 storage.computeSet, 5, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &states,
                 nullptr};
    writes[6] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                 storage.computeSet, 6, 0, 1,
                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &params,
                 nullptr};
    vkUpdateDescriptorSets(device_->logicalDevice(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

void DdgiPass::resetVolume(VkCommandBuffer cmd,
                           const RenderResourcePool &resources) {
    vkCmdFillBuffer(cmd, probeStates_->handle(), 0, VK_WHOLE_SIZE, 0);
    for (RenderImageHandle handle :
         {handles_.ddgiIrradiance, handles_.ddgiDistance}) {
        const Image &image = resources.image(handle, 0);
        const RenderImageDesc &desc = resources.description(handle);
        VkClearColorValue clear{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                      desc.arrayLayers};
        vkCmdClearColorImage(cmd, image.handle(),
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear,
                             1, &range);
    }
    updateCursor_ = 0;
    ++status_.generation;
    ++status_.resetCount;
    resetPending_ = false;
}

void DdgiPass::recordTrace(const RenderFrameContext &frame) {
    if (!preparedActive_ || !frame.pipelineCache || !frame.view)
        return;
    VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memory.srcStageMask =
        VK_PIPELINE_STAGE_2_HOST_BIT |
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    memory.srcAccessMask =
        VK_ACCESS_2_HOST_WRITE_BIT |
        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    memory.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    memory.dstAccessMask =
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &memory;
    vkCmdPipelineBarrier2(frame.cmd, &dependency);

    ComputePipelineConfig config{};
    config.debugName = "Pipeline/DDGI/Trace";
    config.computeShaderPath = traceShaderPath_;
    config.descriptorLayouts = {globalDescriptorSetLayout_,
                                computeDescriptorSetLayout_};
    ComputePipeline &pipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(config));
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.handle());
    const std::array<VkDescriptorSet, 2> sets = {
        frame.globalDescriptorSet,
        frames_[frame.frameIndex].computeSet};
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 0,
                            static_cast<uint32_t>(sets.size()), sets.data(),
                            0, nullptr);
    const uint32_t raysPerProbe =
        frame.view->ddgi.parameters.raysPerProbe;
    vkCmdDispatch(frame.cmd, (raysPerProbe + 63u) / 64u,
                  preparedUpdateCount_, 1);
}

void DdgiPass::recordUpdate(const RenderFrameContext &frame) {
    if (!preparedActive_ || !frame.pipelineCache)
        return;
    ComputePipelineConfig config{};
    config.debugName = "Pipeline/DDGI/Update";
    config.computeShaderPath = updateShaderPath_;
    config.descriptorLayouts = {globalDescriptorSetLayout_,
                                computeDescriptorSetLayout_};
    ComputePipeline &pipeline =
        frame.pipelineCache->getOrCreateCompute(std::move(config));
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.handle());
    const std::array<VkDescriptorSet, 2> sets = {
        frame.globalDescriptorSet,
        frames_[frame.frameIndex].computeSet};
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.layout(), 0,
                            static_cast<uint32_t>(sets.size()), sets.data(),
                            0, nullptr);
    vkCmdDispatch(frame.cmd, 1, 1, preparedUpdateCount_);
}

void DdgiPass::finishFrame(const RenderFrameContext &frame) {
    if (!preparedActive_ || !frame.view)
        return;
    updateCursor_ = (updateCursor_ + preparedUpdateCount_) %
                    frame.view->ddgi.probeCount;
    status_.active = true;
    status_.probeCount = frame.view->ddgi.probeCount;
    status_.raysPerProbe = frame.view->ddgi.parameters.raysPerProbe;
    status_.probesUpdatedPerFrame = preparedUpdateCount_;
    status_.updateCursor = updateCursor_;
    status_.tracedInstanceCount =
        rayTracingScene_->instanceCount(frame.frameIndex);
    status_.allocatedBytes = probeStates_->size();
    for (const FrameStorage &storage : frames_) {
        status_.allocatedBytes += storage.parameters->size();
        status_.allocatedBytes += storage.rayResults->size();
    }
    status_.allocatedBytes +=
        static_cast<uint64_t>(8u * 8u * kMaxDdgiProbes * 8u) +
        static_cast<uint64_t>(16u * 16u * kMaxDdgiProbes * 4u);
    preparedReset_ = false;
}

void DdgiPass::freeDescriptors() {
    if (!descriptorAllocator_)
        return;
    for (VkDescriptorSet &set : samplingSets_) {
        if (set != VK_NULL_HANDLE)
            descriptorAllocator_->free(set);
        set = VK_NULL_HANDLE;
    }
    for (FrameStorage &storage : frames_) {
        if (storage.computeSet != VK_NULL_HANDLE)
            descriptorAllocator_->free(storage.computeSet);
        storage.computeSet = VK_NULL_HANDLE;
    }
}

} // namespace vkr
