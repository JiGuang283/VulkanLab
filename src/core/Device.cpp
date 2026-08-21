#include "Device.h"
#include "GpuDebugUtils.h"
#include "Log.h"
#include "VulkanCheck.h"
#include "diagnostics/TracyProfiler.h"

#include <BuildFeatures.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace {
const std::vector<const char *> requiredDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME};

constexpr std::array<const char *, 3> rayQueryExtensions = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_QUERY_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
} // namespace

namespace vkr {

Device::Device(VulkanContext &ctx, MaterialBindingMode materialBindingMode)
    : ctx_(ctx), requestedMaterialBindingMode_(materialBindingMode) {
    pickPhysicalDevice();
    queryMaterialBindingSupport();
    if (requestedMaterialBindingMode_ == MaterialBindingMode::Bindless &&
        !materialBindingSupport_.supported) {
        throw std::runtime_error("Bindless material binding is unavailable: " +
                                 materialBindingSupport_.reason);
    }
    queryRayQuerySupport();
    queryDdgiSupport();
    createLogicalDevice();
    debugUtils_ =
        std::make_unique<GpuDebugUtils>(device_, ctx_.debugUtilsEnabled());
    debugUtils_->setObjectName(VK_OBJECT_TYPE_DEVICE, device_,
                               "Device/Logical");
    debugUtils_->setObjectName(VK_OBJECT_TYPE_QUEUE, graphicsQueue_,
                               "Queue/Graphics");
    debugUtils_->setObjectName(VK_OBJECT_TYPE_QUEUE, presentQueue_,
                               "Queue/Present");
    const QueueFamilyIndices families = queueFamilies();
    tracyProfiler_ = std::make_unique<TracyProfiler>(
        ctx_.instance(), physicalDevice_, device_, graphicsQueue_,
        families.graphicsFamily.value());
    if (tracyProfiler_->compiled()) {
        VKR_LOG_INFO("Tracy",
                     "Tracy {} enabled (on-demand localhost, GPU={})",
                     tracyProfiler_->version(),
                     tracyProfiler_->gpuAvailable() ? "available"
                                                     : "unavailable");
    }
    createAllocator();
}

void Device::queryMaterialBindingSupport() {
    materialBindingSupport_ = inspectMaterialBindingSupport(physicalDevice_);
}

MaterialBindingDeviceSupport Device::inspectMaterialBindingSupport(
    VkPhysicalDevice device) const {
    MaterialBindingDeviceSupport support{};
    VkPhysicalDeviceDescriptorIndexingFeatures indexing{};
    indexing.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &indexing;
    vkGetPhysicalDeviceFeatures2(device, &features);

    VkPhysicalDeviceDescriptorIndexingProperties indexingProperties{};
    indexingProperties.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &indexingProperties;
    vkGetPhysicalDeviceProperties2(device, &properties);

    constexpr uint32_t gpuMaterialSize = 128;
    support.materialCapacity =
        static_cast<uint32_t>(std::min<uint64_t>(
            65536u, properties.properties.limits.maxStorageBufferRange /
                         gpuMaterialSize));

    const bool featuresAvailable =
        indexing.runtimeDescriptorArray &&
        indexing.shaderSampledImageArrayNonUniformIndexing &&
        indexing.descriptorBindingSampledImageUpdateAfterBind &&
        indexing.descriptorBindingUpdateUnusedWhilePending &&
        indexing.descriptorBindingPartiallyBound;
    if (!featuresAvailable) {
        support.reason =
            "required Vulkan descriptor indexing features are unavailable";
        return support;
    }

    constexpr uint32_t targetTextures = 8192;
    const uint32_t textureLimit = std::min(
        {targetTextures,
         indexingProperties.maxPerStageDescriptorUpdateAfterBindSamplers,
         indexingProperties.maxPerStageDescriptorUpdateAfterBindSampledImages,
         indexingProperties.maxDescriptorSetUpdateAfterBindSamplers,
         indexingProperties.maxDescriptorSetUpdateAfterBindSampledImages,
         indexingProperties.maxUpdateAfterBindDescriptorsInAllPools});
    const uint32_t materialLimit = support.materialCapacity;
    support.textureCapacity = textureLimit;
    support.materialCapacity = materialLimit;
    if (textureLimit < 2048) {
        support.reason =
            "bindless sampled-image capacity is below 2048";
        return support;
    }
    if (materialLimit < 4096) {
        support.reason =
            "GPU material storage capacity is below 4096";
        return support;
    }
    support.supported = true;
    return support;
}
Device::~Device() {
    vmaDestroyAllocator(allocator_);
    tracyProfiler_.reset();
    debugUtils_.reset();
    vkDestroyDevice(device_, nullptr);
}

VkPhysicalDeviceProperties Device::physicalDeviceProperties() const {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
    return properties;
}

SwapChainSupportDetails
Device::querySwapChainSupport(VkPhysicalDevice device) const {
    SwapChainSupportDetails details;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, ctx_.surface(),
                                              &details.capabilities);

    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, ctx_.surface(), &formatCount,
                                         nullptr);
    if (formatCount != 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(
            device, ctx_.surface(), &formatCount, details.formats.data());
    }

    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, ctx_.surface(),
                                              &presentModeCount, nullptr);
    if (presentModeCount != 0) {
        details.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, ctx_.surface(),
                                                  &presentModeCount,
                                                  details.presentModes.data());
    }

    return details;
}

void Device::pickPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(ctx_.instance(), &deviceCount, nullptr);

    if (deviceCount == 0) {
        throw std::runtime_error("failed to find GPUs with Vulkan support!");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(ctx_.instance(), &deviceCount, devices.data());

    auto printDeviceInfo = [](const VkPhysicalDeviceProperties &props) {
        std::string typeStr;
        switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            typeStr = "Discrete GPU";
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            typeStr = "Integrated GPU";
            break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            typeStr = "Virtual GPU";
            break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            typeStr = "CPU";
            break;
        default:
            typeStr = "Other";
            break;
        }
        VKR_LOG_INFO("Device", "Selected GPU: {}", props.deviceName);
        VKR_LOG_INFO("Device", "Device Type: {}", typeStr);
        VKR_LOG_INFO("Device", "API Version: {}.{}.{}",
                     VK_VERSION_MAJOR(props.apiVersion),
                     VK_VERSION_MINOR(props.apiVersion),
                     VK_VERSION_PATCH(props.apiVersion));
    };

    int bestScore = std::numeric_limits<int>::min();
    for (const auto &device : devices) {
        if (!isDeviceSuitable(device))
            continue;

        VkPhysicalDeviceProperties deviceProperties;
        vkGetPhysicalDeviceProperties(device, &deviceProperties);
        const MaterialBindingDeviceSupport bindless =
            inspectMaterialBindingSupport(device);
        if (requestedMaterialBindingMode_ == MaterialBindingMode::Bindless &&
            !bindless.supported)
            continue;

        int score = 0;
        switch (deviceProperties.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            score = 1000;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            score = 500;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            score = 250;
            break;
        default:
            break;
        }
        if (requestedMaterialBindingMode_ != MaterialBindingMode::Legacy &&
            bindless.supported)
            score += 10000;
        if (score > bestScore) {
            bestScore = score;
            physicalDevice_ = device;
        }
    }

    if (physicalDevice_ == VK_NULL_HANDLE) {
        if (requestedMaterialBindingMode_ == MaterialBindingMode::Bindless)
            throw std::runtime_error(
                "failed to find a Vulkan 1.3 GPU with bindless material support");
        throw std::runtime_error("failed to find a suitable GPU!");
    }

    VkPhysicalDeviceProperties selectedProperties{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &selectedProperties);
    msaaSamples_ = getMaxUsableSampleCount();
    printDeviceInfo(selectedProperties);

    enabledDeviceExtensions_ = requiredDeviceExtensions;

    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(physicalDevice_, &features);
    VkFormatProperties bc7Unorm{};
    VkFormatProperties bc7Srgb{};
    vkGetPhysicalDeviceFormatProperties(
        physicalDevice_, VK_FORMAT_BC7_UNORM_BLOCK, &bc7Unorm);
    vkGetPhysicalDeviceFormatProperties(
        physicalDevice_, VK_FORMAT_BC7_SRGB_BLOCK, &bc7Srgb);
    const VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (features.textureCompressionBC &&
        (bc7Unorm.optimalTilingFeatures & required) == required &&
        (bc7Srgb.optimalTilingFeatures & required) == required) {
        textureTranscodeTarget_ = TextureTranscodeTarget::Bc7;
        VKR_LOG_INFO("Device", "Derived texture target: BC7");
    } else {
        VKR_LOG_WARN("Device", "BC7 unavailable; derived textures use RGBA8");
    }

    const auto supportsFilteredSampled = [&](VkFormat format,
                                             VkImageCreateFlags flags) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, format,
                                            &properties);
        const VkFormatFeatureFlags formatRequired =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((properties.optimalTilingFeatures & formatRequired) !=
            formatRequired) {
            return false;
        }
        VkImageFormatProperties imageProperties{};
        return vkGetPhysicalDeviceImageFormatProperties(
                   physicalDevice_, format, VK_IMAGE_TYPE_2D,
                   VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   flags, &imageProperties) == VK_SUCCESS;
    };
    environmentIblSupported_ =
        supportsFilteredSampled(VK_FORMAT_R16G16B16A16_SFLOAT,
                                VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) &&
        supportsFilteredSampled(VK_FORMAT_R16G16_SFLOAT, 0);
    if (environmentIblSupported_) {
        VKR_LOG_INFO("Device", "Floating-point IBL textures are supported");
    } else {
        VKR_LOG_WARN("Device",
                     "Floating-point IBL textures unavailable; IBL disabled");
    }

    const QueueFamilyIndices selectedFamilies =
        findQueueFamilies(physicalDevice_);
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        physicalDevice_, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueProperties(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(
        physicalDevice_, &queueFamilyCount, queueProperties.data());
    const bool graphicsQueueSupportsCompute =
        selectedFamilies.graphicsFamily &&
        *selectedFamilies.graphicsFamily < queueProperties.size() &&
        (queueProperties[*selectedFamilies.graphicsFamily].queueFlags &
         VK_QUEUE_COMPUTE_BIT) != 0;
    graphicsQueueSupportsCompute_ = graphicsQueueSupportsCompute;

    VkFormatProperties bloomFormatProperties{};
    vkGetPhysicalDeviceFormatProperties(
        physicalDevice_, VK_FORMAT_R16G16B16A16_SFLOAT,
        &bloomFormatProperties);
    const VkFormatFeatureFlags bloomRequired =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    VkImageFormatProperties bloomImageProperties{};
    const bool bloomImageSupported =
        vkGetPhysicalDeviceImageFormatProperties(
            physicalDevice_, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, 0,
            &bloomImageProperties) == VK_SUCCESS;
    VkImageFormatProperties atmosphereImageProperties{};
    const bool atmosphereImageSupported =
        vkGetPhysicalDeviceImageFormatProperties(
            physicalDevice_, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            0, &atmosphereImageProperties) == VK_SUCCESS;

    if (!graphicsQueueSupportsCompute) {
        computeBloomSupport_.reason =
            "selected graphics queue does not support compute";
    } else if (!features.shaderStorageImageExtendedFormats) {
        computeBloomSupport_.reason =
            "shaderStorageImageExtendedFormats is unavailable";
    } else if ((bloomFormatProperties.optimalTilingFeatures &
                bloomRequired) != bloomRequired ||
               !bloomImageSupported) {
        computeBloomSupport_.reason =
            "RGBA16F sampled, linear-filtered storage images are unavailable";
    } else {
        computeBloomSupport_.available = true;
        computeBloomSupport_.format =
            VK_FORMAT_R16G16B16A16_SFLOAT;
    }

    if (computeBloomSupport_.available) {
        VKR_LOG_INFO("Device", "Compute Bloom is supported");
    } else {
        VKR_LOG_WARN("Device", "Compute Bloom unavailable: {}",
                     computeBloomSupport_.reason);
    }
    if (!computeBloomSupport_.available) {
        atmosphereSupport_.reason = computeBloomSupport_.reason;
    } else if (!atmosphereImageSupported) {
        atmosphereSupport_.reason =
            "RGBA16F atmosphere image usage is unavailable";
    } else if (atmosphereImageProperties.maxArrayLayers < 32) {
        atmosphereSupport_.reason =
            "RGBA16F storage images do not support 32 array layers";
    } else {
        atmosphereSupport_.available = true;
        atmosphereSupport_.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    }
    if (atmosphereSupport_.available) {
        VKR_LOG_INFO("Device", "Compute Sky Atmosphere is supported");
    } else {
        VKR_LOG_WARN("Device", "Sky Atmosphere unavailable: {}",
                     atmosphereSupport_.reason);
    }

    constexpr VkFormat depthCandidates[] = {
        VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D16_UNORM};
    for (VkFormat format : depthCandidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, format,
                                            &properties);
        const VkFormatFeatureFlags requiredDepth =
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((properties.optimalTilingFeatures & requiredDepth) ==
            requiredDepth) {
            surfaceDataSupport_.depthFormat = format;
            break;
        }
    }
    const auto supportsSurfaceColor = [&](VkFormat format) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, format,
                                            &properties);
        const VkFormatFeatureFlags requiredSurface =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((properties.optimalTilingFeatures & requiredSurface) !=
            requiredSurface) {
            return false;
        }
        VkImageFormatProperties imageProperties{};
        return vkGetPhysicalDeviceImageFormatProperties(
                   physicalDevice_, format, VK_IMAGE_TYPE_2D,
                   VK_IMAGE_TILING_OPTIMAL,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT,
                   0, &imageProperties) == VK_SUCCESS;
    };
    if (surfaceDataSupport_.depthFormat == VK_FORMAT_UNDEFINED) {
        surfaceDataSupport_.reason =
            "no sampled depth attachment format is available";
    } else if (!supportsSurfaceColor(
                   surfaceDataSupport_.normalRoughnessFormat)) {
        surfaceDataSupport_.reason =
            "RGBA16F surface normal attachment is unavailable";
    } else if (!supportsSurfaceColor(surfaceDataSupport_.motionFormat)) {
        surfaceDataSupport_.reason =
            "RG16F surface motion attachment is unavailable";
    } else {
        surfaceDataSupport_.available = true;
    }
    if (surfaceDataSupport_.available) {
        VKR_LOG_INFO("Device", "Surface data attachments are supported");
    } else {
        VKR_LOG_WARN("Device", "Surface data unavailable: {}",
                     surfaceDataSupport_.reason);
    }
    if (!surfaceDataSupport_.available) {
        surfaceDataSupport_.albedoMetallicReason =
            surfaceDataSupport_.reason;
    } else if (!supportsSurfaceColor(
                   surfaceDataSupport_.albedoMetallicFormat)) {
        surfaceDataSupport_.albedoMetallicReason =
            "RGBA8 surface albedo-metallic attachment is unavailable";
    } else {
        surfaceDataSupport_.albedoMetallicAvailable = true;
    }

    gBufferSupport_.depthFormat = surfaceDataSupport_.depthFormat;
    const VkPhysicalDeviceProperties gBufferDeviceProperties =
        physicalDeviceProperties();
    if (!surfaceDataSupport_.available) {
        gBufferSupport_.reason = surfaceDataSupport_.reason;
    } else if (!surfaceDataSupport_.albedoMetallicAvailable) {
        gBufferSupport_.reason =
            surfaceDataSupport_.albedoMetallicReason;
    } else if (gBufferDeviceProperties.limits.maxColorAttachments <
               gBufferSupport_.requiredColorAttachments) {
        gBufferSupport_.reason =
            "fewer than four simultaneous color attachments are supported";
    } else {
        gBufferSupport_.available = true;
    }
    if (gBufferSupport_.available) {
        VKR_LOG_INFO("Device", "Deferred GBuffer attachments are supported");
    } else {
        VKR_LOG_WARN("Device", "Deferred GBuffer unavailable: {}",
                     gBufferSupport_.reason);
    }

    VkFormatProperties hiZProperties{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice_,
                                        VK_FORMAT_R32_SFLOAT,
                                        &hiZProperties);
    const VkFormatFeatureFlags requiredHiZ =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    VkImageFormatProperties hiZImageProperties{};
    const bool hiZImageSupported =
        vkGetPhysicalDeviceImageFormatProperties(
            physicalDevice_, VK_FORMAT_R32_SFLOAT, VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, 0,
            &hiZImageProperties) == VK_SUCCESS;
    if (!graphicsQueueSupportsCompute) {
        occlusionCullingSupport_.reason =
            "selected graphics queue does not support compute";
    } else if (!surfaceDataSupport_.available) {
        occlusionCullingSupport_.reason = surfaceDataSupport_.reason;
    } else if ((hiZProperties.optimalTilingFeatures & requiredHiZ) !=
                   requiredHiZ ||
               !hiZImageSupported) {
        occlusionCullingSupport_.reason =
            "R32F sampled storage images are unavailable";
    } else {
        occlusionCullingSupport_.available = true;
    }
    if (occlusionCullingSupport_.available) {
        VKR_LOG_INFO("Device", "Hi-Z occlusion culling is supported");
    } else {
        VKR_LOG_WARN("Device", "Hi-Z occlusion culling unavailable: {}",
                     occlusionCullingSupport_.reason);
    }

    const auto supportsSampledStorage = [&](VkFormat format,
                                            bool linearFilter) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, format,
                                            &properties);
        VkFormatFeatureFlags requiredFeatures =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if (linearFilter) {
            requiredFeatures |=
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        }
        if ((properties.optimalTilingFeatures & requiredFeatures) !=
            requiredFeatures) {
            return false;
        }
        VkImageFormatProperties imageProperties{};
        return vkGetPhysicalDeviceImageFormatProperties(
                   physicalDevice_, format, VK_IMAGE_TYPE_2D,
                   VK_IMAGE_TILING_OPTIMAL,
                   VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_STORAGE_BIT,
                   0, &imageProperties) == VK_SUCCESS;
    };

    if (!graphicsQueueSupportsCompute) {
        screenSpaceEffectsSupport_.depthPyramidReason =
            "selected graphics queue does not support compute";
    } else if (!supportsSampledStorage(VK_FORMAT_R32_SFLOAT, false)) {
        screenSpaceEffectsSupport_.depthPyramidReason =
            "R32F sampled storage images are unavailable";
    } else if (!surfaceDataSupport_.available) {
        screenSpaceEffectsSupport_.depthPyramidReason =
            surfaceDataSupport_.reason;
    } else {
        screenSpaceEffectsSupport_.depthPyramidAvailable = true;
    }

    if (!occlusionCullingSupport_.available ||
        !screenSpaceEffectsSupport_.depthPyramidAvailable) {
        depthHierarchySupport_.reason =
            !occlusionCullingSupport_.available
                ? occlusionCullingSupport_.reason
                : screenSpaceEffectsSupport_.depthPyramidReason;
    } else if (features.shaderStorageImageExtendedFormats &&
               supportsSampledStorage(VK_FORMAT_R32G32_SFLOAT, false)) {
        depthHierarchySupport_.mode =
            DepthHierarchyMode::CombinedMinMax;
        depthHierarchySupport_.format = VK_FORMAT_R32G32_SFLOAT;
    } else {
        depthHierarchySupport_.mode = DepthHierarchyMode::SplitR32;
        depthHierarchySupport_.format = VK_FORMAT_R32_SFLOAT;
        depthHierarchySupport_.reason =
            "RG32F sampled storage images are unavailable; using split R32F chains";
    }
    if (depthHierarchySupport_.available()) {
        VKR_LOG_INFO(
            "Device", "Depth hierarchy mode: {} ({})",
            depthHierarchySupport_.combined() ? "combined-min-max"
                                              : "split-r32",
            depthHierarchySupport_.combined()
                ? "RG32F"
                : depthHierarchySupport_.reason);
    } else {
        VKR_LOG_WARN("Device", "Depth hierarchy unavailable: {}",
                     depthHierarchySupport_.reason);
    }

    if (!graphicsQueueSupportsCompute) {
        screenSpaceEffectsSupport_.colorPyramidReason =
            "selected graphics queue does not support compute";
    } else if (!features.shaderStorageImageExtendedFormats) {
        screenSpaceEffectsSupport_.colorPyramidReason =
            "shaderStorageImageExtendedFormats is unavailable";
    } else if (!supportsSampledStorage(
                   VK_FORMAT_R16G16B16A16_SFLOAT, true)) {
        screenSpaceEffectsSupport_.colorPyramidReason =
            "RGBA16F sampled, linear-filtered storage images are unavailable";
    } else {
        screenSpaceEffectsSupport_.colorPyramidAvailable = true;
    }

    if (!surfaceDataSupport_.available) {
        screenSpaceEffectsSupport_.ssaoReason = surfaceDataSupport_.reason;
    } else if (!graphicsQueueSupportsCompute) {
        screenSpaceEffectsSupport_.ssaoReason =
            "selected graphics queue does not support compute";
    } else if (!features.shaderStorageImageExtendedFormats) {
        screenSpaceEffectsSupport_.ssaoReason =
            "shaderStorageImageExtendedFormats is unavailable";
    } else if (!supportsSampledStorage(VK_FORMAT_R16_SFLOAT, true)) {
        screenSpaceEffectsSupport_.ssaoReason =
            "R16F sampled, linear-filtered storage images are unavailable";
    } else {
        screenSpaceEffectsSupport_.ssaoAvailable = true;
    }

    if (!screenSpaceEffectsSupport_.ssaoAvailable) {
        screenSpaceEffectsSupport_.gtaoReason =
            screenSpaceEffectsSupport_.ssaoReason;
    } else if (!screenSpaceEffectsSupport_.depthPyramidAvailable) {
        screenSpaceEffectsSupport_.gtaoReason =
            screenSpaceEffectsSupport_.depthPyramidReason;
    } else {
        screenSpaceEffectsSupport_.gtaoAvailable = true;
    }

    if (!surfaceDataSupport_.available) {
        screenSpaceEffectsSupport_.taaReason = surfaceDataSupport_.reason;
    } else if (!screenSpaceEffectsSupport_.colorPyramidAvailable) {
        screenSpaceEffectsSupport_.taaReason =
            screenSpaceEffectsSupport_.colorPyramidReason;
    } else {
        screenSpaceEffectsSupport_.taaAvailable = true;
    }

    if (!surfaceDataSupport_.available) {
        screenSpaceEffectsSupport_.ssrReason = surfaceDataSupport_.reason;
    } else if (!screenSpaceEffectsSupport_.depthPyramidAvailable) {
        screenSpaceEffectsSupport_.ssrReason =
            screenSpaceEffectsSupport_.depthPyramidReason;
    } else if (!screenSpaceEffectsSupport_.colorPyramidAvailable) {
        screenSpaceEffectsSupport_.ssrReason =
            screenSpaceEffectsSupport_.colorPyramidReason;
    } else {
        screenSpaceEffectsSupport_.ssrAvailable = true;
    }

    if (!surfaceDataSupport_.available) {
        screenSpaceEffectsSupport_.ssgiReason = surfaceDataSupport_.reason;
    } else if (!surfaceDataSupport_.albedoMetallicAvailable) {
        screenSpaceEffectsSupport_.ssgiReason =
            surfaceDataSupport_.albedoMetallicReason;
    } else if (!screenSpaceEffectsSupport_.depthPyramidAvailable) {
        screenSpaceEffectsSupport_.ssgiReason =
            screenSpaceEffectsSupport_.depthPyramidReason;
    } else if (!screenSpaceEffectsSupport_.colorPyramidAvailable) {
        screenSpaceEffectsSupport_.ssgiReason =
            screenSpaceEffectsSupport_.colorPyramidReason;
    } else {
        screenSpaceEffectsSupport_.ssgiAvailable = true;
    }

    cacaoSupport_.compiled = build::kCacao;
    const auto supportsCacao2D = [&](VkFormat format, uint32_t arrayLayers,
                                     uint32_t mipLevels,
                                     bool requireLinearFilter = false) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, format,
                                            &properties);
        constexpr VkFormatFeatureFlags requiredFeatures =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        const VkFormatFeatureFlags required =
            requiredFeatures |
            (requireLinearFilter
                 ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
                 : 0u);
        if ((properties.optimalTilingFeatures & required) != required) {
            return false;
        }
        VkImageFormatProperties imageProperties{};
        if (vkGetPhysicalDeviceImageFormatProperties(
                physicalDevice_, format, VK_IMAGE_TYPE_2D,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                0, &imageProperties) != VK_SUCCESS) {
            return false;
        }
        return imageProperties.maxArrayLayers >= arrayLayers &&
               imageProperties.maxMipLevels >= mipLevels;
    };
    const auto supportsCacaoCounter = [&]() {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_,
                                            VK_FORMAT_R32_UINT,
                                            &properties);
        constexpr VkFormatFeatureFlags requiredFeatures =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
            VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((properties.optimalTilingFeatures & requiredFeatures) !=
            requiredFeatures) {
            return false;
        }
        VkImageFormatProperties imageProperties{};
        return vkGetPhysicalDeviceImageFormatProperties(
                   physicalDevice_, VK_FORMAT_R32_UINT,
                   VK_IMAGE_TYPE_1D, VK_IMAGE_TILING_OPTIMAL,
                   VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_STORAGE_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   0, &imageProperties) == VK_SUCCESS;
    };
    if (!cacaoSupport_.compiled) {
        cacaoSupport_.reason = "CACAO was not compiled into this build";
    } else if (!graphicsQueueSupportsCompute) {
        cacaoSupport_.reason =
            "selected graphics queue does not support compute";
    } else if (!surfaceDataSupport_.available) {
        cacaoSupport_.reason = surfaceDataSupport_.reason;
    } else if (!features.shaderStorageImageExtendedFormats) {
        cacaoSupport_.reason =
            "shaderStorageImageExtendedFormats is unavailable";
    } else if (!features.shaderImageGatherExtended) {
        cacaoSupport_.reason =
            "shaderImageGatherExtended is unavailable";
    } else if (!supportsCacao2D(VK_FORMAT_R16_SFLOAT, 4, 4) ||
               !supportsCacao2D(VK_FORMAT_R8G8B8A8_SNORM, 4, 1) ||
               !supportsCacao2D(VK_FORMAT_R8G8_UNORM, 4, 1) ||
               !supportsCacao2D(VK_FORMAT_R8_UNORM, 1, 1) ||
               !supportsCacao2D(cacaoSupport_.depthAdapterFormat, 1, 1) ||
               !supportsCacao2D(cacaoSupport_.normalAdapterFormat, 1, 1) ||
               !supportsCacao2D(cacaoSupport_.outputFormat, 1, 1, true) ||
               !supportsCacaoCounter()) {
        cacaoSupport_.reason =
            "required CACAO sampled/storage image formats are unavailable";
    } else {
        cacaoSupport_.available = true;
    }

    VKR_LOG_INFO(
        "Device",
        "Screen-space support: depth pyramid={}, color pyramid={}, SSAO={}, "
        "GTAO={}, TAA={}, SSR={}, SSGI={}",
        screenSpaceEffectsSupport_.depthPyramidAvailable ? "yes" : "no",
        screenSpaceEffectsSupport_.colorPyramidAvailable ? "yes" : "no",
        screenSpaceEffectsSupport_.ssaoAvailable ? "yes" : "no",
        screenSpaceEffectsSupport_.gtaoAvailable ? "yes" : "no",
        screenSpaceEffectsSupport_.taaAvailable ? "yes" : "no",
        screenSpaceEffectsSupport_.ssrAvailable ? "yes" : "no",
        screenSpaceEffectsSupport_.ssgiAvailable ? "yes" : "no");
    if (cacaoSupport_.available) {
        VKR_LOG_INFO("Device", "FidelityFX CACAO FP32 is supported");
    } else if (cacaoSupport_.compiled) {
        VKR_LOG_WARN("Device", "FidelityFX CACAO unavailable: {}",
                     cacaoSupport_.reason);
    }
}

void Device::queryRayQuerySupport() {
    const VkPhysicalDeviceProperties properties = physicalDeviceProperties();
    if (VK_VERSION_MAJOR(properties.apiVersion) < 1 ||
        (VK_VERSION_MAJOR(properties.apiVersion) == 1 &&
         VK_VERSION_MINOR(properties.apiVersion) < 2)) {
        rayQuerySupport_.reason = "Vulkan 1.2 is required";
        VKR_LOG_WARN("Device", "Ray Query unavailable: {}",
                     rayQuerySupport_.reason);
        return;
    }

    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr,
                                         &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr,
                                         &extensionCount,
                                         extensions.data());
    const auto hasExtension = [&](const char *name) {
        return std::any_of(extensions.begin(), extensions.end(),
                           [&](const VkExtensionProperties &extension) {
                               return std::strcmp(extension.extensionName,
                                                  name) == 0;
                           });
    };
    for (const char *extension : rayQueryExtensions) {
        if (!hasExtension(extension)) {
            rayQuerySupport_.reason =
                std::string("missing device extension ") + extension;
            VKR_LOG_WARN("Device", "Ray Query unavailable: {}",
                         rayQuerySupport_.reason);
            return;
        }
    }

    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{};
    rayQuery.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{};
    acceleration.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    acceleration.pNext = &rayQuery;
    VkPhysicalDeviceBufferDeviceAddressFeatures bufferAddress{};
    bufferAddress.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    bufferAddress.pNext = &acceleration;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &bufferAddress;
    vkGetPhysicalDeviceFeatures2(physicalDevice_, &features);
    if (!features.features.shaderInt64 ||
        !bufferAddress.bufferDeviceAddress ||
        !acceleration.accelerationStructure || !rayQuery.rayQuery) {
        rayQuerySupport_.reason =
            "shaderInt64, buffer device address, acceleration structure, or ray query feature is unavailable";
        VKR_LOG_WARN("Device", "Ray Query unavailable: {}",
                     rayQuerySupport_.reason);
        return;
    }

    VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationProps{};
    accelerationProps.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties2.pNext = &accelerationProps;
    vkGetPhysicalDeviceProperties2(physicalDevice_, &properties2);
    rayQuerySupport_.available = true;
    rayQuerySupport_.maxGeometryCount = accelerationProps.maxGeometryCount;
    rayQuerySupport_.maxInstanceCount = accelerationProps.maxInstanceCount;
    rayQuerySupport_.maxPrimitiveCount = accelerationProps.maxPrimitiveCount;
    rayQuerySupport_.minScratchAlignment =
        accelerationProps.minAccelerationStructureScratchOffsetAlignment;
    enabledDeviceExtensions_.insert(enabledDeviceExtensions_.end(),
                                    rayQueryExtensions.begin(),
                                    rayQueryExtensions.end());
    VKR_LOG_INFO("Device",
                 "Vulkan Ray Query is supported (max instances={}, scratch alignment={})",
                 rayQuerySupport_.maxInstanceCount,
                 rayQuerySupport_.minScratchAlignment);
}

void Device::queryDdgiSupport() {
    if (!rayQuerySupport_.available) {
        ddgiSupport_.reason = rayQuerySupport_.reason;
        return;
    }
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(physicalDevice_, &features);
    if (!features.shaderStorageImageExtendedFormats) {
        ddgiSupport_.reason =
            "shaderStorageImageExtendedFormats is unavailable";
        return;
    }
    const auto supportsAtlas = [&](VkFormat format) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, format,
                                            &properties);
        constexpr VkFormatFeatureFlags required =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if ((properties.optimalTilingFeatures & required) != required)
            return false;
        VkImageFormatProperties imageProperties{};
        return vkGetPhysicalDeviceImageFormatProperties(
                   physicalDevice_, format, VK_IMAGE_TYPE_2D,
                   VK_IMAGE_TILING_OPTIMAL,
                   VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_STORAGE_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   0, &imageProperties) == VK_SUCCESS &&
               imageProperties.maxArrayLayers >= 2048;
    };
    if (!supportsAtlas(ddgiSupport_.irradianceFormat) ||
        !supportsAtlas(ddgiSupport_.distanceFormat)) {
        ddgiSupport_.reason =
            "required RGBA16F/RG16F 2048-layer storage atlases are unavailable";
        return;
    }
    ddgiSupport_.available = true;
    VKR_LOG_INFO("Device", "Ray Query DDGI is supported");
}
void Device::createLogicalDevice() {
    QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::set<uint32_t> uniqueQueueFamilies = {indices.graphicsFamily.value(),
                                              indices.presentFamily.value()};

    float queuePriority = 1.0f;
    for (uint32_t queueFamily : uniqueQueueFamilies) {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy = VK_TRUE;
    deviceFeatures.sampleRateShading = VK_TRUE;
    deviceFeatures.imageCubeArray = VK_TRUE;
    deviceFeatures.textureCompressionBC =
        textureTranscodeTarget_ == TextureTranscodeTarget::Bc7;
    deviceFeatures.shaderStorageImageExtendedFormats =
        (computeBloomSupport_.available || atmosphereSupport_.available ||
         screenSpaceEffectsSupport_.colorPyramidAvailable ||
         screenSpaceEffectsSupport_.ssaoAvailable ||
         cacaoSupport_.available || ddgiSupport_.available)
            ? VK_TRUE
            : VK_FALSE;
    deviceFeatures.shaderImageGatherExtended =
        cacaoSupport_.available ? VK_TRUE : VK_FALSE;
    deviceFeatures.shaderInt64 =
        rayQuerySupport_.available ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{};
    rayQuery.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    rayQuery.rayQuery = rayQuerySupport_.available ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{};
    acceleration.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    acceleration.accelerationStructure =
        rayQuerySupport_.available ? VK_TRUE : VK_FALSE;
    acceleration.pNext = &rayQuery;
    VkPhysicalDeviceVulkan13Features vulkan13{};
    vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13.dynamicRendering = VK_TRUE;
    vulkan13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features vulkan12{};
    vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    const bool enableBindless =
        requestedMaterialBindingMode_ != MaterialBindingMode::Legacy &&
        materialBindingSupport_.supported;
    vulkan12.runtimeDescriptorArray = enableBindless;
    vulkan12.shaderSampledImageArrayNonUniformIndexing = enableBindless;
    vulkan12.descriptorBindingSampledImageUpdateAfterBind = enableBindless;
    vulkan12.descriptorBindingUpdateUnusedWhilePending = enableBindless;
    vulkan12.descriptorBindingPartiallyBound = enableBindless;
    vulkan12.bufferDeviceAddress =
        rayQuerySupport_.available ? VK_TRUE : VK_FALSE;
    vulkan12.pNext = rayQuerySupport_.available ? &acceleration : nullptr;
    vulkan13.pNext = &vulkan12;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

    createInfo.queueCreateInfoCount =
        static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();

    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount =
        static_cast<uint32_t>(enabledDeviceExtensions_.size());
    createInfo.ppEnabledExtensionNames = enabledDeviceExtensions_.data();
    createInfo.pNext = &vulkan13;

    VK_CHECK(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_));

    vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0,
                     &graphicsQueue_);
    vkGetDeviceQueue(device_, indices.presentFamily.value(), 0, &presentQueue_);

    if (rayQuerySupport_.available) {
        createAccelerationStructure_ =
            reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
                vkGetDeviceProcAddr(device_,
                                    "vkCreateAccelerationStructureKHR"));
        destroyAccelerationStructure_ =
            reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
                vkGetDeviceProcAddr(device_,
                                    "vkDestroyAccelerationStructureKHR"));
        getAccelerationStructureBuildSizes_ =
            reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
                vkGetDeviceProcAddr(
                    device_, "vkGetAccelerationStructureBuildSizesKHR"));
        cmdBuildAccelerationStructures_ =
            reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
                vkGetDeviceProcAddr(
                    device_, "vkCmdBuildAccelerationStructuresKHR"));
        getAccelerationStructureDeviceAddress_ =
            reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
                vkGetDeviceProcAddr(
                    device_, "vkGetAccelerationStructureDeviceAddressKHR"));
        if (!createAccelerationStructure_ || !destroyAccelerationStructure_ ||
            !getAccelerationStructureBuildSizes_ ||
            !cmdBuildAccelerationStructures_ ||
            !getAccelerationStructureDeviceAddress_) {
            throw std::runtime_error(
                "Ray Query extension functions could not be loaded");
        }
    }
}

bool Device::isDeviceSuitable(VkPhysicalDevice device) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3)
        return false;

    QueueFamilyIndices indices = findQueueFamilies(device);

    bool extensionsSupported = checkDeviceExtensionSupport(device);

    bool swapChainAdequate = false;
    if (extensionsSupported) {
        SwapChainSupportDetails swapChainSupport =
            querySwapChainSupport(device);
        swapChainAdequate = !swapChainSupport.formats.empty() &&
                            !swapChainSupport.presentModes.empty();
    }

    VkPhysicalDeviceVulkan13Features vulkan13{};
    vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 supportedFeatures{};
    supportedFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supportedFeatures.pNext = &vulkan13;
    vkGetPhysicalDeviceFeatures2(device, &supportedFeatures);

    return indices.isComplete() && extensionsSupported && swapChainAdequate &&
           supportedFeatures.features.samplerAnisotropy &&
           supportedFeatures.features.imageCubeArray &&
           vulkan13.dynamicRendering && vulkan13.synchronization2;
}
bool Device::checkDeviceExtensionSupport(VkPhysicalDevice device) {
    uint32_t extensionCount;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                         nullptr);

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                         availableExtensions.data());

    std::set<std::string> requiredExtensions(
        requiredDeviceExtensions.begin(), requiredDeviceExtensions.end());

    for (const auto &extension : availableExtensions) {
        requiredExtensions.erase(extension.extensionName);
    }

    return requiredExtensions.empty();
}
QueueFamilyIndices Device::findQueueFamilies(VkPhysicalDevice device) const {
    QueueFamilyIndices indices;

    uint32_t QueueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &QueueFamilyCount,
                                             nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(QueueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &QueueFamilyCount,
                                             queueFamilies.data());

    std::optional<uint32_t> graphicsFallback;
    std::optional<uint32_t> graphicsCompute;
    uint32_t i = 0;
    for (const auto &queueFamily : queueFamilies) {
        if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            if (!graphicsFallback)
                graphicsFallback = i;
            if (!graphicsCompute &&
                (queueFamily.queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                graphicsCompute = i;
            }
        }

        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, ctx_.surface(),
                                             &presentSupport);

        if (presentSupport && !indices.presentFamily) {
            indices.presentFamily = i;
        }

        i++;
    }

    indices.graphicsFamily =
        graphicsCompute ? graphicsCompute : graphicsFallback;

    return indices;
}

VkSampleCountFlagBits Device::getMaxUsableSampleCount() {
    VkPhysicalDeviceProperties physicalDeviceProperties;
    vkGetPhysicalDeviceProperties(physicalDevice_, &physicalDeviceProperties);

    VkSampleCountFlags counts =
        physicalDeviceProperties.limits.framebufferColorSampleCounts &
        physicalDeviceProperties.limits.framebufferDepthSampleCounts;
    if (counts & VK_SAMPLE_COUNT_64_BIT) {
        return VK_SAMPLE_COUNT_64_BIT;
    }
    if (counts & VK_SAMPLE_COUNT_32_BIT) {
        return VK_SAMPLE_COUNT_32_BIT;
    }
    if (counts & VK_SAMPLE_COUNT_16_BIT) {
        return VK_SAMPLE_COUNT_16_BIT;
    }
    if (counts & VK_SAMPLE_COUNT_8_BIT) {
        return VK_SAMPLE_COUNT_8_BIT;
    }
    if (counts & VK_SAMPLE_COUNT_4_BIT) {
        return VK_SAMPLE_COUNT_4_BIT;
    }
    if (counts & VK_SAMPLE_COUNT_2_BIT) {
        return VK_SAMPLE_COUNT_2_BIT;
    }

    return VK_SAMPLE_COUNT_1_BIT;
}

SwapChainSupportDetails Device::querySwapChainSupport() const {
    return querySwapChainSupport(physicalDevice_);
}

QueueFamilyIndices Device::queueFamilies() const {
    return findQueueFamilies(physicalDevice_);
}

AllocatorMemorySnapshot Device::allocatorMemorySnapshot() const {
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(allocator_, &stats);
    return {static_cast<uint64_t>(stats.total.statistics.allocationCount),
            static_cast<uint64_t>(stats.total.statistics.allocationBytes),
            static_cast<uint64_t>(stats.total.statistics.blockBytes)};
}

void Device::createAllocator() {
    VmaAllocatorCreateInfo info{};
    info.physicalDevice = physicalDevice_;
    info.device = device_;
    info.instance = ctx_.instance();
    info.vulkanApiVersion = physicalDeviceProperties().apiVersion;
    if (rayQuerySupport_.available)
        info.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    VK_CHECK(vmaCreateAllocator(&info, &allocator_));
}

VkDeviceAddress Device::bufferDeviceAddress(VkBuffer buffer) const {
    if (!rayQuerySupport_.available || buffer == VK_NULL_HANDLE)
        return 0;
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(device_, &info);
}

VkAccelerationStructureKHR Device::createAccelerationStructure(
    VkBuffer buffer, VkDeviceSize size,
    VkAccelerationStructureTypeKHR type,
    const std::string &debugName) const {
    if (!rayQuerySupport_.available || !createAccelerationStructure_)
        return VK_NULL_HANDLE;
    VkAccelerationStructureCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    info.buffer = buffer;
    info.size = size;
    info.type = type;
    VkAccelerationStructureKHR result = VK_NULL_HANDLE;
    VK_CHECK(createAccelerationStructure_(device_, &info, nullptr, &result));
    if (!debugName.empty()) {
        debugUtils_->setObjectName(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR,
                                   result, debugName);
    }
    return result;
}

void Device::destroyAccelerationStructure(
    VkAccelerationStructureKHR accelerationStructure) const {
    if (accelerationStructure != VK_NULL_HANDLE &&
        destroyAccelerationStructure_) {
        destroyAccelerationStructure_(device_, accelerationStructure,
                                      nullptr);
    }
}

void Device::accelerationStructureBuildSizes(
    VkAccelerationStructureBuildGeometryInfoKHR &buildInfo,
    uint32_t primitiveCount,
    VkAccelerationStructureBuildSizesInfoKHR &sizes) const {
    sizes = {};
    sizes.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    getAccelerationStructureBuildSizes_(
        device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo, &primitiveCount, &sizes);
}

void Device::cmdBuildAccelerationStructures(
    VkCommandBuffer commandBuffer,
    const VkAccelerationStructureBuildGeometryInfoKHR &buildInfo,
    const VkAccelerationStructureBuildRangeInfoKHR &range) const {
    const VkAccelerationStructureBuildRangeInfoKHR *rangePtr = &range;
    cmdBuildAccelerationStructures_(commandBuffer, 1, &buildInfo,
                                    &rangePtr);
}

VkDeviceAddress Device::accelerationStructureDeviceAddress(
    VkAccelerationStructureKHR accelerationStructure) const {
    if (accelerationStructure == VK_NULL_HANDLE ||
        !getAccelerationStructureDeviceAddress_)
        return 0;
    VkAccelerationStructureDeviceAddressInfoKHR info{};
    info.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    info.accelerationStructure = accelerationStructure;
    return getAccelerationStructureDeviceAddress_(device_, &info);
}

} // namespace vkr
