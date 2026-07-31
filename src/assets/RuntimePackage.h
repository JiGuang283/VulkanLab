#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

struct RuntimePackageFile {
    std::string path;
    uint64_t bytes = 0;
    std::string sha256;
};

struct RuntimePackageManifest {
    static constexpr uint32_t kLegacySchemaVersion = 1;
    static constexpr uint32_t kSchemaVersion = 2;

    uint32_t schemaVersion = kSchemaVersion;
    std::string platform;
    std::string projectId;
    std::string profileId;
    std::string requiredTextureEncoder;
    std::string catalogPath = "assets/catalog.json";
    std::string cacheRoot = "runtime_assets";
    std::vector<RuntimePackageFile> files;
};

struct RuntimePackageVerification {
    uint64_t fileCount = 0;
    uint64_t totalBytes = 0;
};

bool saveRuntimePackageManifest(const std::filesystem::path &path,
                                const RuntimePackageManifest &manifest,
                                std::string &error);
bool loadRuntimePackageManifest(const std::filesystem::path &path,
                                RuntimePackageManifest &manifest,
                                std::string &error);
RuntimePackageVerification verifyRuntimePackage(
    const std::filesystem::path &packageRoot,
    const RuntimePackageManifest &manifest);
std::optional<std::filesystem::path> findRuntimePackageRoot(
    const std::filesystem::path &executablePath);

} // namespace vkr
