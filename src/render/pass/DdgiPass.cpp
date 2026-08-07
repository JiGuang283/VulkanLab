#include "render/pass/DdgiPass.h"

#include "core/Buffer.h"
#include "core/ComputePipeline.h"
#include "core/ComputePipelineConfig.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuBarrier.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/UploadContext.h"
#include "core/VulkanCheck.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"
#include "render/PipelineCache.h"
#include "render/RayTracingScene.h"
#include "render/RenderFrame.h"
#include "render/RenderView.h"
#include "render/Visibility.h"

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
                   const RenderResourceRegistry &resources,
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

std::vector<RenderImageUsage> DdgiPass::resourceUsages() const {
    if (!status_.supported)
        return {};
    return {
        {handles_.ddgiIrradiance, RenderImageAccess::StorageReadWrite,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {handles_.ddgiDistance, RenderImageAccess::StorageReadWrite,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
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
    const RenderResourceRegistry &resources) {
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
         {handles_.ddgiIrradiance, handles_.ddgiDistance}) {
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
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
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
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
    }
    upload.finish();
}

void DdgiPass::disableSampling(uint32_t frameIndex) {
    if (frameIndex >= frames_.size() || !frames_[frameIndex].parameters)
        return;
    std::memset(frames_[frameIndex].parameters->mappedData(), 0,
                sizeof(DdgiGpuParams));
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
    const RenderResourceRegistry &resources) {
    const VkSampler sampler = resources.sampler(handles_.ddgiSampler);
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        samplingSets_[frame] = descriptorAllocator_->allocate(
            samplingDescriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
             {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
             {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}},
            "DDGI/Frame" + std::to_string(frame) + "/SamplingSet");
        VkDescriptorBufferInfo params{frames_[frame].parameters->handle(), 0,
                                      sizeof(DdgiGpuParams)};
        VkDescriptorBufferInfo states{probeStates_->handle(), 0,
                                      VK_WHOLE_SIZE};
        std::array<VkDescriptorImageInfo, 2> images{};
        images[0] = {sampler,
                     resources.image(handles_.ddgiIrradiance, 0).imageView(),
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        images[1] = {sampler,
                     resources.image(handles_.ddgiDistance, 0).imageView(),
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        std::array<VkWriteDescriptorSet, 4> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     samplingSets_[frame], 0, 0, 1,
                     VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &params,
                     nullptr};
        for (uint32_t index = 0; index < 2; ++index) {
            writes[index + 1] = {
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                samplingSets_[frame], index + 1, 0, 1,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[index],
                nullptr, nullptr};
        }
        writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     samplingSets_[frame], 3, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &states,
                     nullptr};
        vkUpdateDescriptorSets(device_->logicalDevice(),
                               static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

void DdgiPass::updateComputeDescriptor(
    uint32_t frameIndex, const RenderResourceRegistry &resources) {
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
    VkAccelerationStructureKHR tlas = rayTracingScene_->handle(frameIndex);
    VkWriteDescriptorSetAccelerationStructureKHR acceleration{};
    acceleration.sType =
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    acceleration.accelerationStructureCount = 1;
    acceleration.pAccelerationStructures = &tlas;
    VkDescriptorBufferInfo metadata{
        rayTracingScene_->metadataBuffer(frameIndex), 0, VK_WHOLE_SIZE};
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
                           const RenderResourceRegistry &resources) {
    vkCmdFillBuffer(cmd, probeStates_->handle(), 0, VK_WHOLE_SIZE, 0);
    for (RenderImageHandle handle :
         {handles_.ddgiIrradiance, handles_.ddgiDistance}) {
        const Image &image = resources.image(handle, 0);
        const RenderImageDesc &desc = resources.description(handle);
        cmdImageBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_SHADER_READ_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, image.handle(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                         desc.arrayLayers});
        VkClearColorValue clear{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                      desc.arrayLayers};
        vkCmdClearColorImage(cmd, image.handle(),
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear,
                             1, &range);
        cmdImageBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_SHADER_WRITE_BIT,
                        image.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL, range);
    }
    updateCursor_ = 0;
    ++status_.generation;
    ++status_.resetCount;
    resetPending_ = false;
}

void DdgiPass::execute(const RenderFrameContext &frame,
                       const RenderResourceRegistry &resources,
                       const VisibilityFrame &visibility) {
    status_.componentPresent = frame.view &&
                               frame.view->ddgi.componentPresent;
    status_.componentEntity = status_.componentPresent
                                  ? frame.view->ddgi.componentEntity
                                  : PersistentEntityId{};
    status_.probeCount = status_.componentPresent
                             ? frame.view->ddgi.probeCount
                             : 0u;
    status_.raysPerProbe = status_.componentPresent
                               ? frame.view->ddgi.parameters.raysPerProbe
                               : 0u;
    status_.probesUpdatedPerFrame =
        status_.componentPresent
            ? std::min(frame.view->ddgi.parameters.probesUpdatedPerFrame,
                       frame.view->ddgi.probeCount)
            : 0u;
    if (!status_.componentPresent)
        status_.updateCursor = 0;
    status_.active = false;
    if (!status_.supported || !frame.features.ddgiRequired || !frame.view ||
        !frame.pipelineCache ||
        rayTracingScene_->handle(frame.frameIndex) == VK_NULL_HANDLE) {
        disableSampling(frame.frameIndex);
        status_.tracedInstanceCount = 0;
        return;
    }

    VKL_PROFILE_ZONE("Record DDGI");
    VKL_PROFILE_GPU_ZONE(*frame.tracyProfiler, frame.cmd, "DDGI");
    const DdgiFrameData &volume = frame.view->ddgi;
    const auto &settings = volume.parameters;
    const uint64_t signature =
        volumeSignature(volume, visibility.history.sceneGeneration);
    if (volumeSignature_ != signature) {
        volumeEntity_ = volume.componentEntity;
        volumeSignature_ = signature;
        resetPending_ = true;
    }
    const uint32_t updateCount = std::min(
        settings.probesUpdatedPerFrame, volume.probeCount);
    const uint32_t rayCount = updateCount * settings.raysPerProbe;
    ensureRayCapacity(frame.frameIndex, rayCount);

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
        glm::uvec4(settings.raysPerProbe, updateCount,
                   settings.relocationEnabled ? 1u : 0u,
                   settings.classificationEnabled ? 1u : 0u);
    gpu.updateWindow =
        glm::uvec4(updateCursor_, updateCount,
                   static_cast<uint32_t>(frame.submissionSerial),
                   static_cast<uint32_t>(frame.view->settings.ddgi.debugView));
    gpu.traceParameters =
        glm::vec4(frame.view->settings.ddgi.radianceClamp,
                  resetPending_ ? 1.0f : 0.0f, 0.0f, 0.0f);
    std::memcpy(frames_[frame.frameIndex].parameters->mappedData(), &gpu,
                sizeof(gpu));

    const bool resetThisFrame = resetPending_;
    if (resetPending_)
        resetVolume(frame.cmd, resources);
    else {
        for (RenderImageHandle handle :
             {handles_.ddgiIrradiance, handles_.ddgiDistance}) {
            const Image &image = resources.image(handle, 0);
            const RenderImageDesc &desc = resources.description(handle);
            cmdImageBarrier(
                frame.cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                image.handle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL,
                {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, desc.arrayLayers});
        }
    }
    if (resetThisFrame) {
        gpu.traceParameters.y = 1.0f;
        std::memcpy(frames_[frame.frameIndex].parameters->mappedData(), &gpu,
                    sizeof(gpu));
    }
    updateComputeDescriptor(frame.frameIndex, resources);

    VkMemoryBarrier hostAndAs{};
    hostAndAs.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hostAndAs.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT |
                              VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                              VK_ACCESS_TRANSFER_WRITE_BIT;
    hostAndAs.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                              VK_ACCESS_SHADER_WRITE_BIT |
                              VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(
        frame.cmd,
        VK_PIPELINE_STAGE_HOST_BIT |
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
            VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &hostAndAs, 0, nullptr,
        0, nullptr);

    ComputePipelineConfig traceConfig{};
    traceConfig.debugName = "Pipeline/DDGI/Trace";
    traceConfig.computeShaderPath = traceShaderPath_;
    traceConfig.descriptorLayouts = {globalDescriptorSetLayout_,
                                     computeDescriptorSetLayout_};
    ComputePipelineConfig updateConfig = traceConfig;
    updateConfig.debugName = "Pipeline/DDGI/Update";
    updateConfig.computeShaderPath = updateShaderPath_;
    ComputePipeline &trace =
        frame.pipelineCache->getOrCreateCompute(std::move(traceConfig));
    ComputePipeline &update =
        frame.pipelineCache->getOrCreateCompute(std::move(updateConfig));
    const VkDescriptorSet computeSet = frames_[frame.frameIndex].computeSet;
    const auto bind = [&](ComputePipeline &pipeline) {
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipeline.handle());
        const std::array<VkDescriptorSet, 2> sets = {
            frame.globalDescriptorSet, computeSet};
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline.layout(), 0,
                                static_cast<uint32_t>(sets.size()),
                                sets.data(), 0, nullptr);
    };
    {
        ScopedGpuLabel label(device_->debugUtils(), frame.cmd,
                             "Trace Probe Rays");
        bind(trace);
        vkCmdDispatch(frame.cmd,
                      (settings.raysPerProbe + 63u) / 64u,
                      updateCount, 1);
    }
    VkMemoryBarrier rayBarrier{};
    rayBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    rayBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    rayBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(frame.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &rayBarrier, 0, nullptr, 0, nullptr);
    {
        ScopedGpuLabel label(device_->debugUtils(), frame.cmd,
                             "Update Probe Atlases");
        bind(update);
        vkCmdDispatch(frame.cmd, 1, 1, updateCount);
    }
    for (RenderImageHandle handle :
         {handles_.ddgiIrradiance, handles_.ddgiDistance}) {
        const Image &image = resources.image(handle, 0);
        const RenderImageDesc &desc = resources.description(handle);
        cmdImageBarrier(
            frame.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, image.handle(),
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, desc.arrayLayers});
    }
    VkMemoryBarrier stateBarrier{};
    stateBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    stateBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    stateBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(frame.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                         &stateBarrier, 0, nullptr, 0, nullptr);

    updateCursor_ = (updateCursor_ + updateCount) % volume.probeCount;
    status_.active = true;
    status_.probeCount = volume.probeCount;
    status_.raysPerProbe = settings.raysPerProbe;
    status_.probesUpdatedPerFrame = updateCount;
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
