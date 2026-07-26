#include "ValidationProfile.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace vkr {

const char *validationProfileName(ValidationProfile profile) {
    switch (profile) {
    case ValidationProfile::Off:
        return "off";
    case ValidationProfile::Core:
        return "core";
    case ValidationProfile::Sync:
        return "sync";
    case ValidationProfile::Gpu:
        return "gpu";
    }
    return "off";
}

ValidationProfile parseValidationProfile(std::string_view value) {
    if (value == "off")
        return ValidationProfile::Off;
    if (value == "core")
        return ValidationProfile::Core;
    if (value == "sync")
        return ValidationProfile::Sync;
    if (value == "gpu")
        return ValidationProfile::Gpu;
    throw std::invalid_argument(
        "--validation must be off, core, sync, or gpu");
}

ValidationStatus
resolveValidationStatus(ValidationProfile requested,
                        const ValidationEnvironment &environment) {
    ValidationStatus status;
    status.requested = requested;
    status.layerAvailable = environment.layerAvailable;
    status.validationFeaturesAvailable =
        environment.validationFeaturesAvailable;
    status.debugUtilsAvailable = environment.debugUtilsAvailable;
    status.debugUtilsEnabled =
        environment.debugUtilsRequested && environment.debugUtilsAvailable;

    if (!environment.validationAllowed) {
        status.actual = ValidationProfile::Off;
        if (requested != ValidationProfile::Off)
            status.fallbackReason = "cooked_package_forces_off";
        return status;
    }

    if (requested == ValidationProfile::Off) {
        status.actual = ValidationProfile::Off;
        return status;
    }

    if (!environment.layerAvailable) {
        status.actual = ValidationProfile::Off;
        status.fallbackReason = "validation_layer_unavailable";
        return status;
    }

    if ((requested == ValidationProfile::Sync ||
         requested == ValidationProfile::Gpu) &&
        !environment.validationFeaturesAvailable) {
        status.actual = ValidationProfile::Core;
        status.fallbackReason = "validation_features_unavailable";
        return status;
    }

    if (requested == ValidationProfile::Gpu &&
        VK_API_VERSION_MAJOR(environment.loaderApiVersion) == 1 &&
        VK_API_VERSION_MINOR(environment.loaderApiVersion) < 1) {
        status.actual = ValidationProfile::Core;
        status.fallbackReason = "gpu_validation_requires_vulkan_1_1";
        return status;
    }

    status.actual = requested;
    return status;
}

uint32_t validationInstanceApiVersion(const ValidationStatus &status) {
    return status.actual == ValidationProfile::Gpu ? VK_API_VERSION_1_1
                                                    : VK_API_VERSION_1_0;
}

ValidationFeatureConfig
validationFeatureConfig(ValidationProfile actualProfile) {
    ValidationFeatureConfig result;
    if (actualProfile == ValidationProfile::Sync) {
        result.enabled.push_back(
            VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
    } else if (actualProfile == ValidationProfile::Gpu) {
        result.enabled.push_back(
            VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT);
        result.enabled.push_back(
            VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT);
        result.disabled.push_back(
            VK_VALIDATION_FEATURE_DISABLE_CORE_CHECKS_EXT);
    }
    return result;
}

void appendUniqueInstanceExtension(std::vector<const char *> &extensions,
                                   const char *extension) {
    if (!extension || *extension == '\0')
        return;
    const auto existing = std::find_if(
        extensions.begin(), extensions.end(), [&](const char *value) {
            return value && std::strcmp(value, extension) == 0;
        });
    if (existing == extensions.end())
        extensions.push_back(extension);
}

} // namespace vkr
