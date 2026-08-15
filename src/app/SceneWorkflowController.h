#pragma once

#include "assets/AssetImportManager.h"
#include "assets/AssetLoadCoordinator.h"
#include "assets/ArtifactStatus.h"
#include "assets/AssetValidation.h"
#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"
#include "assets/ModelImportService.h"
#include "scene/SceneEntry.h"
#include "workflows/SceneWorkflowTypes.h"

#include <array>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vkr {

struct ModelImportWorkerState {
    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> completedBytes{0};
    std::atomic<uint64_t> totalBytes{0};
    std::mutex mutex;
    std::string currentFile;
};

struct ModelImportUiState {
    std::future<ModelImportResult> importFuture;
    std::shared_ptr<AssetImportTask> validationTask;
    std::optional<ModelImportPreflight> preflight;
    std::optional<AssetValidationReport> validationReport;
    std::filesystem::path validationReportPath;
    std::shared_ptr<ModelImportWorkerState> worker;
    std::array<char, 192> displayName{};
    std::array<char, 128> modelId{};
    std::vector<std::string> profileIds;
    int profileIndex = 0;
    bool requestOpenModal = false;
    bool referenceExisting = false;
    bool loadAfterImport = true;
    bool loadAfterActiveImport = true;
    bool allowUnvalidated = false;
    std::string status;
    std::string error;

    ModelImportUiState() = default;
    ModelImportUiState(const ModelImportUiState &) = delete;
    ModelImportUiState &operator=(const ModelImportUiState &) = delete;
};

struct SceneAssetOperationState {
    AssetLoadCoordinator coordinator;
    std::unordered_map<uint64_t, uint64_t> importToLoadTask;
    std::unordered_set<uint64_t> processedImports;
    std::unordered_map<std::string, ArtifactStatus> statuses;
    std::unordered_map<std::string, AssetValidationQuery> validationStatuses;
    int selectedSceneIndex = -1;
    std::array<char, 128> search{};
    std::string status;
    std::string error;
};

class SceneWorkflowController {
  public:
    SceneWorkflowController(const ProjectContext &projectContext,
                            SceneCatalog catalog);

    SceneCatalog &catalog() { return catalog_; }
    const SceneCatalog &catalog() const { return catalog_; }
    std::vector<SceneEntry> &entries() { return entries_; }
    const std::vector<SceneEntry> &entries() const { return entries_; }
    ModelImportUiState &modelImport() { return modelImport_; }
    SceneAssetOperationState &assetOperations() { return assetOperations_; }

    void refresh(const ProjectContext &projectContext);
    int findEntryByName(const std::string &name) const;
    int findEntryById(const std::string &id) const;
    SceneWorkflowSnapshot snapshot() const;

  private:
    SceneCatalog catalog_;
    std::vector<SceneEntry> entries_;
    ModelImportUiState modelImport_;
    SceneAssetOperationState assetOperations_;
};

} // namespace vkr
