#include "VulkanContext.h"
#include <BuildFeatures.h>
#include "Log.h"
#include "VulkanCheck.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {
const std::vector<const char *> validationLayers = {
    "VK_LAYER_KHRONOS_validation"};

uint32_t loaderApiVersion() {
    const auto enumerateVersion =
        reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(VK_NULL_HANDLE,
                                  "vkEnumerateInstanceVersion"));
    if (!enumerateVersion)
        return VK_API_VERSION_1_0;
    uint32_t version = VK_API_VERSION_1_0;
    if (enumerateVersion(&version) != VK_SUCCESS)
        return VK_API_VERSION_1_0;
    return version;
}

bool hasValidationLayer() {
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    return std::any_of(layers.begin(), layers.end(), [](const auto &layer) {
        return std::strcmp(layer.layerName, validationLayers.front()) == 0;
    });
}

bool hasInstanceExtension(const char *layerName, const char *extensionName) {
    uint32_t extensionCount = 0;
    if (vkEnumerateInstanceExtensionProperties(
            layerName, &extensionCount, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> extensions(extensionCount);
    if (vkEnumerateInstanceExtensionProperties(
            layerName, &extensionCount, extensions.data()) != VK_SUCCESS) {
        return false;
    }
    return std::any_of(
        extensions.begin(), extensions.end(), [&](const auto &extension) {
            return std::strcmp(extension.extensionName, extensionName) == 0;
        });
}

VkResult CreateDebugUtilsMessengerEXT(
    VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkDebugUtilsMessengerEXT    *pDebugMessenger) {
    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance, "vkCreateDebugUtilsMessengerEXT");
    if (func != nullptr) {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    } else {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
}

void DestroyDebugUtilsMessengerEXT(VkInstance                   instance,
                                   VkDebugUtilsMessengerEXT     debugMessenger,
                                   const VkAllocationCallbacks *pAllocator) {
    auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance, "vkDestroyDebugUtilsMessengerEXT");
    if (func != nullptr) {
        func(instance, debugMessenger, pAllocator);
    }
}
} // namespace

namespace vkr {

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanContext::debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT *callbackData,
    void *userData) {
    (void)messageType;
    auto *context = static_cast<VulkanContext *>(userData);

    if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        if (context)
            context->validationErrors_.fetch_add(1,
                                                 std::memory_order_relaxed);
        VKR_LOG_ERROR("Vulkan", "[Validation] {}", callbackData->pMessage);
    } else if (messageSeverity &
               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        if (context)
            context->validationWarnings_.fetch_add(
                1, std::memory_order_relaxed);
        VKR_LOG_WARN("Vulkan", "[Validation] {}", callbackData->pMessage);
    } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        VKR_LOG_DEBUG("Vulkan", "[Validation] {}", callbackData->pMessage);
    } else {
        VKR_LOG_TRACE("Vulkan", "[Validation] {}", callbackData->pMessage);
    }
    return VK_FALSE;
}

VulkanContext::VulkanContext(SurfaceCreator            createSurface,
                             std::vector<const char *> requiredExtensions,
                             VulkanContextOptions options)
    : options_(options) {
#if !VKL_ENABLE_VALIDATION
        options_.validationProfile = ValidationProfile::Off;
        options_.validationAllowed = false;
#endif
#if !VKL_ENABLE_GPU_DEBUG_UTILS
    options_.debugUtilsRequested = false;
#endif
    createInstance(std::move(requiredExtensions));
    setupDebugMessenger();
    surface_ = createSurface(instance_);

    const ValidationStatus status = validationStatus();
    VKR_LOG_INFO(
        "Vulkan",
        "Validation requested={} actual={} layer={} features={} "
        "debugUtils={} fallback={}",
        validationProfileName(status.requested),
        validationProfileName(status.actual), status.layerAvailable,
        status.validationFeaturesAvailable, status.debugUtilsEnabled,
        status.fallbackReason.empty() ? "none" : status.fallbackReason);
}

VulkanContext::~VulkanContext() {
    if (debugMessenger_ != VK_NULL_HANDLE) {
        DestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
    }
    if (surface_ != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_ != VK_NULL_HANDLE)
        vkDestroyInstance(instance_, nullptr);
}

void VulkanContext::createInstance(
    std::vector<const char *> requiredExtensions) {
    ValidationEnvironment environment;
    environment.validationAllowed = options_.validationAllowed;
    environment.debugUtilsRequested = options_.debugUtilsRequested;
    environment.layerAvailable = hasValidationLayer();
    environment.validationFeaturesAvailable =
        environment.layerAvailable &&
        hasInstanceExtension(validationLayers.front(),
                             VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    environment.debugUtilsAvailable =
        hasInstanceExtension(nullptr, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    environment.loaderApiVersion = loaderApiVersion();
    validationStatus_ =
        resolveValidationStatus(options_.validationProfile, environment);
    instanceApiVersion_ =
        validationInstanceApiVersion(validationStatus_);

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "VulkanLab";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "VulkanLab";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = instanceApiVersion_;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    if (validationStatus_.debugUtilsEnabled)
        appendUniqueInstanceExtension(requiredExtensions,
                                      VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (validationStatus_.actual == ValidationProfile::Sync ||
        validationStatus_.actual == ValidationProfile::Gpu) {
        appendUniqueInstanceExtension(
            requiredExtensions,
            VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    }
    createInfo.enabledExtensionCount =
        static_cast<uint32_t>(requiredExtensions.size());
    createInfo.ppEnabledExtensionNames = requiredExtensions.data();

    if (validationEnabled()) {
        createInfo.enabledLayerCount =
            static_cast<uint32_t>(validationLayers.size());
        createInfo.ppEnabledLayerNames = validationLayers.data();
    }

    ValidationFeatureConfig featureConfig =
        validationFeatureConfig(validationStatus_.actual);

    const void *pNext = nullptr;
    VkValidationFeaturesEXT validationFeatures{};
    if (!featureConfig.enabled.empty() ||
        !featureConfig.disabled.empty()) {
        validationFeatures.sType =
            VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
        validationFeatures.enabledValidationFeatureCount =
            static_cast<uint32_t>(featureConfig.enabled.size());
        validationFeatures.pEnabledValidationFeatures =
            featureConfig.enabled.data();
        validationFeatures.disabledValidationFeatureCount =
            static_cast<uint32_t>(featureConfig.disabled.size());
        validationFeatures.pDisabledValidationFeatures =
            featureConfig.disabled.data();
        validationFeatures.pNext = pNext;
        pNext = &validationFeatures;
    }

    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (validationStatus_.debugUtilsEnabled) {
        populateDebugMessengerCreateInfo(debugCreateInfo);
        debugCreateInfo.pNext = pNext;
        pNext = &debugCreateInfo;
    }
    createInfo.pNext = pNext;

    VK_CHECK(vkCreateInstance(&createInfo, nullptr, &instance_));
}

void VulkanContext::setupDebugMessenger() {
    if (!validationStatus_.debugUtilsEnabled)
        return;

    VkDebugUtilsMessengerCreateInfoEXT createInfo;
    populateDebugMessengerCreateInfo(createInfo);

    const VkResult result = CreateDebugUtilsMessengerEXT(
        instance_, &createInfo, nullptr, &debugMessenger_);
    if (result != VK_SUCCESS) {
        VKR_LOG_WARN("Vulkan",
                     "VK_EXT_debug_utils messenger unavailable ({})",
                     static_cast<int>(result));
    }
}

void VulkanContext::populateDebugMessengerCreateInfo(
    VkDebugUtilsMessengerCreateInfoEXT &createInfo) {
    createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = &VulkanContext::debugCallback;
    createInfo.pUserData = this;
}

ValidationStatus VulkanContext::validationStatus() const {
    ValidationStatus status = validationStatus_;
    status.warningCount =
        validationWarnings_.load(std::memory_order_relaxed);
    status.errorCount = validationErrors_.load(std::memory_order_relaxed);
    return status;
}

} // namespace vkr
