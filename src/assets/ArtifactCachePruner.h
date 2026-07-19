#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace vkr {

struct ArtifactPruneCandidate {
    std::filesystem::path path;
    uint64_t bytes = 0;
    int64_t ageSeconds = 0;
};

struct ArtifactPruneOptions {
    std::filesystem::path cacheRoot;
    uint32_t olderThanDays = 7;
    bool execute = false;
    const std::atomic_bool *cancelRequested = nullptr;
};

struct ArtifactPruneReport {
    bool executed = false;
    uint64_t manifestFiles = 0;
    uint64_t protectedBlobs = 0;
    uint64_t scannedBlobFiles = 0;
    uint64_t scannedBlobBytes = 0;
    uint64_t deferredBlobFiles = 0;
    uint64_t deferredBlobBytes = 0;
    uint64_t deletedBlobFiles = 0;
    uint64_t deletedBlobBytes = 0;
    std::vector<ArtifactPruneCandidate> candidates;
};

ArtifactPruneReport pruneArtifactCache(const ArtifactPruneOptions &options);

} // namespace vkr
