#pragma once

#include "assets/DerivedTextureManifest.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace vkr::assettool {

class IProcessRunner;

struct TextureCacheBuildOptions {
    std::filesystem::path scene;
    std::filesystem::path sceneProjectPath;
    std::string projectId;
    std::string sceneId;
    std::string profileId;
    uint32_t textureLimit = 0;
    std::filesystem::path cacheRoot;
    std::filesystem::path ktxTool;
    std::string qualityPreset = "development";
    TextureEncoder textureEncoder = TextureEncoder::Bc7;
    uint32_t maxWorkers = 0;
    uint64_t memoryBudgetMiB = 2048;
    bool progressNdjson = false;
    bool force = false;
    std::atomic_bool *cancelRequested = nullptr;
};

int buildTextureCache(const TextureCacheBuildOptions &options);
int buildTextureCache(const TextureCacheBuildOptions &options,
                      IProcessRunner &processRunner);

struct TextureCacheMigrationOptions {
    std::filesystem::path projectRoot;
    std::filesystem::path legacyCacheRoot;
    std::filesystem::path cacheRoot;
};

int migrateTextureCache(const TextureCacheMigrationOptions &options);

} // namespace vkr::assettool
