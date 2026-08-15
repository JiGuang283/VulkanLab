#pragma once

#include "ModelPrepareFactory.h"
#include "scene_data/SceneIds.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace vkr {

struct ProjectContext;
class SceneCatalog;

enum class ModelSourceKind {
    Gltf,
    Primitive,
};

struct ResolvedModelSource {
    ModelSourceKind kind = ModelSourceKind::Gltf;
    ModelAssetId id;
    std::string displayName;
    std::string profileId;
    std::filesystem::path sourcePath;
    ModelPrepareFactory prepareFactory;
    uint32_t textureLimit = 0;
    bool instanceable = false;
    bool available = false;
    std::string unavailableReason;
};

std::optional<ResolvedModelSource>
resolveModelSource(const SceneCatalog &catalog,
                   const ProjectContext &projectContext,
                   const ModelAssetId &modelId);

} // namespace vkr
