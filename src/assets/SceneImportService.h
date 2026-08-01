#pragma once

#include "AssetValidation.h"
#include "ProjectContext.h"
#include "SceneCatalog.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace vkr {

enum class ModelImportPlacement { CopyIntoProject, ReferenceExisting };

struct ModelImportDependency {
    std::string uri;
    std::filesystem::path sourcePath;
    std::filesystem::path relativePath;
};

struct ModelImportPreflight {
    std::filesystem::path sourcePath;
    std::string suggestedDisplayName;
    std::string suggestedModelId;
    std::string suggestedSceneId; // Compatibility alias.
    std::vector<ModelImportDependency> dependencies;
    std::vector<std::string> extensionsUsed;
    std::vector<std::string> extensionsRequired;
    uint64_t totalBytes = 0;
};

struct ModelImportRequest {
    std::filesystem::path sourcePath;
    std::string displayName;
    std::string modelId;
    std::string sceneId; // Compatibility alias.
    std::string profileId;
    ModelImportPlacement placement = ModelImportPlacement::CopyIntoProject;
    std::optional<SceneValidationReceipt> validation;
    bool allowUnvalidated = false;

    std::string resolvedModelId() const {
        return modelId.empty() ? sceneId : modelId;
    }
};

struct ModelImportProgress {
    uint64_t completedBytes = 0;
    uint64_t totalBytes = 0;
    std::string currentFile;
};

struct ModelImportResult {
    CatalogModel model;
    CatalogModel scene; // Compatibility copy.
    std::filesystem::path projectSourcePath;
};

using ModelImportCancel = std::function<bool()>;
using ModelImportProgressCallback =
    std::function<void(const ModelImportProgress &)>;

class ModelImportService {
  public:
    static ModelImportPreflight
    preflight(const std::filesystem::path &sourcePath);

    static ModelImportResult
    importModel(const ProjectContext &project,
                const ModelImportRequest &request,
                const ModelImportCancel &cancel = {},
                const ModelImportProgressCallback &progress = {});

    static std::string suggestModelId(const std::string &name);

    static ModelImportResult
    importScene(const ProjectContext &project,
                const ModelImportRequest &request,
                const ModelImportCancel &cancel = {},
                const ModelImportProgressCallback &progress = {}) {
        return importModel(project, request, cancel, progress);
    }

    static std::string suggestSceneId(const std::string &name) {
        return suggestModelId(name);
    }
};

using SceneImportPlacement = ModelImportPlacement;
using SceneImportDependency = ModelImportDependency;
using SceneImportPreflight = ModelImportPreflight;
using SceneImportRequest = ModelImportRequest;
using SceneImportProgress = ModelImportProgress;
using SceneImportResult = ModelImportResult;
using SceneImportCancel = ModelImportCancel;
using SceneImportProgressCallback = ModelImportProgressCallback;
using SceneImportService = ModelImportService;

} // namespace vkr
