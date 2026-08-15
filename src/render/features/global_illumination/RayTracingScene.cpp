#include "render/features/global_illumination/RayTracingScene.h"

#include "core/AccelerationStructure.h"
#include "core/Device.h"
#include "core/GpuBarrier.h"
#include "render/material/MaterialInstance.h"
#include "render/geometry/Mesh.h"
#include "render/features/shadows_visibility/Visibility.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace vkr {
namespace {

uint32_t nextCapacity(uint32_t required) {
    uint32_t capacity = 16;
    while (capacity < required &&
           capacity <= std::numeric_limits<uint32_t>::max() / 2) {
        capacity *= 2;
    }
    return std::max(capacity, required);
}

VkTransformMatrixKHR toVulkanTransform(const glm::mat4 &matrix) {
    VkTransformMatrixKHR result{};
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            const float value = matrix[column][row];
            result.matrix[row][column] =
                std::isfinite(value) ? value : (row == column ? 1.0f : 0.0f);
        }
    }
    return result;
}

glm::uvec2 splitAddress(VkDeviceAddress address) {
    return {static_cast<uint32_t>(address),
            static_cast<uint32_t>(address >> 32u)};
}

} // namespace

RayTracingScene::RayTracingScene(Device &device) : device_(&device) {
    status_.supported = device.rayQuerySupport().available;
    status_.unavailableReason = device.rayQuerySupport().reason;
}

RayTracingScene::~RayTracingScene() = default;

void RayTracingScene::prepareFrame(uint32_t frameIndex,
                                   const VisibilityFrame &visibility) {
    if (!status_.supported)
        return;
    uint32_t count = 0;
    for (const RenderItem &item : visibility.items) {
        if (!item.mesh || !item.mesh->bottomLevelAccelerationStructure())
            continue;
        if (item.material) {
            const MaterialParams &params = item.material->params();
            if (params.alphaMode == AlphaMode::Blend ||
                params.transmissionFactor > 0.0f)
                continue;
        }
        ++count;
    }
    preparedCounts_.at(frameIndex) = count;
    ensureCapacity(frameIndex, std::max(count, 1u));
}

void RayTracingScene::ensureCapacity(uint32_t frameIndex,
                                     uint32_t required) {
    FrameStorage &storage = frames_.at(frameIndex);
    if (required <= storage.capacity && storage.topLevel)
        return;
    const uint32_t capacity = nextCapacity(std::max(required, 1u));
    const std::string root =
        "RayTracing/TLAS/Frame" + std::to_string(frameIndex);
    storage.topLevel.reset();
    storage.scratch.reset();
    storage.metadata.reset();
    storage.instances.reset();

    storage.instances = std::make_unique<Buffer>(
        *device_, sizeof(VkAccelerationStructureInstanceKHR) * capacity,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VMA_ALLOCATION_CREATE_MAPPED_BIT, root + "/Instances");
    storage.instances->map();
    storage.metadata = std::make_unique<Buffer>(
        *device_, sizeof(GpuRayTracingInstance) * capacity,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VMA_ALLOCATION_CREATE_MAPPED_BIT, root + "/Metadata");
    storage.metadata->map();

    VkAccelerationStructureGeometryInstancesDataKHR instances{};
    instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instances.data.deviceAddress = storage.instances->deviceAddress();
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances = instances;
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags =
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    device_->accelerationStructureBuildSizes(buildInfo, capacity, sizes);
    storage.topLevel = std::make_unique<AccelerationStructure>(
        *device_, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        sizes.accelerationStructureSize, root);

    const VkDeviceSize alignment = std::max<VkDeviceSize>(
        device_->rayQuerySupport().minScratchAlignment, 1);
    storage.scratch = std::make_unique<Buffer>(
        *device_, sizes.buildScratchSize + alignment - 1,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, root + "/Scratch");
    storage.capacity = capacity;
}

void RayTracingScene::build(VkCommandBuffer commandBuffer,
                            uint32_t frameIndex,
                            const VisibilityFrame &visibility) {
    if (!status_.supported)
        return;
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    std::vector<GpuRayTracingInstance> metadata;
    instances.reserve(visibility.items.size());
    metadata.reserve(visibility.items.size());
    for (uint32_t itemIndex = 0;
         itemIndex < visibility.items.size(); ++itemIndex) {
        const RenderItem &item = visibility.items[itemIndex];
        if (!item.mesh || itemIndex > 0x00ffffffu)
            continue;
        if (item.material) {
            const MaterialParams &params = item.material->params();
            if (params.alphaMode == AlphaMode::Blend ||
                params.transmissionFactor > 0.0f) {
                continue;
            }
        }
        const AccelerationStructure *bottomLevel =
            item.mesh->bottomLevelAccelerationStructure();
        if (!bottomLevel || bottomLevel->deviceAddress() == 0)
            continue;
        VkAccelerationStructureInstanceKHR instance{};
        instance.transform = toVulkanTransform(item.world);
        instance.instanceCustomIndex =
            static_cast<uint32_t>(metadata.size());
        instance.mask = 0xff;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.flags =
            item.material && item.material->params().doubleSided
                ? VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR
                : 0;
        instance.accelerationStructureReference =
            bottomLevel->deviceAddress();
        instances.push_back(instance);
        GpuRayTracingInstance gpu{};
        const glm::uvec2 vertexAddress =
            splitAddress(item.mesh->vertexDeviceAddress());
        const glm::uvec2 indexAddress =
            splitAddress(item.mesh->indexDeviceAddress());
        gpu.addresses = glm::uvec4(vertexAddress, indexAddress);
        if (item.material) {
            const MaterialParams &params = item.material->params();
            gpu.baseColorFactor = params.baseColorFactor;
            gpu.emissiveMetallic = glm::vec4(
                params.emissiveFactor * params.emissiveStrength,
                params.metallicFactor);
            gpu.materialParams = glm::vec4(
                params.roughnessFactor,
                params.doubleSided ? 1.0f : 0.0f,
                params.transmissionFactor,
                static_cast<float>(params.alphaMode));
        }
        metadata.push_back(gpu);
    }

    FrameStorage &storage = frames_.at(frameIndex);
    if (instances.empty()) {
        storage.count = 0;
        status_.active = false;
        status_.instanceCount = 0;
        refreshStatus();
        return;
    }
    if (instances.size() > device_->rayQuerySupport().maxInstanceCount) {
        throw std::runtime_error(
            "Ray tracing scene exceeds the device TLAS instance limit");
    }
    ensureCapacity(frameIndex, static_cast<uint32_t>(instances.size()));
    std::memcpy(storage.instances->mappedData(), instances.data(),
                instances.size() * sizeof(instances.front()));
    std::memcpy(storage.metadata->mappedData(), metadata.data(),
                metadata.size() * sizeof(metadata.front()));

    VkMemoryBarrier hostBarrier{};
    hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hostBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT |
                                VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    hostBarrier.dstAccessMask =
        VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    cmdPipelineBarrier2Compat(
        commandBuffer,
        VK_PIPELINE_STAGE_HOST_BIT |
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1,
        &hostBarrier, 0, nullptr, 0, nullptr);

    VkAccelerationStructureGeometryInstancesDataKHR instanceData{};
    instanceData.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instanceData.data.deviceAddress = storage.instances->deviceAddress();
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances = instanceData;
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags =
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.dstAccelerationStructure = storage.topLevel->handle();
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;
    const VkDeviceSize alignment = std::max<VkDeviceSize>(
        device_->rayQuerySupport().minScratchAlignment, 1);
    const VkDeviceAddress scratchBase = storage.scratch->deviceAddress();
    buildInfo.scratchData.deviceAddress =
        (scratchBase + alignment - 1) & ~(alignment - 1);
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = static_cast<uint32_t>(instances.size());
    device_->cmdBuildAccelerationStructures(commandBuffer, buildInfo, range);
    storage.count = range.primitiveCount;
    status_.active = true;
    status_.instanceCount = storage.count;
    refreshStatus();
}

VkAccelerationStructureKHR
RayTracingScene::handle(uint32_t frameIndex) const {
    const FrameStorage &storage = frames_.at(frameIndex);
    return storage.count > 0 && storage.topLevel
               ? storage.topLevel->handle()
               : VK_NULL_HANDLE;
}

VkAccelerationStructureKHR
RayTracingScene::allocatedHandle(uint32_t frameIndex) const {
    const FrameStorage &storage = frames_.at(frameIndex);
    return storage.topLevel ? storage.topLevel->handle() : VK_NULL_HANDLE;
}

VkBuffer RayTracingScene::metadataBuffer(uint32_t frameIndex) const {
    const FrameStorage &storage = frames_.at(frameIndex);
    return storage.count > 0 && storage.metadata
               ? storage.metadata->handle()
               : VK_NULL_HANDLE;
}

VkBuffer RayTracingScene::allocatedMetadataBuffer(uint32_t frameIndex) const {
    const FrameStorage &storage = frames_.at(frameIndex);
    return storage.metadata ? storage.metadata->handle() : VK_NULL_HANDLE;
}

uint32_t RayTracingScene::preparedInstanceCount(uint32_t frameIndex) const {
    return preparedCounts_.at(frameIndex);
}

uint32_t RayTracingScene::instanceCount(uint32_t frameIndex) const {
    return frames_.at(frameIndex).count;
}

void RayTracingScene::refreshStatus() {
    status_.allocatedBytes = 0;
    for (uint32_t index = 0; index < frames_.size(); ++index) {
        const FrameStorage &storage = frames_[index];
        status_.frameCapacities[index] = storage.capacity;
        if (storage.instances)
            status_.allocatedBytes += storage.instances->size();
        if (storage.metadata)
            status_.allocatedBytes += storage.metadata->size();
        if (storage.scratch)
            status_.allocatedBytes += storage.scratch->size();
        if (storage.topLevel)
            status_.allocatedBytes += storage.topLevel->size();
    }
}

} // namespace vkr
