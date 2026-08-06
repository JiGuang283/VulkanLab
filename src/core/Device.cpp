#include "Device.h"
#include "GpuDebugUtils.h"
#include "Log.h"
#include "VulkanCheck.h"
#include "diagnostics/TracyProfiler.h"

#include <BuildFeatures.h>

#include <set>
#include <string>
#include <vector>

namespace {
const std::vector<const char *> deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME};
} // namespace

namespace vkr {

Device::Device(VulkanContext &ctx) : ctx_(ctx) {
    pickPhysicalDevice();
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

    VkPhysicalDevice fallbackDevice = VK_NULL_HANDLE;

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

    for (const auto &device : devices) {
        if (!isDeviceSuitable(device))
            continue;

        VkPhysicalDeviceProperties deviceProperties;
        vkGetPhysicalDeviceProperties(device, &deviceProperties);

        if (deviceProperties.deviceType ==
            VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physicalDevice_ = device;
            msaaSamples_ = getMaxUsableSampleCount();
            printDeviceInfo(deviceProperties);
            break;
        }

        if (fallbackDevice == VK_NULL_HANDLE) {
            fallbackDevice = device;
        }
    }

    if (physicalDevice_ == VK_NULL_HANDLE) {
        if (fallbackDevice != VK_NULL_HANDLE) {
            physicalDevice_ = fallbackDevice;
            VkPhysicalDeviceProperties deviceProperties;
            vkGetPhysicalDeviceProperties(physicalDevice_, &deviceProperties);
            printDeviceInfo(deviceProperties);
        } else {
            throw std::runtime_error("failed to find a suitable GPU!");
        }
    }

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
        "Screen-space support: depth pyramid={}, color pyramid={}, SSAO={}",
        screenSpaceEffectsSupport_.depthPyramidAvailable ? "yes" : "no",
        screenSpaceEffectsSupport_.colorPyramidAvailable ? "yes" : "no",
        screenSpaceEffectsSupport_.ssaoAvailable ? "yes" : "no");
    if (cacaoSupport_.available) {
        VKR_LOG_INFO("Device", "FidelityFX CACAO FP32 is supported");
    } else if (cacaoSupport_.compiled) {
        VKR_LOG_WARN("Device", "FidelityFX CACAO unavailable: {}",
                     cacaoSupport_.reason);
    }
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
    deviceFeatures.textureCompressionBC =
        textureTranscodeTarget_ == TextureTranscodeTarget::Bc7;
    deviceFeatures.shaderStorageImageExtendedFormats =
        (computeBloomSupport_.available || atmosphereSupport_.available ||
         screenSpaceEffectsSupport_.colorPyramidAvailable ||
         screenSpaceEffectsSupport_.ssaoAvailable ||
         cacaoSupport_.available)
            ? VK_TRUE
            : VK_FALSE;
    deviceFeatures.shaderImageGatherExtended =
        cacaoSupport_.available ? VK_TRUE : VK_FALSE;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

    createInfo.queueCreateInfoCount =
        static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();

    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount =
        static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    VK_CHECK(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_));

    vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0,
                     &graphicsQueue_);
    vkGetDeviceQueue(device_, indices.presentFamily.value(), 0, &presentQueue_);
}

bool Device::isDeviceSuitable(VkPhysicalDevice device) {
    QueueFamilyIndices indices = findQueueFamilies(device);

    bool extensionsSupported = checkDeviceExtensionSupport(device);

    bool swapChainAdequate = false;
    if (extensionsSupported) {
        SwapChainSupportDetails swapChainSupport =
            querySwapChainSupport(device);
        swapChainAdequate = !swapChainSupport.formats.empty() &&
                            !swapChainSupport.presentModes.empty();
    }

    VkPhysicalDeviceFeatures supportedFeatures;
    vkGetPhysicalDeviceFeatures(device, &supportedFeatures);

    return indices.isComplete() && extensionsSupported && swapChainAdequate &&
           supportedFeatures.samplerAnisotropy;
}
bool Device::checkDeviceExtensionSupport(VkPhysicalDevice device) {
    uint32_t extensionCount;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                         nullptr);

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                         availableExtensions.data());

    std::set<std::string> requiredExtensions(deviceExtensions.begin(),
                                             deviceExtensions.end());

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
    info.vulkanApiVersion = VK_API_VERSION_1_0;
    VK_CHECK(vmaCreateAllocator(&info, &allocator_));
}

} // namespace vkr
