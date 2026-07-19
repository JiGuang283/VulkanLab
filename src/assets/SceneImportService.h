#pragma once

#include "ProjectContext.h"
#include "SceneCatalog.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace vkr {

enum class SceneImportPlacement { CopyIntoProject, ReferenceExisting };

struct SceneImportDependency {
    std::string uri;
    std::filesystem::path sourcePath;
    std::filesystem::path relativePath;
};

struct SceneImportPreflight {
    std::filesystem::path sourcePath;
    std::string suggestedDisplayName;
    std::string suggestedSceneId;
    std::vector<SceneImportDependency> dependencies;
    uint64_t totalBytes = 0;
};

struct SceneImportRequest {
    std::filesystem::path sourcePath;
    std::string displayName;
    std::string sceneId;
    std::string profileId;
    SceneImportPlacement placement = SceneImportPlacement::CopyIntoProject;
};

struct SceneImportProgress {
    uint64_t completedBytes = 0;
    uint64_t totalBytes = 0;
    std::string currentFile;
};

struct SceneImportResult {
    CatalogScene scene;
    std::filesystem::path projectSourcePath;
};

using SceneImportCancel = std::function<bool()>;
using SceneImportProgressCallback =
    std::function<void(const SceneImportProgress &)>;

class SceneImportService {
  public:
    static SceneImportPreflight
    preflight(const std::filesystem::path &sourcePath);

    static SceneImportResult
    importScene(const ProjectContext &project,
                const SceneImportRequest &request,
                const SceneImportCancel &cancel = {},
                const SceneImportProgressCallback &progress = {});

    static std::string suggestSceneId(const std::string &name);
};

} // namespace vkr
