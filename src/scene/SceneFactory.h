#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "render/TextureTranscodeTarget.h"

namespace vkr {

class Device;
class DescriptorAllocator;
class Scene;
class UploadContext;
class CancellationToken;
struct PreparedSceneData;
struct SceneLoadProgress;
struct SceneLoadStats;

struct SceneLoadContext {
    uint32_t maxTextureSize = 2048; // 0 = Full resolution
    std::string derivedTextureCachePath = "derived_assets";
    TextureTranscodeTarget textureTranscodeTarget =
        TextureTranscodeTarget::Rgba8;
    SceneLoadStats *loadStats = nullptr;
};

/// Constructs a scene given already-created core objects. The factory
/// captures by value anything it needs (paths, etc.).
///
/// Note: a Pipeline is intentionally NOT passed here. Scene factories create
/// material templates; Application builds the shared opaque pipeline from the
/// scene's primary template.
using SceneFactory = std::function<std::unique_ptr<Scene>(
    Device &, UploadContext &, DescriptorAllocator &,
    const SceneLoadContext &)>;

using ScenePrepareFactory = std::function<PreparedSceneData(
    const SceneLoadContext &, const CancellationToken &,
    SceneLoadProgress &)>;

struct SceneEntry {
    std::string         name;
    SceneFactory        factory;
    ScenePrepareFactory prepareFactory;

    bool supportsBackgroundPrepare() const {
        return static_cast<bool>(prepareFactory);
    }
};

} // namespace vkr
