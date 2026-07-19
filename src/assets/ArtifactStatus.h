#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace vkr {

enum class ArtifactState { Ready, Missing, Stale, Invalid, Importing };

const char *artifactStateName(ArtifactState state);

struct ArtifactStatusRequest {
    std::filesystem::path cacheRoot;
    std::filesystem::path scenePath;
    std::string projectId;
    std::string sceneId;
    std::string profileId;
    uint32_t textureLimit = 0;
};

struct ArtifactStatus {
    ArtifactState state = ArtifactState::Missing;
    std::string reason;
    std::filesystem::path manifestPath;
    uint64_t entryCount = 0;
    uint64_t blobBytes = 0;

    bool ready() const { return state == ArtifactState::Ready; }
};

ArtifactStatus inspectTextureArtifacts(const ArtifactStatusRequest &request);

} // namespace vkr
