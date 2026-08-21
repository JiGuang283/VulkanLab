#include "render/features/lighting/ClusteredLighting.h"

#include "core/Buffer.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/VulkanCheck.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace vkr {
namespace {

uint32_t divideRoundUp(uint32_t value, uint32_t divisor) {
    return (value + divisor - 1u) / divisor;
}

uint64_t lightIndexBytes(const ClusterGridConfig &grid) {
    return static_cast<uint64_t>(grid.clusterCount) *
           grid.maxLightsPerCluster * sizeof(uint32_t);
}

void hashCombine(uint64_t &seed, uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
}

} // namespace

struct ClusteredLightingResources::FrameStorage {
    std::unique_ptr<Buffer> clusterCounts;
    std::unique_ptr<Buffer> lightIndices;
    std::unique_ptr<Buffer> statistics;
    uint64_t submittedSerial = 0;
    bool active = false;
};

ClusteredLightingResources::ClusteredLightingResources(
    Device &device, DescriptorAllocator &descriptorAllocator,
    VkExtent2D viewportExtent)
    : device_(&device), descriptorAllocator_(&descriptorAllocator) {
    status_.supported = device.graphicsQueueSupportsCompute();
    if (!status_.supported) {
        status_.unavailableReason =
            "selected graphics queue does not support compute";
        return;
    }
    createDescriptorSetLayout();
    for (auto &frame : frames_)
        frame = std::make_unique<FrameStorage>();
    recreateBuffers(viewportExtent);
}

ClusteredLightingResources::~ClusteredLightingResources() {
    freeDescriptorSets();
    for (auto &frame : frames_)
        frame.reset();
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     descriptorSetLayout_, nullptr);
    }
}

ClusterGridConfig ClusteredLightingResources::makeGrid(
    VkExtent2D extent, float nearPlane, float farPlane) {
    ClusterGridConfig grid{};
    grid.viewport = {std::max(extent.width, 1u),
                     std::max(extent.height, 1u)};
    grid.nearPlane = std::max(nearPlane, 1.0e-3f);
    grid.farPlane = std::max(farPlane, grid.nearPlane + 1.0e-3f);

    for (const uint32_t tileSize : {16u, 32u, 64u}) {
        grid.tileSize = tileSize;
        grid.tilesX = divideRoundUp(grid.viewport.width, tileSize);
        grid.tilesY = divideRoundUp(grid.viewport.height, tileSize);
        const uint64_t clusterCount =
            static_cast<uint64_t>(grid.tilesX) * grid.tilesY *
            grid.depthSlices;
        if (clusterCount > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("cluster grid exceeds uint32 capacity");
        grid.clusterCount = static_cast<uint32_t>(clusterCount);
        if (lightIndexBytes(grid) <= kClusterLightIndexBudgetBytes ||
            tileSize == 64u) {
            break;
        }
    }

    const float logarithmicRange =
        std::log2(grid.farPlane / grid.nearPlane);
    grid.logScale = logarithmicRange > 1.0e-6f
                        ? static_cast<float>(grid.depthSlices) /
                              logarithmicRange
                        : 0.0f;
    grid.logBias = -std::log2(grid.nearPlane) * grid.logScale;
    return grid;
}

void ClusteredLightingResources::resize(VkExtent2D viewportExtent) {
    if (!status_.supported)
        return;
    if (grid_.viewport.width == std::max(viewportExtent.width, 1u) &&
        grid_.viewport.height == std::max(viewportExtent.height, 1u)) {
        return;
    }
    recreateBuffers(viewportExtent);
}

void ClusteredLightingResources::setSceneLightBuffer(uint32_t frameIndex,
                                                       VkBuffer buffer) {
    sceneLightBuffers_.at(frameIndex) = buffer;
}

void ClusteredLightingResources::prepareFrame(
    uint32_t frameIndex, uint64_t frameSerial, bool active,
    uint32_t punctualLightCount, float nearPlane, float farPlane) {
    if (!status_.supported)
        return;
    FrameStorage &storage = *frames_.at(frameIndex);
    if (storage.submittedSerial != 0 && storage.statistics) {
        storage.statistics->invalidate();
        const auto *statistics = static_cast<const GpuClusterStatistics *>(
            storage.statistics->mappedData());
        status_.nonEmptyClusters = statistics->nonEmptyClusters;
        status_.totalLightReferences = statistics->totalLightReferences;
        status_.overflowClusters = statistics->overflowClusters;
        status_.overflowLightReferences =
            statistics->overflowLightReferences;
        status_.maxLightReferences = statistics->maxLightReferences;
        status_.averageLightReferences =
            statistics->nonEmptyClusters > 0
                ? static_cast<float>(statistics->totalLightReferences) /
                      static_cast<float>(statistics->nonEmptyClusters)
                : 0.0f;
        status_.completedFrameSerial = storage.submittedSerial;
    }

    const ClusterGridConfig updated = makeGrid(
        grid_.viewport, nearPlane, farPlane);
    grid_.nearPlane = updated.nearPlane;
    grid_.farPlane = updated.farPlane;
    grid_.logScale = updated.logScale;
    grid_.logBias = updated.logBias;

    storage.active = active;
    storage.submittedSerial = active ? frameSerial : 0;
    status_.active = active;
    status_.punctualLightCount = punctualLightCount;
}

bool ClusteredLightingResources::active(uint32_t frameIndex) const {
    return status_.supported && frames_.at(frameIndex)->active;
}

VkBuffer ClusteredLightingResources::clusterCountBuffer(
    uint32_t frameIndex) const {
    const auto &buffer = frames_.at(frameIndex)->clusterCounts;
    return buffer ? buffer->handle() : VK_NULL_HANDLE;
}

VkBuffer ClusteredLightingResources::lightIndexBuffer(
    uint32_t frameIndex) const {
    const auto &buffer = frames_.at(frameIndex)->lightIndices;
    return buffer ? buffer->handle() : VK_NULL_HANDLE;
}

VkBuffer ClusteredLightingResources::statisticsBuffer(
    uint32_t frameIndex) const {
    const auto &buffer = frames_.at(frameIndex)->statistics;
    return buffer ? buffer->handle() : VK_NULL_HANDLE;
}

uint64_t ClusteredLightingResources::topologySignature() const {
    uint64_t signature = 1469598103934665603ull;
    hashCombine(signature, grid_.tileSize);
    hashCombine(signature, grid_.clusterCount);
    for (uint32_t frame = 0; frame < frames_.size(); ++frame) {
        hashCombine(signature,
                    reinterpret_cast<uint64_t>(clusterCountBuffer(frame)));
        hashCombine(signature,
                    reinterpret_cast<uint64_t>(lightIndexBuffer(frame)));
        hashCombine(signature,
                    reinterpret_cast<uint64_t>(statisticsBuffer(frame)));
        hashCombine(signature,
                    reinterpret_cast<uint64_t>(sceneLightBuffer(frame)));
    }
    return signature;
}

void ClusteredLightingResources::createDescriptorSetLayout() {
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    for (uint32_t binding = 0; binding < bindings.size(); ++binding) {
        bindings[binding] = {
            binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            nullptr};
    }
    VkDescriptorSetLayoutCreateInfo info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                         nullptr, &descriptorSetLayout_));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, descriptorSetLayout_,
        "Lighting/ClusteredDescriptorSetLayout");
}

void ClusteredLightingResources::recreateBuffers(VkExtent2D extent) {
    freeDescriptorSets();
    grid_ = makeGrid(extent, grid_.nearPlane, grid_.farPlane);
    const VkDeviceSize countBytes =
        static_cast<VkDeviceSize>(grid_.clusterCount) * sizeof(uint32_t);
    const VkDeviceSize indexBytes =
        static_cast<VkDeviceSize>(lightIndexBytes(grid_));
    constexpr VmaAllocationCreateFlags hostFlags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT;

    status_.allocatedBytes = 0;
    for (uint32_t frameIndex = 0; frameIndex < frames_.size(); ++frameIndex) {
        FrameStorage &frame = *frames_[frameIndex];
        frame.clusterCounts = std::make_unique<Buffer>(
            *device_, countBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
            "Lighting/Clusters/Frame" + std::to_string(frameIndex) +
                "/Counts");
        frame.lightIndices = std::make_unique<Buffer>(
            *device_, indexBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
            "Lighting/Clusters/Frame" + std::to_string(frameIndex) +
                "/LightIndices");
        frame.statistics = std::make_unique<Buffer>(
            *device_, sizeof(GpuClusterStatistics),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            hostFlags,
            "Lighting/Clusters/Frame" + std::to_string(frameIndex) +
                "/Statistics");
        frame.statistics->map();
        std::memset(frame.statistics->mappedData(), 0,
                    sizeof(GpuClusterStatistics));
        frame.submittedSerial = 0;
        frame.active = false;
        status_.allocatedBytes += static_cast<uint64_t>(countBytes) +
                                  static_cast<uint64_t>(indexBytes) +
                                  sizeof(GpuClusterStatistics);
    }
    allocateDescriptorSets();

    status_.viewport = grid_.viewport;
    status_.tileSize = grid_.tileSize;
    status_.tilesX = grid_.tilesX;
    status_.tilesY = grid_.tilesY;
    status_.depthSlices = grid_.depthSlices;
    status_.clusterCount = grid_.clusterCount;
    status_.maxLightsPerCluster = grid_.maxLightsPerCluster;
}

void ClusteredLightingResources::freeDescriptorSets() {
    for (VkDescriptorSet &set : descriptorSets_) {
        if (set != VK_NULL_HANDLE)
            descriptorAllocator_->free(set);
        set = VK_NULL_HANDLE;
    }
}

void ClusteredLightingResources::allocateDescriptorSets() {
    for (uint32_t frame = 0; frame < descriptorSets_.size(); ++frame) {
        descriptorSets_[frame] = descriptorAllocator_->allocate(
            descriptorSetLayout_,
            {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}},
            "Lighting/Clusters/Frame" + std::to_string(frame) +
                "/DescriptorSet");
        updateDescriptor(frame);
    }
}

void ClusteredLightingResources::updateDescriptor(uint32_t frameIndex) {
    const FrameStorage &frame = *frames_.at(frameIndex);
    const std::array<VkDescriptorBufferInfo, 3> infos{
        VkDescriptorBufferInfo{frame.clusterCounts->handle(), 0,
                               VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{frame.lightIndices->handle(), 0,
                               VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{frame.statistics->handle(), 0,
                               sizeof(GpuClusterStatistics)}};
    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t binding = 0; binding < writes.size(); ++binding) {
        writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[binding].dstSet = descriptorSets_.at(frameIndex);
        writes[binding].dstBinding = binding;
        writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding].descriptorCount = 1;
        writes[binding].pBufferInfo = &infos[binding];
    }
    vkUpdateDescriptorSets(device_->logicalDevice(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

} // namespace vkr
