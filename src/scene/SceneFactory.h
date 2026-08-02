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
struct PreparedModelData;
using PreparedSceneData = PreparedModelData;
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

/// Constructs a scene given already-created core objects. The factory
/// captures by value anything it needs (paths, etc.).
///
/// Note: a Pipeline is intentionally NOT passed here. Scene factories create
/// material templates; Application builds the shared opaque pipeline from the
/// scene's primary template.
using SceneFactory = std::function<std::unique_ptr<Scene>(
    Device &, UploadContext &, DescriptorAllocator &,
    const SceneLoadContext &)>;

using ModelPrepareFactory = std::function<PreparedModelData(
    const SceneLoadContext &, const CancellationToken &,
    SceneLoadProgress &)>;

using ScenePrepareFactory = ModelPrepareFactory;

enum class SceneEntryKind {
    ModelPreview,
    NativeScene,
};

const char *sceneEntryKindName(SceneEntryKind kind);

struct SceneEntry {
    SceneEntryKind kind = SceneEntryKind::ModelPreview;
    std::string         name;
    SceneFactory        factory;
    ScenePrepareFactory prepareFactory;
    std::string         id;
    std::string         profileId;
    std::string         sourcePath;
    bool                builtin = false;
    bool                available = true;
    std::string         unavailableReason;

    bool supportsBackgroundPrepare() const {
        return static_cast<bool>(prepareFactory);
    }
    bool isModelPreview() const {
        return kind == SceneEntryKind::ModelPreview;
    }
    bool isNativeScene() const {
        return kind == SceneEntryKind::NativeScene;
    }
};

} // namespace vkr
