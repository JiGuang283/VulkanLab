#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

namespace vkr {

enum class ValidationProfile {
    Off,
    Core,
    Sync,
    Gpu,
};

const char *validationProfileName(ValidationProfile profile);
ValidationProfile parseValidationProfile(std::string_view value);

struct ValidationStatus {
    ValidationProfile requested = ValidationProfile::Core;
    ValidationProfile actual = ValidationProfile::Off;
    bool layerAvailable = false;
    bool validationFeaturesAvailable = false;
    bool debugUtilsAvailable = false;
    bool debugUtilsEnabled = false;
    std::string fallbackReason;
    uint64_t warningCount = 0;
    uint64_t errorCount = 0;
};

struct ValidationEnvironment {
    bool validationAllowed = true;
    bool debugUtilsRequested = true;
    bool layerAvailable = false;
    bool validationFeaturesAvailable = false;
    bool debugUtilsAvailable = false;
    uint32_t loaderApiVersion = VK_API_VERSION_1_0;
};

ValidationStatus
resolveValidationStatus(ValidationProfile requested,
                        const ValidationEnvironment &environment);

uint32_t validationInstanceApiVersion(const ValidationStatus &status);

struct ValidationFeatureConfig {
    std::vector<VkValidationFeatureEnableEXT> enabled;
    std::vector<VkValidationFeatureDisableEXT> disabled;
};

ValidationFeatureConfig
validationFeatureConfig(ValidationProfile actualProfile);

void appendUniqueInstanceExtension(std::vector<const char *> &extensions,
                                   const char *extension);

} // namespace vkr
