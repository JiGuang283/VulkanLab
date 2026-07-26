#pragma once

#include "assets/SceneCatalog.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

#include <glm/glm.hpp>

namespace vkr::assettool {

struct EnvironmentCacheBuildOptions {
    std::filesystem::path source;
    std::filesystem::path sourceProjectPath;
    std::filesystem::path cacheRoot;
    std::string projectId;
    std::string environmentId;
    EnvironmentProfile profile;
    bool force = false;
    uint32_t maxWorkers = 0;
    std::atomic_bool *cancelRequested = nullptr;
};

struct EnvironmentCacheBuildReport {
    uint64_t sourceBytes = 0;
    uint64_t blobBytes = 0;
    uint32_t generatedBlobs = 0;
    uint32_t reusedBlobs = 0;
    std::filesystem::path manifestPath;
};

glm::vec3 environmentCubeDirection(uint32_t face, float u, float v);
glm::vec2 environmentHammersley(uint32_t index, uint32_t count);

EnvironmentCacheBuildReport
buildEnvironmentCache(const EnvironmentCacheBuildOptions &options);

} // namespace vkr::assettool
