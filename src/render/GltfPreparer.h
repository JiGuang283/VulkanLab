#pragma once

#include "scene/PreparedModelData.h"
#include "scene/SceneLoadTask.h"
#include "TextureTranscodeTarget.h"

#include <cstdint>
#include <optional>
#include <string>

namespace vkr {

struct SceneLoadStats;

class GltfPreparer {
  public:
    struct Options {
        bool     generateMissingNormals = true;
        uint32_t maxTextureSize = 2048;
        std::string derivedTextureCachePath = "derived_assets";
        std::string projectId;
        std::string sceneId;
        std::string profileId;
        TextureTranscodeTarget textureTranscodeTarget =
            TextureTranscodeTarget::Rgba8;
        bool requireDerivedTextures = false;
        std::optional<CameraPose> cameraOverride;
        SceneLoadStats *loadStats = nullptr;
    };

    static PreparedModelData prepare(
        const std::string &path, const Options &options,
        const CancellationToken &cancellation = {},
        SceneLoadProgress *progress = nullptr);
};

} // namespace vkr
