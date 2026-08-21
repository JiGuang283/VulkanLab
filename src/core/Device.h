#pragma once

#include <optional>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

#include "VulkanContext.h"
#include "VulkanTypes.h"
#include "diagnostics/SceneLoadStats.h"
#include "core/TextureTranscodeTarget.h"
#include "core/MaterialBindingMode.h"
#include "vk_mem_alloc.h"

namespace vkr {

class GpuDebugUtils;
class TracyProfiler;

struct ComputeBloomSupport {
    bool available = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::string reason;
};

struct AtmosphereSupport {
    bool available = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::string reason;
};

struct SurfaceDataSupport {
    bool available = false;
    bool albedoMetallicAvailable = false;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkFormat normalRoughnessFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat motionFormat = VK_FORMAT_R16G16_SFLOAT;
    VkFormat albedoMetallicFormat = VK_FORMAT_R8G8B8A8_UNORM;
    std::string reason;
    std::string albedoMetallicReason;
};

struct GBufferSupport {
    bool available = false;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkFormat baseColorMetallicFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat normalRoughnessOcclusionFormat =
        VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat emissiveSurfaceFlagsFormat =
        VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat motionFormat = VK_FORMAT_R16G16_SFLOAT;
    uint32_t requiredColorAttachments = 4;
    std::string reason;
};

struct OcclusionCullingSupport {
    bool available = false;
    VkFormat hiZFormat = VK_FORMAT_R32_SFLOAT;
    std::string reason;
};

enum class DepthHierarchyMode {
    Unavailable,
    SplitR32,
    CombinedMinMax,
};

inline const char *depthHierarchyModeName(DepthHierarchyMode mode) {
    switch (mode) {
    case DepthHierarchyMode::Unavailable:
        return "unavailable";
    case DepthHierarchyMode::SplitR32:
        return "split-r32";
    case DepthHierarchyMode::CombinedMinMax:
        return "combined-min-max";
    }
    return "unavailable";
}

struct DepthHierarchySupport {
    DepthHierarchyMode mode = DepthHierarchyMode::Unavailable;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::string reason;

    bool available() const { return mode != DepthHierarchyMode::Unavailable; }
    bool combined() const {
        return mode == DepthHierarchyMode::CombinedMinMax;
    }
};

struct ScreenSpaceEffectsSupport {
    bool depthPyramidAvailable = false;
    bool colorPyramidAvailable = false;
    bool ssaoAvailable = false;
    bool gtaoAvailable = false;
    bool taaAvailable = false;
    bool ssrAvailable = false;
    bool ssgiAvailable = false;
    VkFormat depthPyramidFormat = VK_FORMAT_R32_SFLOAT;
    VkFormat colorPyramidFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat ssaoFormat = VK_FORMAT_R16_SFLOAT;
    std::string depthPyramidReason;
    std::string colorPyramidReason;
    std::string ssaoReason;
    std::string gtaoReason;
    std::string taaReason;
    std::string ssrReason;
    std::string ssgiReason;
};

struct CacaoSupport {
    bool compiled = false;
    bool available = false;
    bool fp32 = true;
    VkFormat depthAdapterFormat = VK_FORMAT_R32_SFLOAT;
    VkFormat normalAdapterFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat outputFormat = VK_FORMAT_R32_SFLOAT;
    std::string reason;
};

struct RayQuerySupport {
    bool available = false;
    std::string reason;
    uint64_t maxGeometryCount = 0;
    uint64_t maxInstanceCount = 0;
    uint64_t maxPrimitiveCount = 0;
    VkDeviceSize minScratchAlignment = 1;
};

struct DdgiSupport {
    bool available = false;
    VkFormat irradianceFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat distanceFormat = VK_FORMAT_R16G16_SFLOAT;
    std::string reason;
};

struct MaterialBindingDeviceSupport {
    bool supported = false;
    uint32_t textureCapacity = 0;
    uint32_t materialCapacity = 0;
    std::string reason;
};

class Device {
  public:
    Device(VulkanContext &ctx,
           MaterialBindingMode materialBindingMode =
               MaterialBindingMode::Auto);
    ~Device();

    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    VkDevice              logicalDevice() const { return device_; }
    VkPhysicalDevice      physicalDevice() const { return physicalDevice_; }
    VkPhysicalDeviceProperties physicalDeviceProperties() const;
    VkQueue               graphicsQueue() const { return graphicsQueue_; }
    VkQueue               presentQueue() const { return presentQueue_; }
    QueueFamilyIndices    queueFamilies() const;
    VkSampleCountFlagBits msaaSamples() const { return msaaSamples_; }
    TextureTranscodeTarget textureTranscodeTarget() const {
        return textureTranscodeTarget_;
    }
    bool environmentIblSupported() const {
        return environmentIblSupported_;
    }
    const ComputeBloomSupport &computeBloomSupport() const {
        return computeBloomSupport_;
    }
    const AtmosphereSupport &atmosphereSupport() const {
        return atmosphereSupport_;
    }
    const SurfaceDataSupport &surfaceDataSupport() const {
        return surfaceDataSupport_;
    }
    const GBufferSupport &gBufferSupport() const { return gBufferSupport_; }
    const OcclusionCullingSupport &occlusionCullingSupport() const {
        return occlusionCullingSupport_;
    }
    const ScreenSpaceEffectsSupport &screenSpaceEffectsSupport() const {
        return screenSpaceEffectsSupport_;
    }
    const DepthHierarchySupport &depthHierarchySupport() const {
        return depthHierarchySupport_;
    }
    const CacaoSupport &cacaoSupport() const { return cacaoSupport_; }
    const RayQuerySupport &rayQuerySupport() const {
        return rayQuerySupport_;
    }
    const DdgiSupport &ddgiSupport() const { return ddgiSupport_; }
    const MaterialBindingDeviceSupport &materialBindingSupport() const {
        return materialBindingSupport_;
    }
    bool graphicsQueueSupportsCompute() const {
        return graphicsQueueSupportsCompute_;
    }

    VkDeviceAddress bufferDeviceAddress(VkBuffer buffer) const;
    VkAccelerationStructureKHR createAccelerationStructure(
        VkBuffer buffer, VkDeviceSize size,
        VkAccelerationStructureTypeKHR type,
        const std::string &debugName = {}) const;
    void destroyAccelerationStructure(
        VkAccelerationStructureKHR accelerationStructure) const;
    void accelerationStructureBuildSizes(
        VkAccelerationStructureBuildGeometryInfoKHR &buildInfo,
        uint32_t primitiveCount,
        VkAccelerationStructureBuildSizesInfoKHR &sizes) const;
    void cmdBuildAccelerationStructures(
        VkCommandBuffer commandBuffer,
        const VkAccelerationStructureBuildGeometryInfoKHR &buildInfo,
        const VkAccelerationStructureBuildRangeInfoKHR &range) const;
    VkDeviceAddress accelerationStructureDeviceAddress(
        VkAccelerationStructureKHR accelerationStructure) const;

    SwapChainSupportDetails querySwapChainSupport() const;

    VmaAllocator allocator() const { return allocator_; }
    AllocatorMemorySnapshot allocatorMemorySnapshot() const;
    GpuDebugUtils &debugUtils() { return *debugUtils_; }
    const GpuDebugUtils &debugUtils() const { return *debugUtils_; }
    TracyProfiler &tracyProfiler() { return *tracyProfiler_; }
    const TracyProfiler &tracyProfiler() const { return *tracyProfiler_; }

  private:
    void pickPhysicalDevice();
    void queryRayQuerySupport();
    void queryDdgiSupport();
    void queryMaterialBindingSupport();
    MaterialBindingDeviceSupport
    inspectMaterialBindingSupport(VkPhysicalDevice device) const;
    void createLogicalDevice();

    bool               isDeviceSuitable(VkPhysicalDevice device);
    bool               checkDeviceExtensionSupport(VkPhysicalDevice device);
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;
    SwapChainSupportDetails
                          querySwapChainSupport(VkPhysicalDevice device) const;
    VkSampleCountFlagBits getMaxUsableSampleCount();
    void                  createAllocator();

    VulkanContext        &ctx_;
    VkPhysicalDevice      physicalDevice_ = VK_NULL_HANDLE;
    VkDevice              device_ = VK_NULL_HANDLE;
    VkQueue               graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue               presentQueue_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    TextureTranscodeTarget textureTranscodeTarget_ =
        TextureTranscodeTarget::Rgba8;
    bool environmentIblSupported_ = false;
    ComputeBloomSupport computeBloomSupport_{};
    AtmosphereSupport atmosphereSupport_{};
    SurfaceDataSupport surfaceDataSupport_{};
    GBufferSupport gBufferSupport_{};
    OcclusionCullingSupport occlusionCullingSupport_{};
    ScreenSpaceEffectsSupport screenSpaceEffectsSupport_{};
    DepthHierarchySupport depthHierarchySupport_{};
    CacaoSupport cacaoSupport_{};
    RayQuerySupport rayQuerySupport_{};
    DdgiSupport ddgiSupport_{};
    MaterialBindingMode requestedMaterialBindingMode_ =
        MaterialBindingMode::Auto;
    MaterialBindingDeviceSupport materialBindingSupport_{};
    bool graphicsQueueSupportsCompute_ = false;
    std::vector<const char *> enabledDeviceExtensions_;
    PFN_vkCreateAccelerationStructureKHR createAccelerationStructure_ =
        nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroyAccelerationStructure_ =
        nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR
        getAccelerationStructureBuildSizes_ = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR
        cmdBuildAccelerationStructures_ = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR
        getAccelerationStructureDeviceAddress_ = nullptr;
    VmaAllocator          allocator_ = VK_NULL_HANDLE;
    std::unique_ptr<GpuDebugUtils> debugUtils_;
    std::unique_ptr<TracyProfiler> tracyProfiler_;
};

} // namespace vkr
