#include "core/ValidationProfile.h"

#include <stdexcept>
#include <string_view>

namespace {

void requireValidation(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

void testValidationProfileParser() {
    using vkr::ValidationProfile;
    requireValidation(vkr::parseValidationProfile("off") ==
                          ValidationProfile::Off,
                      "off validation profile was parsed incorrectly");
    requireValidation(vkr::parseValidationProfile("core") ==
                          ValidationProfile::Core,
                      "core validation profile was parsed incorrectly");
    requireValidation(vkr::parseValidationProfile("sync") ==
                          ValidationProfile::Sync,
                      "sync validation profile was parsed incorrectly");
    requireValidation(vkr::parseValidationProfile("gpu") ==
                          ValidationProfile::Gpu,
                      "gpu validation profile was parsed incorrectly");

    try {
        (void)vkr::parseValidationProfile("all");
    } catch (const std::invalid_argument &) {
        return;
    }
    throw std::runtime_error("invalid validation profile was accepted");
}

void testValidationFallbacks() {
    using vkr::ValidationEnvironment;
    using vkr::ValidationProfile;

    ValidationEnvironment available;
    available.layerAvailable = true;
    available.validationFeaturesAvailable = true;
    available.debugUtilsAvailable = true;
    available.loaderApiVersion = VK_API_VERSION_1_3;

    auto status =
        vkr::resolveValidationStatus(ValidationProfile::Gpu, available);
    requireValidation(status.actual == ValidationProfile::Gpu,
                      "available GPU validation unexpectedly fell back");
    requireValidation(status.debugUtilsEnabled,
                      "available debug utils were not enabled");
    requireValidation(vkr::validationInstanceApiVersion(status) ==
                          VK_API_VERSION_1_1,
                      "GPU validation did not request Vulkan 1.1");

    available.validationFeaturesAvailable = false;
    status = vkr::resolveValidationStatus(ValidationProfile::Sync, available);
    requireValidation(status.actual == ValidationProfile::Core,
                      "sync validation did not fall back to core");
    requireValidation(status.fallbackReason ==
                          "validation_features_unavailable",
                      "sync fallback reason is incorrect");

    available.validationFeaturesAvailable = true;
    available.loaderApiVersion = VK_API_VERSION_1_0;
    status = vkr::resolveValidationStatus(ValidationProfile::Gpu, available);
    requireValidation(status.actual == ValidationProfile::Core,
                      "GPU validation did not fall back on Vulkan 1.0");

    available.layerAvailable = false;
    status = vkr::resolveValidationStatus(ValidationProfile::Core, available);
    requireValidation(status.actual == ValidationProfile::Off,
                      "missing validation layer did not fall back to off");

    available.validationAllowed = false;
    available.layerAvailable = true;
    status = vkr::resolveValidationStatus(ValidationProfile::Gpu, available);
    requireValidation(status.actual == ValidationProfile::Off,
                      "cooked validation was not forced off");
    requireValidation(status.fallbackReason == "cooked_package_forces_off",
                      "cooked fallback reason is incorrect");
}

void testDebugUtilsIndependence() {
    vkr::ValidationEnvironment environment;
    environment.validationAllowed = true;
    environment.debugUtilsRequested = true;
    environment.debugUtilsAvailable = true;

    const auto status = vkr::resolveValidationStatus(
        vkr::ValidationProfile::Off, environment);
    requireValidation(status.actual == vkr::ValidationProfile::Off,
                      "off validation profile changed");
    requireValidation(status.debugUtilsEnabled,
                      "debug utils incorrectly depended on validation");
}

void testFeatureSelectionAndExtensionDeduplication() {
    const auto core =
        vkr::validationFeatureConfig(vkr::ValidationProfile::Core);
    requireValidation(core.enabled.empty() && core.disabled.empty(),
                      "core validation unexpectedly configured features");

    const auto sync =
        vkr::validationFeatureConfig(vkr::ValidationProfile::Sync);
    requireValidation(
        sync.enabled.size() == 1 &&
            sync.enabled.front() ==
                VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT &&
            sync.disabled.empty(),
        "sync validation feature selection is incorrect");

    const auto gpu =
        vkr::validationFeatureConfig(vkr::ValidationProfile::Gpu);
    requireValidation(
        gpu.enabled.size() == 2 &&
            gpu.enabled[0] ==
                VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT &&
            gpu.enabled[1] ==
                VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT &&
            gpu.disabled.size() == 1 &&
            gpu.disabled.front() ==
                VK_VALIDATION_FEATURE_DISABLE_CORE_CHECKS_EXT,
        "GPU validation feature selection is incorrect");

    std::vector<const char *> extensions = {"VK_A", "VK_B"};
    vkr::appendUniqueInstanceExtension(extensions, "VK_B");
    vkr::appendUniqueInstanceExtension(extensions, "VK_C");
    requireValidation(extensions.size() == 3 &&
                          std::string_view(extensions.back()) == "VK_C",
                      "instance extension de-duplication is incorrect");
}

} // namespace

void runValidationProfileTests() {
    testValidationProfileParser();
    testValidationFallbacks();
    testDebugUtilsIndependence();
    testFeatureSelectionAndExtensionDeduplication();
}
