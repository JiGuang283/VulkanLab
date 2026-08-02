#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vkr::assettool {

struct CookPackageOptions {
    std::filesystem::path projectRoot;
    std::filesystem::path cacheRoot;
    std::filesystem::path runtimeDirectory;
    std::filesystem::path outputDirectory;
    std::string platform = "windows-x64";
    std::vector<std::string> sceneDocumentIds;
    std::optional<std::string> startupSceneId;
};

struct CookPackageReport {
    uint64_t sceneCount = 0;
    uint64_t modelCount = 0;
    uint64_t environmentCount = 0;
    uint64_t manifestCount = 0;
    uint64_t blobCount = 0;
    uint64_t fileCount = 0;
    uint64_t totalBytes = 0;
    std::filesystem::path outputDirectory;
};

CookPackageReport buildCookPackage(const CookPackageOptions &options);

} // namespace vkr::assettool
