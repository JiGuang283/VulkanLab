#pragma once

#include <cstdint>
#include <filesystem>

namespace vkr::assettool {

struct TextureCacheBuildOptions {
    std::filesystem::path scene;
    uint32_t textureLimit = 0;
    std::filesystem::path cacheRoot = "derived_assets";
    std::filesystem::path ktxTool;
    bool force = false;
};

int buildTextureCache(const TextureCacheBuildOptions &options);

} // namespace vkr::assettool
