#pragma once

#include "ProcessRunner.h"

#include "assets/AssetValidation.h"
#include "assets/SceneImportService.h"

#include <atomic>
#include <filesystem>
#include <string>

namespace vkr::assettool {

inline constexpr const char *kGltfValidatorVersion =
    "2.0.0-dev.3.10";

struct GltfValidationOptions {
    std::filesystem::path sourcePath;
    std::filesystem::path cacheRoot;
    std::filesystem::path validatorPath;
    bool force = false;
    bool requireExecutable = false;
};

struct GltfValidationResult {
    SceneImportPreflight preflight;
    AssetValidationReport report;
    std::filesystem::path reportPath;
    bool reused = false;
};

std::filesystem::path locateGltfValidator(
    const std::filesystem::path &explicitPath,
    IProcessRunner &processRunner, std::string &reason);

GltfValidationResult validateGltf(
    const GltfValidationOptions &options,
    const std::atomic_bool &cancelRequested,
    IProcessRunner &processRunner);

} // namespace vkr::assettool
