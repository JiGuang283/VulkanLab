#pragma once

#include "core/FrameSync.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Buffer;
class DescriptorAllocator;
class Device;

inline constexpr uint32_t kClusterDepthSliceCount = 24;
inline constexpr uint32_t kMaxLightsPerCluster = 32;
inline constexpr uint32_t kClusteredLightingMinPunctualLights = 8;
inline constexpr uint64_t kClusterLightIndexBudgetBytes = 32ull * 1024ull * 1024ull;

struct ClusterGridConfig {
    VkExtent2D viewport{};
    uint32_t tileSize = 16;
    uint32_t tilesX = 0;
    uint32_t tilesY = 0;
    uint32_t depthSlices = kClusterDepthSliceCount;
    uint32_t maxLightsPerCluster = kMaxLightsPerCluster;
    uint32_t clusterCount = 0;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    float logScale = 0.0f;
    float logBias = 0.0f;
};

struct alignas(16) GpuClusterStatistics {
    uint32_t nonEmptyClusters = 0;
    uint32_t totalLightReferences = 0;
    uint32_t overflowClusters = 0;
    uint32_t maxLightReferences = 0;
    uint32_t testedPunctualLights = 0;
    uint32_t overflowLightReferences = 0;
    uint32_t reserved1 = 0;
    uint32_t reserved2 = 0;
};
static_assert(sizeof(GpuClusterStatistics) == 32);

struct ClusteredLightingStatus {
    bool supported = false;
    bool active = false;
    std::string unavailableReason;
    VkExtent2D viewport{};
    uint32_t tileSize = 0;
    uint32_t tilesX = 0;
    uint32_t tilesY = 0;
    uint32_t depthSlices = 0;
    uint32_t clusterCount = 0;
    uint32_t maxLightsPerCluster = 0;
    uint32_t punctualLightCount = 0;
    uint32_t nonEmptyClusters = 0;
    uint32_t totalLightReferences = 0;
    uint32_t overflowClusters = 0;
    uint32_t overflowLightReferences = 0;
    uint32_t maxLightReferences = 0;
    float averageLightReferences = 0.0f;
    uint64_t allocatedBytes = 0;
    uint64_t completedFrameSerial = 0;
};

class ClusteredLightingResources {
  public:
    ClusteredLightingResources(Device &device,
                               DescriptorAllocator &descriptorAllocator,
                               VkExtent2D viewportExtent);
    ~ClusteredLightingResources();

    ClusteredLightingResources(const ClusteredLightingResources &) = delete;
    ClusteredLightingResources &operator=(const ClusteredLightingResources &) = delete;

    void resize(VkExtent2D viewportExtent);
    void setSceneLightBuffer(uint32_t frameIndex, VkBuffer buffer);
    void prepareFrame(uint32_t frameIndex, uint64_t frameSerial,
                      bool active, uint32_t punctualLightCount,
                      float nearPlane, float farPlane);

    bool supported() const { return status_.supported; }
    bool active(uint32_t frameIndex) const;
    const ClusterGridConfig &grid() const { return grid_; }
    VkDescriptorSetLayout descriptorSetLayout() const {
        return descriptorSetLayout_;
    }
    VkDescriptorSet descriptorSet(uint32_t frameIndex) const {
        return descriptorSets_.at(frameIndex);
    }
    VkBuffer clusterCountBuffer(uint32_t frameIndex) const;
    VkBuffer lightIndexBuffer(uint32_t frameIndex) const;
    VkBuffer statisticsBuffer(uint32_t frameIndex) const;
    VkBuffer sceneLightBuffer(uint32_t frameIndex) const {
        return sceneLightBuffers_.at(frameIndex);
    }
    uint64_t topologySignature() const;
    const ClusteredLightingStatus &status() const { return status_; }

  private:
    struct FrameStorage;

    static ClusterGridConfig makeGrid(VkExtent2D extent, float nearPlane,
                                      float farPlane);
    void createDescriptorSetLayout();
    void recreateBuffers(VkExtent2D extent);
    void freeDescriptorSets();
    void allocateDescriptorSets();
    void updateDescriptor(uint32_t frameIndex);

    Device *device_ = nullptr;
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptorSets_{};
    std::array<std::unique_ptr<FrameStorage>, MAX_FRAMES_IN_FLIGHT> frames_{};
    std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> sceneLightBuffers_{};
    ClusterGridConfig grid_{};
    ClusteredLightingStatus status_{};
};

} // namespace vkr
