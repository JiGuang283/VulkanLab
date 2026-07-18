#pragma once

#include "scene/PreparedSceneData.h"
#include "scene/SceneLoadTask.h"

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
        std::string vertShaderPath;
        std::string fragShaderPath;
        std::optional<CameraPose> cameraOverride;
        SceneLoadStats *loadStats = nullptr;
    };

    static PreparedSceneData prepare(
        const std::string &path, const Options &options,
        const CancellationToken &cancellation = {},
        SceneLoadProgress *progress = nullptr);
};

} // namespace vkr
