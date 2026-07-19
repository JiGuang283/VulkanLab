#pragma once

#include <cstdint>
#include <filesystem>

namespace vkr::assettool {

struct TextureCacheBuildOptions {
    std::filesystem::path scene;
    std::filesystem::path sceneProjectPath;
    std::string projectId;
    std::string sceneId;
    std::string profileId;
    uint32_t textureLimit = 0;
    std::filesystem::path cacheRoot;
    std::filesystem::path ktxTool;
    bool force = false;
};

int buildTextureCache(const TextureCacheBuildOptions &options);

struct TextureCacheMigrationOptions {
    std::filesystem::path projectRoot;
    std::filesystem::path legacyCacheRoot;
    std::filesystem::path cacheRoot;
};

int migrateTextureCache(const TextureCacheMigrationOptions &options);

} // namespace vkr::assettool
