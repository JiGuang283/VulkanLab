#pragma once

#include "DerivedTextureManifest.h"

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
    TextureEncoder textureEncoder = TextureEncoder::Bc7;
};

struct EnvironmentArtifactStatusRequest {
    std::filesystem::path cacheRoot;
    std::filesystem::path sourcePath;
    std::string projectId;
    std::string environmentId;
    std::string profileId;
};

struct ArtifactStatus {
    ArtifactState state = ArtifactState::Missing;
    std::string reason;
    std::filesystem::path manifestPath;
    uint64_t entryCount = 0;
    uint64_t blobBytes = 0;
    std::string textureEncoder;
    std::string payloadKind;

    bool ready() const { return state == ArtifactState::Ready; }
};

ArtifactStatus inspectTextureArtifacts(const ArtifactStatusRequest &request);
ArtifactStatus inspectEnvironmentArtifacts(
    const EnvironmentArtifactStatusRequest &request);

} // namespace vkr
