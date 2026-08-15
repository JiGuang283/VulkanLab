#pragma once

#include "core/TextureTranscodeTarget.h"

#include <cstdint>
#include <functional>
#include <string>

namespace vkr {

class CancellationToken;
struct PreparedModelData;
struct SceneLoadProgress;
struct SceneLoadStats;

struct SceneLoadContext {
    uint32_t maxTextureSize = 2048; // 0 = Full resolution
    std::string derivedTextureCachePath = "derived_assets";
    std::string projectId;
    std::string modelId;
    std::string sceneId;
    std::string profileId;
    TextureTranscodeTarget textureTranscodeTarget =
        TextureTranscodeTarget::Rgba8;
    bool requireDerivedTextures = false;
    SceneLoadStats *loadStats = nullptr;
};

using ModelPrepareFactory = std::function<PreparedModelData(
    const SceneLoadContext &, const CancellationToken &,
    SceneLoadProgress &)>;

} // namespace vkr
