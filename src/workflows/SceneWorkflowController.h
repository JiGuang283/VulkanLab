#pragma once

#include "app/Config.h"

#include "assets/AssetImportManager.h"
#include "assets/AssetLoadCoordinator.h"
#include "assets/ArtifactIndex.h"
#include "assets/ArtifactStatus.h"
#include "assets/AssetValidation.h"
#include "assets/ModelImportService.h"
#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"
#include "scene/SceneEntry.h"
#include "scene_data/SceneIds.h"
#include "workflows/SceneWorkflowTypes.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vkr {

class SceneWorkflowError : public std::runtime_error {
  public:
    SceneWorkflowError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code_(std::move(code)) {}

    const std::string &code() const noexcept { return code_; }

  private:
    std::string code_;
};

struct SceneWorkflowConfig {
    AssetImportMode importMode = AssetImportMode::ReadOnly;
    std::filesystem::path cacheRoot;
    std::filesystem::path assetToolPath;
    std::filesystem::path validatorPath;
    uint32_t importWorkers = 0;
    uint64_t importMemoryBudgetMiB = 2048;
    bool authoringCompiled = false;
};

struct SceneWorkflowCallbacks {
    std::function<uint64_t(int, bool, bool)> requestSceneLoad;
    std::function<bool()> hasUnsavedChanges;
    std::function<void(const ModelAssetId &,
                       const std::optional<std::string> &)>
        invalidateModel;
    std::function<void(const std::string &,
                       const std::optional<std::string> &)>
        invalidateEnvironment;
    std::function<void()> catalogRefreshed;
    std::function<void(const std::string &)> environmentArtifactsReady;
    std::function<void(const std::string &)> environmentWillBeRemoved;
    std::function<void(uint32_t)> textureLimitChanged;
};

class SceneWorkflowController {
  public:
    SceneWorkflowController(const ProjectContext &projectContext,
                            SceneCatalog catalog);
    ~SceneWorkflowController();

    SceneWorkflowController(const SceneWorkflowController &) = delete;
    SceneWorkflowController &
    operator=(const SceneWorkflowController &) = delete;

    void initialize(SceneWorkflowConfig config,
                    SceneWorkflowCallbacks callbacks);
    void shutdown();
    void pump();

    SceneCatalog &catalog() { return catalog_; }
    const SceneCatalog &catalog() const { return catalog_; }
    std::vector<SceneEntry> &entries() { return entries_; }
    const std::vector<SceneEntry> &entries() const { return entries_; }

    void refresh(const std::string &selectEntryId = {});
    int findEntryByName(const std::string &name) const;
    int findEntryById(const std::string &id) const;
    void selectEntry(int index);
    int selectedEntryIndex() const { return selectedEntryIndex_; }

    void setTextureLimit(uint32_t value);
    uint32_t textureLimit() const { return textureLimit_; }
    std::string profileIdForEntry(const SceneEntry &entry) const;

    uint64_t requestEntry(int index, bool sourceFallback = false,
                          bool loadAfter = true,
                          SceneWorkflowRequestReason reason =
                              SceneWorkflowRequestReason::SceneLoad,
                          bool forceReimport = false,
                          bool reloadAsset = false);
    uint64_t validateModel(int index);
    void savePreviewCamera(int index, const CameraPose &camera);
    void removeModel(int index);

    uint64_t beginModelImport(const std::filesystem::path &source);
    void confirmModelImport(const ModelImportPanelSubmission &submission);
    void dismissModelImport();
    bool cancelModelImport();

    void importEnvironment(const EnvironmentImportSubmission &submission);
    uint64_t buildEnvironment(const std::string &id, bool force);
    void removeEnvironment(const std::string &id);
    void setEnvironmentUiError(std::string message);
    void clearEnvironmentUiError();
    const std::string &environmentStatus() const {
        return environmentStatus_;
    }
    const std::string &environmentError() const { return environmentError_; }
    void reportStatus(std::string message);
    void reportError(std::string message);

    bool cancelAssetTask(uint64_t taskId);
    std::optional<AssetTaskSnapshot> assetTask(uint64_t taskId) const;
    std::optional<AssetTaskSnapshot> activeAssetTask() const;
    std::vector<AssetTaskSnapshot> recentAssetTasks(size_t limit = 8) const;
    uint64_t linkedSceneLoadTask(uint64_t importTaskId) const;
    bool assetAuthoringAvailable() const {
        return assetImportManager_ != nullptr;
    }
    bool isAssetTaskId(uint64_t taskId) const;

    void recordModelUse(const std::string &modelId,
                        const std::string &profileId);
    void recordEnvironmentUse(const std::string &environmentId,
                              const std::string &profileId);
    void reloadArtifactIndex();
    void persistArtifactIndex();
    void refreshArtifactStatus(int entryIndex, bool admission = false);
    void refreshAllArtifactStatuses();
    void refreshValidationStatus(int entryIndex);
    void refreshAllValidationStatuses();
    std::optional<ArtifactStatus> artifactStatus(int entryIndex) const;
    std::optional<ArtifactIndexRecord>
    artifactRecord(int entryIndex) const;
    std::optional<AssetValidationQuery>
    validationStatus(int entryIndex) const;
    std::filesystem::path artifactIndexPath() const;

    SceneWorkflowSnapshot
    snapshot(const SceneWorkflowRuntimeState &runtime) const;
    AssetWorkflowSnapshot assetSnapshot() const;
    void acknowledgeImportDialog();

  private:
    struct ModelImportWorkerState {
        std::atomic<bool> cancel{false};
        std::atomic<uint64_t> completedBytes{0};
        std::atomic<uint64_t> totalBytes{0};
        std::mutex mutex;
        std::string currentFile;
    };

    struct ModelImportOperation {
        std::future<ModelImportResult> importFuture;
        std::shared_ptr<AssetImportTask> validationTask;
        std::optional<ModelImportPreflight> preflight;
        std::optional<AssetValidationReport> validationReport;
        std::filesystem::path validationReportPath;
        std::shared_ptr<ModelImportWorkerState> worker;
        std::vector<std::string> profileIds;
        int profileIndex = 0;
        bool openDialog = false;
        bool loadAfterActiveImport = true;
        bool referenceExisting = false;
        std::string status;
        std::string error;
    };

    void processModelImport();
    void processAssetTasks();
    AssetTaskSnapshot
    makeAssetTaskSnapshot(const std::shared_ptr<AssetImportTask> &task) const;
    void validateEntryIndex(int index) const;
    std::string artifactKeyForEntry(const SceneEntry &entry) const;
    ProjectContext projectContext_;
    SceneCatalog catalog_;
    std::vector<SceneEntry> entries_;
    SceneWorkflowConfig config_;
    SceneWorkflowCallbacks callbacks_;
    uint32_t textureLimit_ = 2048;
    int selectedEntryIndex_ = -1;
    bool initialized_ = false;
    bool shutdown_ = false;

    std::unique_ptr<AssetImportManager> assetImportManager_;
    std::unique_ptr<ArtifactIndex> artifactIndex_;
    std::optional<ArtifactIndexUsage> artifactUsage_;
    AssetLoadCoordinator loadCoordinator_;
    std::unordered_map<uint64_t, uint64_t> importToLoadTask_;
    std::unordered_set<uint64_t> processedImports_;
    std::unordered_map<std::string, ArtifactStatus> artifactStatuses_;
    std::unordered_map<std::string, AssetValidationQuery>
        validationStatuses_;
    ModelImportOperation modelImport_;
    std::string status_;
    std::string error_;
    std::string environmentStatus_;
    std::string environmentError_;
};

} // namespace vkr
