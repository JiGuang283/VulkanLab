#include "SceneWorkflowController.h"

#include <RuntimeFeatures.h>

#include "core/Log.h"
#include "diagnostics/Profiling.h"
#include "assets/SceneCatalogEditor.h"
#include "scene/SceneRegistryBuilder.h"
#include "scene_data/PrimitiveModelDefinitions.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <system_error>

namespace vkr {
namespace {

bool asciiEqualsIgnoreCase(const std::string &left,
                           const std::string &right) {
    if (left.size() != right.size())
        return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

const char *importModeName(AssetImportMode mode) {
    switch (mode) {
    case AssetImportMode::OnDemand:
        return "OnDemand";
    case AssetImportMode::ReadOnly:
        return "ReadOnly";
    case AssetImportMode::CookedOnly:
        return "CookedOnly";
    }
    return "Unknown";
}

ImportReason importReason(SceneWorkflowRequestReason reason) {
    switch (reason) {
    case SceneWorkflowRequestReason::SceneLoad:
        return ImportReason::SceneLoad;
    case SceneWorkflowRequestReason::SceneRegistration:
        return ImportReason::SceneRegistration;
    case SceneWorkflowRequestReason::ManualReimport:
        return ImportReason::ManualReimport;
    }
    return ImportReason::SceneLoad;
}

std::string statusKey(const std::string &assetId,
                      const std::string &profileId) {
    return assetId + '\n' + profileId;
}

} // namespace

SceneWorkflowController::SceneWorkflowController(
    const ProjectContext &projectContext, SceneCatalog catalog)
    : projectContext_(projectContext), catalog_(std::move(catalog)),
      entries_(buildSceneRegistry(catalog_, projectContext_)) {}

SceneWorkflowController::~SceneWorkflowController() { shutdown(); }

void SceneWorkflowController::initialize(
    SceneWorkflowConfig config, SceneWorkflowCallbacks callbacks) {
    if (initialized_)
        throw std::logic_error("SceneWorkflowController is already initialized");
    config_ = std::move(config);
    callbacks_ = std::move(callbacks);
    shutdown_ = false;
    initialized_ = true;

    if (config_.authoringCompiled &&
        config_.importMode == AssetImportMode::OnDemand) {
        assetImportManager_ = std::make_unique<AssetImportManager>(
            AssetImportManagerOptions{
                projectContext_.projectRoot, config_.cacheRoot,
                config_.assetToolPath, config_.importWorkers,
                config_.importMemoryBudgetMiB, config_.validatorPath});
    }
    reloadArtifactIndex();
    refreshAllArtifactStatuses();
    refreshAllValidationStatuses();
}

void SceneWorkflowController::shutdown() {
    if (shutdown_)
        return;
    shutdown_ = true;
    if (modelImport_.worker)
        modelImport_.worker->cancel = true;
    if (modelImport_.validationTask && assetImportManager_)
        assetImportManager_->cancel(modelImport_.validationTask->id);
    if (modelImport_.importFuture.valid())
        modelImport_.importFuture.wait();
    if (assetImportManager_)
        assetImportManager_->shutdown();
}

void SceneWorkflowController::pump() {
    if (shutdown_)
        return;
    VKL_PROFILE_ZONE("Scene Workflow Pump");
    processModelImport();
    processAssetTasks();
}

void SceneWorkflowController::refresh(const std::string &selectEntryId) {
    std::unordered_map<std::string, std::string> previousModelSources;
    for (const SceneEntry &entry : entries_) {
        if (entry.prepareFactory)
            previousModelSources.emplace(entry.id, entry.sourcePath);
    }

    std::string selectedId = selectEntryId;
    if (selectedId.empty() && selectedEntryIndex_ >= 0 &&
        selectedEntryIndex_ < static_cast<int>(entries_.size())) {
        selectedId = entries_[selectedEntryIndex_].id;
    }

    catalog_ = SceneCatalog::load(projectContext_.catalogPath,
                                  projectContext_.projectRoot);
    entries_ = buildSceneRegistry(catalog_, projectContext_);
    for (const auto &[modelId, sourcePath] : previousModelSources) {
        const auto found = std::find_if(
            entries_.begin(), entries_.end(), [&](const SceneEntry &entry) {
                return entry.id == modelId;
            });
        if (found == entries_.end() || found->sourcePath != sourcePath) {
            if (callbacks_.invalidateModel)
                callbacks_.invalidateModel(ModelAssetId(modelId),
                                           std::nullopt);
        }
    }

    selectedEntryIndex_ = findEntryById(selectedId);
    reloadArtifactIndex();
    refreshAllArtifactStatuses();
    refreshAllValidationStatuses();
    if (callbacks_.catalogRefreshed)
        callbacks_.catalogRefreshed();
}

int SceneWorkflowController::findEntryByName(const std::string &name) const {
    for (int index = 0; index < static_cast<int>(entries_.size()); ++index) {
        if (asciiEqualsIgnoreCase(entries_[index].name, name))
            return index;
    }
    return -1;
}

int SceneWorkflowController::findEntryById(const std::string &id) const {
    if (id.empty())
        return -1;
    for (int index = 0; index < static_cast<int>(entries_.size()); ++index) {
        if (asciiEqualsIgnoreCase(entries_[index].id, id))
            return index;
    }
    return -1;
}

void SceneWorkflowController::selectEntry(int index) {
    validateEntryIndex(index);
    selectedEntryIndex_ = index;
}

void SceneWorkflowController::setTextureLimit(uint32_t value) {
    if (textureLimit_ == value)
        return;
    textureLimit_ = value;
    refreshAllArtifactStatuses();
}

std::string SceneWorkflowController::profileIdForEntry(
    const SceneEntry &entry) const {
    if (!entry.isModelPreview())
        return {};
    const auto preferred = catalog_.importProfiles.find(entry.profileId);
    if (preferred != catalog_.importProfiles.end() &&
        preferred->second.textureLimit == textureLimit_) {
        return preferred->first;
    }
    for (const auto &candidate : catalog_.importProfiles) {
        if (candidate.second.textureLimit == textureLimit_)
            return candidate.first;
    }
    return textureLimit_ == 0 ? std::string("runtime_full")
                              : std::string("runtime_") +
                                    std::to_string(textureLimit_);
}

uint64_t SceneWorkflowController::requestEntry(
    int index, bool sourceFallback, bool loadAfter,
    SceneWorkflowRequestReason reason,
    bool forceReimport, bool reloadAsset) {
    validateEntryIndex(index);
    if (callbacks_.hasUnsavedChanges && callbacks_.hasUnsavedChanges()) {
        throw SceneWorkflowError(
            "unsaved_changes",
            "The active native scene has unsaved changes.");
    }

    const SceneEntry &entry = entries_[index];
    if (!entry.available)
        throw SceneWorkflowError("scene_unavailable",
                                 entry.unavailableReason);

    const uint64_t generation = loadCoordinator_.beginOperation();
    selectedEntryIndex_ = index;
    error_.clear();

    if (entry.isNativeScene()) {
        if (!loadAfter)
            return 0;
        if (!callbacks_.requestSceneLoad)
            throw std::logic_error("Scene runtime load callback is missing");
        return callbacks_.requestSceneLoad(index, false, reloadAsset);
    }

    if (sourceFallback) {
        if (config_.importMode == AssetImportMode::CookedOnly) {
            throw SceneWorkflowError(
                "source_fallback_disabled",
                "Source fallback is disabled in CookedOnly mode.");
        }
        if (!loadAfter)
            return 0;
        if (!callbacks_.requestSceneLoad)
            throw std::logic_error("Scene runtime load callback is missing");
        return callbacks_.requestSceneLoad(index, true, reloadAsset);
    }

    refreshArtifactStatus(index, true);
    const std::string profileId = profileIdForEntry(entry);
    const auto found = artifactStatuses_.find(
        statusKey(entry.id, profileId));
    const bool ready = found != artifactStatuses_.end() &&
                       found->second.ready();
    if (ready && !forceReimport) {
        if (!loadAfter)
            return 0;
        if (!callbacks_.requestSceneLoad)
            throw std::logic_error("Scene runtime load callback is missing");
        return callbacks_.requestSceneLoad(index, false, reloadAsset);
    }

    if (config_.importMode != AssetImportMode::OnDemand) {
        throw SceneWorkflowError(
            "artifact_not_ready",
            "Derived artifacts are not ready for scene '" + entry.name +
                "' (mode=" + importModeName(config_.importMode) + "). " +
                (found == artifactStatuses_.end()
                     ? std::string("No artifact status is available.")
                     : found->second.reason));
    }
    if (!assetImportManager_) {
        throw SceneWorkflowError(
            "feature_not_compiled",
            "Asset authoring support is unavailable in this build.");
    }

    const std::shared_ptr<AssetImportTask> task = assetImportManager_->request(
        {entry.id, profileId, importReason(reason), forceReimport});
    if (loadAfter)
        loadCoordinator_.attach(task->id, generation, index);
    refreshArtifactStatus(index);
    status_ = "Importing " + entry.name + " (" + profileId + ")";
    VKR_LOG_INFO("Assets", "Queued import task {} for model '{}' profile '{}'",
                 task->id, entry.id, profileId);
    return task->id;
}

uint64_t SceneWorkflowController::validateModel(int index) {
    validateEntryIndex(index);
    if (!assetImportManager_)
        throw SceneWorkflowError("feature_not_compiled",
                                 "Asset validation is unavailable.");
    const SceneEntry &entry = entries_[index];
    if (!entry.isModelPreview())
        throw SceneWorkflowError("scene_not_catalog",
                                 "Validation applies only to Catalog models.");
    AssetImportRequest request;
    request.sceneId = entry.id;
    request.profileId = "validation";
    request.kind = AssetImportKind::SceneValidation;
    request.force = true;
    const auto task = assetImportManager_->request(request);
    status_ = "Validating " + entry.name + " (task " +
              std::to_string(task->id) + ")";
    error_.clear();
    return task->id;
}

void SceneWorkflowController::savePreviewCamera(
    int index, const CameraPose &camera) {
#if !VKL_ENABLE_ASSET_AUTHORING
    (void)index;
    (void)camera;
    throw SceneWorkflowError("feature_not_compiled",
                             "Asset authoring support is unavailable.");
#else
    validateEntryIndex(index);
    const SceneEntry &entry = entries_[index];
    if (!entry.isModelPreview())
        throw SceneWorkflowError("scene_not_catalog",
                                 "Preview cameras apply only to models.");
    SceneCatalogEditor::saveModelPreviewCamera(projectContext_, entry.id,
                                               camera);
    status_ = "Saved preview camera for " + entry.name;
    refresh(entry.id);
#endif
}

void SceneWorkflowController::removeModel(int index) {
#if !VKL_ENABLE_ASSET_AUTHORING
    (void)index;
    throw SceneWorkflowError("feature_not_compiled",
                             "Asset authoring support is unavailable.");
#else
    validateEntryIndex(index);
    const SceneEntry entry = entries_[index];
    if (!entry.isModelPreview())
        throw SceneWorkflowError("scene_not_catalog",
                                 "Only Catalog models can be removed here.");
    SceneCatalogEditor::removeModel(projectContext_, entry.id);
    status_ = "Removed " + entry.name + " from Catalog";
    refresh();
#endif
}

uint64_t SceneWorkflowController::beginModelImport(
    const std::filesystem::path &source) {
    if (!assetImportManager_)
        throw SceneWorkflowError("feature_not_compiled",
                                 "Asset authoring support is unavailable.");
    modelImport_.error.clear();
    modelImport_.preflight.reset();
    modelImport_.validationReport.reset();
    modelImport_.validationReportPath.clear();
    AssetImportRequest request;
    request.kind = AssetImportKind::SceneValidation;
    request.profileId = "validation";
    request.sourcePath = source;
    modelImport_.validationTask = assetImportManager_->request(request);
    modelImport_.status = "Validating model and local dependencies...";
    return modelImport_.validationTask->id;
}

void SceneWorkflowController::confirmModelImport(
    const ModelImportPanelSubmission &submission) {
#if !VKL_ENABLE_ASSET_AUTHORING
    (void)submission;
    throw SceneWorkflowError("feature_not_compiled",
                             "Asset authoring support is unavailable.");
#else
    if (!modelImport_.preflight || !modelImport_.validationReport)
        throw SceneWorkflowError("validation_required",
                                 "Model validation has not completed.");
    if (modelImport_.importFuture.valid())
        throw SceneWorkflowError("import_busy",
                                 "Another model copy is already active.");

    ModelImportRequest request;
    request.sourcePath = modelImport_.preflight->sourcePath;
    request.displayName = submission.displayName;
    request.modelId = submission.modelId;
    request.profileId = submission.profileId;
    request.placement = submission.referenceExisting
                            ? ModelImportPlacement::ReferenceExisting
                            : ModelImportPlacement::CopyIntoProject;
    request.validation =
        sceneValidationReceipt(*modelImport_.validationReport);
    request.allowUnvalidated = submission.allowUnvalidated;
    modelImport_.loadAfterActiveImport = submission.loadAfterImport;
    modelImport_.worker = std::make_shared<ModelImportWorkerState>();
    modelImport_.worker->totalBytes = modelImport_.preflight->totalBytes;
    const auto worker = modelImport_.worker;
    ProjectContext project = projectContext_;
    project.cacheRoot = config_.cacheRoot;
    modelImport_.importFuture = std::async(
        std::launch::async, [project, request, worker] {
            profileSetThreadName("ModelImportCopy");
            VKL_PROFILE_ZONE("Model Catalog Import");
            return ModelImportService::importModel(
                project, request,
                [worker] { return worker->cancel.load(); },
                [worker](const ModelImportProgress &progress) {
                    worker->completedBytes = progress.completedBytes;
                    worker->totalBytes = progress.totalBytes;
                    std::lock_guard<std::mutex> lock(worker->mutex);
                    worker->currentFile = progress.currentFile;
                });
        });
    modelImport_.status = "Importing model source files...";
    modelImport_.error.clear();
#endif
}

void SceneWorkflowController::dismissModelImport() {
    modelImport_.preflight.reset();
    modelImport_.validationReport.reset();
    modelImport_.validationReportPath.clear();
    modelImport_.openDialog = false;
}

bool SceneWorkflowController::cancelModelImport() {
    bool cancelled = false;
    if (modelImport_.validationTask && assetImportManager_)
        cancelled = assetImportManager_->cancel(
                        modelImport_.validationTask->id) ||
                    cancelled;
    if (modelImport_.worker) {
        modelImport_.worker->cancel = true;
        cancelled = true;
    }
    return cancelled;
}

void SceneWorkflowController::importEnvironment(
    const EnvironmentImportSubmission &submission) {
#if !VKL_ENABLE_ASSET_AUTHORING
    (void)submission;
    throw SceneWorkflowError("feature_not_compiled",
                             "Asset authoring support is unavailable.");
#else
    const std::filesystem::path relative =
        std::filesystem::path("assets/environments") /
        submission.environmentId / submission.source.filename();
    const std::filesystem::path destination =
        (projectContext_.projectRoot / relative).lexically_normal();
    if (!pathIsWithin(projectContext_.projectRoot, destination))
        throw SceneWorkflowError(
            "invalid_environment_path",
            "Environment destination escapes project root.");
    if (catalog_.findEnvironment(submission.environmentId))
        throw SceneWorkflowError(
            "environment_exists",
            "Environment ID already exists in the Catalog.");
    if (std::filesystem::exists(destination))
        throw SceneWorkflowError(
            "environment_destination_exists",
            "Environment destination already exists: " +
                destination.string());

    std::filesystem::create_directories(destination.parent_path());
    std::filesystem::copy_file(submission.source, destination,
                               std::filesystem::copy_options::none);
    CatalogEnvironment environment;
    environment.id = submission.environmentId;
    environment.displayName = submission.displayName;
    environment.source = relative;
    environment.environmentProfile = submission.profileId;
    try {
        SceneCatalogEditor::addEnvironment(projectContext_, environment);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
        throw;
    }
    refresh();
    environmentStatus_ = "Imported " + submission.displayName +
                         "; build derived IBL artifacts next";
    environmentError_.clear();
#endif
}

uint64_t SceneWorkflowController::buildEnvironment(const std::string &id,
                                                   bool force) {
    if (!assetImportManager_)
        throw SceneWorkflowError("feature_not_compiled",
                                 "Asset authoring support is unavailable.");
    const CatalogEnvironment *environment = catalog_.findEnvironment(id);
    if (!environment)
        throw SceneWorkflowError(
            "unknown_environment",
            "Environment is no longer present in Catalog.");
    const auto task = assetImportManager_->request(
        {environment->id, environment->environmentProfile,
         ImportReason::ManualReimport, force, AssetImportKind::Environment});
    environmentStatus_ =
        "Queued environment bake for " + environment->displayName;
    environmentError_.clear();
    return task->id;
}

void SceneWorkflowController::removeEnvironment(const std::string &id) {
#if !VKL_ENABLE_ASSET_AUTHORING
    (void)id;
    throw SceneWorkflowError("feature_not_compiled",
                             "Asset authoring support is unavailable.");
#else
    if (callbacks_.environmentWillBeRemoved)
        callbacks_.environmentWillBeRemoved(id);
    SceneCatalogEditor::removeEnvironment(projectContext_, id);
    environmentStatus_ = "Removed " + id + " from Catalog";
    environmentError_.clear();
    refresh();
#endif
}

void SceneWorkflowController::setEnvironmentUiError(std::string message) {
    environmentError_ = std::move(message);
}

void SceneWorkflowController::clearEnvironmentUiError() {
    environmentError_.clear();
}

void SceneWorkflowController::reportStatus(std::string message) {
    status_ = std::move(message);
    error_.clear();
}

void SceneWorkflowController::reportError(std::string message) {
    error_ = std::move(message);
}

bool SceneWorkflowController::cancelAssetTask(uint64_t taskId) {
    return assetImportManager_ && assetImportManager_->cancel(taskId);
}

std::optional<AssetTaskSnapshot>
SceneWorkflowController::assetTask(uint64_t taskId) const {
    if (!assetImportManager_)
        return std::nullopt;
    const auto task = assetImportManager_->task(taskId);
    return task ? std::optional<AssetTaskSnapshot>(makeAssetTaskSnapshot(task))
                : std::nullopt;
}

std::optional<AssetTaskSnapshot>
SceneWorkflowController::activeAssetTask() const {
    if (!assetImportManager_)
        return std::nullopt;
    const auto task = assetImportManager_->activeTask();
    return task ? std::optional<AssetTaskSnapshot>(makeAssetTaskSnapshot(task))
                : std::nullopt;
}

std::vector<AssetTaskSnapshot>
SceneWorkflowController::recentAssetTasks(size_t limit) const {
    std::vector<AssetTaskSnapshot> result;
    if (!assetImportManager_)
        return result;
    const auto history = assetImportManager_->history();
    const size_t count = std::min(limit, history.size());
    result.reserve(count);
    for (size_t index = 0; index < count; ++index)
        result.push_back(makeAssetTaskSnapshot(history[index]));
    return result;
}

uint64_t
SceneWorkflowController::linkedSceneLoadTask(uint64_t importTaskId) const {
    const auto found = importToLoadTask_.find(importTaskId);
    return found == importToLoadTask_.end() ? 0 : found->second;
}

bool SceneWorkflowController::isAssetTaskId(uint64_t taskId) const {
    return (taskId & AssetImportManager::kTaskIdMask) != 0;
}

void SceneWorkflowController::recordModelUse(
    const std::string &modelId, const std::string &profileId) {
    if (!artifactIndex_ || profileId.empty())
        return;
    const auto profile = catalog_.importProfiles.find(profileId);
    if (profile == catalog_.importProfiles.end())
        return;
    artifactIndex_->touch(modelId, profileId);
    persistArtifactIndex();
}

void SceneWorkflowController::recordEnvironmentUse(
    const std::string &environmentId, const std::string &profileId) {
    if (!artifactIndex_)
        return;
    artifactIndex_->touchEnvironment(environmentId, profileId);
    persistArtifactIndex();
}

void SceneWorkflowController::reloadArtifactIndex() {
    try {
        bool rebuilt = false;
        std::string diagnostic;
        artifactIndex_ = std::make_unique<ArtifactIndex>(
            ArtifactIndex::loadOrRebuild(
                config_.cacheRoot, projectContext_.projectRoot, catalog_,
                &rebuilt, &diagnostic));
        artifactUsage_ = artifactIndex_->usage();
        VKR_LOG_INFO("Assets", "Artifact index {} at {} ({} records)",
                     rebuilt ? "rebuilt" : "loaded",
                     artifactIndex_->path().string(),
                     artifactIndex_->records().size());
        if (rebuilt && diagnostic != "index not found")
            VKR_LOG_WARN("Assets", "Artifact index rebuild reason: {}",
                         diagnostic);
    } catch (const std::exception &error) {
        artifactIndex_.reset();
        artifactUsage_.reset();
        VKR_LOG_WARN("Assets",
                     "Artifact index unavailable; using manifest scans: {}",
                     error.what());
    }
}

void SceneWorkflowController::persistArtifactIndex() {
    if (!artifactIndex_ || config_.importMode == AssetImportMode::CookedOnly)
        return;
    try {
        artifactIndex_->save();
        artifactUsage_ = artifactIndex_->usage();
    } catch (const std::exception &error) {
        VKR_LOG_WARN("Assets", "Could not persist ArtifactIndex: {}",
                     error.what());
    }
}

void SceneWorkflowController::refreshArtifactStatus(int entryIndex,
                                                    bool admission) {
    if (entryIndex < 0 || entryIndex >= static_cast<int>(entries_.size()))
        return;
    const SceneEntry &entry = entries_[entryIndex];
    if (!entry.isModelPreview())
        return;
    const std::string profileId = profileIdForEntry(entry);
    ArtifactStatus status;
    if (!entry.available) {
        status.state = ArtifactState::Missing;
        status.reason = entry.unavailableReason;
    } else {
        const auto profile = catalog_.importProfiles.find(profileId);
        if (profile == catalog_.importProfiles.end()) {
            status.state = ArtifactState::Missing;
            status.reason = "No Catalog import profile matches texture limit " +
                            std::to_string(textureLimit_);
        } else {
            const auto encoder =
                textureEncoderFromName(profile->second.textureEncoder);
            if (!encoder) {
                status.state = ArtifactState::Invalid;
                status.reason = "Unknown texture encoder '" +
                                profile->second.textureEncoder + "'";
            } else {
                const ArtifactStatusRequest request{
                    config_.cacheRoot,
                    std::filesystem::u8path(entry.sourcePath),
                    catalog_.projectId,
                    entry.id,
                    profileId,
                    textureLimit_,
                    *encoder};
                status = artifactIndex_
                             ? artifactIndex_->query(
                                   request,
                                   admission
                                       ? ArtifactValidationMode::Admission
                                       : ArtifactValidationMode::Fast)
                             : inspectTextureArtifacts(request);
            }
        }
    }
    if (assetImportManager_) {
        for (const auto &task : assetImportManager_->history()) {
            if (task->sceneId == entry.id && task->profileId == profileId &&
                !isTerminalAssetImportState(task->state.load())) {
                status.state = ArtifactState::Importing;
                status.reason = std::string("asset import ") +
                                assetImportStateName(task->state.load());
                break;
            }
        }
    }
    artifactStatuses_[statusKey(entry.id, profileId)] = std::move(status);
}

void SceneWorkflowController::refreshAllArtifactStatuses() {
    artifactStatuses_.clear();
    for (int index = 0; index < static_cast<int>(entries_.size()); ++index)
        refreshArtifactStatus(index);
    if (artifactIndex_)
        artifactUsage_ = artifactIndex_->usage();
}

void SceneWorkflowController::refreshValidationStatus(int entryIndex) {
    if (entryIndex < 0 || entryIndex >= static_cast<int>(entries_.size()))
        return;
    const SceneEntry &entry = entries_[entryIndex];
    if (!entry.isModelPreview())
        return;
    AssetValidationQuery query;
    const CatalogModel *model = catalog_.findModel(entry.id);
    if (!model || model->type != "gltf") {
        query.state = AssetValidationState::NotApplicable;
        query.reason = "validation applies only to Catalog glTF models";
    } else {
        query = querySceneValidation(config_.cacheRoot,
                                     projectContext_.projectRoot, entry.id);
    }
    validationStatuses_[entry.id] = std::move(query);
}

void SceneWorkflowController::refreshAllValidationStatuses() {
    validationStatuses_.clear();
    for (int index = 0; index < static_cast<int>(entries_.size()); ++index)
        refreshValidationStatus(index);
}

std::optional<ArtifactStatus>
SceneWorkflowController::artifactStatus(int entryIndex) const {
    if (entryIndex < 0 || entryIndex >= static_cast<int>(entries_.size()))
        return std::nullopt;
    const SceneEntry &entry = entries_[entryIndex];
    const auto found = artifactStatuses_.find(artifactKeyForEntry(entry));
    return found == artifactStatuses_.end()
               ? std::nullopt
               : std::optional<ArtifactStatus>(found->second);
}

std::optional<ArtifactIndexRecord>
SceneWorkflowController::artifactRecord(int entryIndex) const {
    if (!artifactIndex_ || entryIndex < 0 ||
        entryIndex >= static_cast<int>(entries_.size())) {
        return std::nullopt;
    }
    const SceneEntry &entry = entries_[entryIndex];
    const auto found = artifactIndex_->records().find(
        artifactIndexKey(entry.id, profileIdForEntry(entry)));
    return found == artifactIndex_->records().end()
               ? std::nullopt
               : std::optional<ArtifactIndexRecord>(found->second);
}

std::optional<AssetValidationQuery>
SceneWorkflowController::validationStatus(int entryIndex) const {
    if (entryIndex < 0 || entryIndex >= static_cast<int>(entries_.size()))
        return std::nullopt;
    const auto found = validationStatuses_.find(entries_[entryIndex].id);
    return found == validationStatuses_.end()
               ? std::nullopt
               : std::optional<AssetValidationQuery>(found->second);
}

std::filesystem::path SceneWorkflowController::artifactIndexPath() const {
    return artifactIndex_ ? artifactIndex_->path() : std::filesystem::path{};
}

SceneWorkflowSnapshot SceneWorkflowController::snapshot(
    const SceneWorkflowRuntimeState &runtime) const {
    SceneWorkflowSnapshot result;
    result.projectId = catalog_.projectId;
    result.selectedIndex = selectedEntryIndex_;
    result.showImport = config_.importMode != AssetImportMode::CookedOnly;
    result.canImport = config_.importMode == AssetImportMode::OnDemand &&
                       projectContext_.catalogWritable &&
                       assetImportManager_ != nullptr;
    result.busy =
        (modelImport_.validationTask &&
         !isTerminalAssetImportState(
             modelImport_.validationTask->state.load())) ||
        modelImport_.importFuture.valid();
    result.status = modelImport_.status;
    if (!status_.empty()) {
        if (!result.status.empty())
            result.status += " | ";
        result.status += status_;
    }
    result.error = modelImport_.error;
    if (!error_.empty()) {
        if (!result.error.empty())
            result.error += " | ";
        result.error += error_;
    }
    if (modelImport_.worker) {
        const uint64_t total = modelImport_.worker->totalBytes.load();
        const uint64_t completed =
            modelImport_.worker->completedBytes.load();
        result.copyActive = true;
        result.copyProgress = total == 0
                                  ? 0.0f
                                  : static_cast<float>(completed) /
                                        static_cast<float>(total);
        std::lock_guard<std::mutex> lock(modelImport_.worker->mutex);
        result.copyFile = modelImport_.worker->currentFile;
    }
    result.openImportDialog = modelImport_.openDialog;
    result.importPreflight = modelImport_.preflight;
    result.importValidation = modelImport_.validationReport;
    result.importValidationReportPath =
        modelImport_.validationReportPath;
    result.importProfileIds = modelImport_.profileIds;
    result.defaultImportProfileIndex = modelImport_.profileIndex;
    result.defaultReferenceExisting = modelImport_.referenceExisting;
    result.defaultLoadAfterImport = modelImport_.loadAfterActiveImport;

    const bool canInstantiate = runtime.nativeSceneSessionActive &&
                                projectContext_.catalogWritable;
    for (const PrimitiveModelDefinition &primitive :
         primitiveModelDefinitions()) {
        result.enginePrimitives.push_back(
            {std::string(primitive.id), std::string(primitive.displayName),
             canInstantiate});
    }

    result.models.reserve(catalog_.models.size());
    result.nativeScenes.reserve(catalog_.sceneDocuments.size());
    for (int index = 0; index < static_cast<int>(entries_.size()); ++index) {
        const SceneEntry &entry = entries_[index];
        SceneWorkflowItemSnapshot item{index,
                                       entry.id,
                                       entry.name,
                                       entry.sourcePath,
                                       entry.profileId,
                                       entry.available,
                                       entry.unavailableReason};
        item.current = index == runtime.currentSceneIndex;
        if (entry.isNativeScene()) {
            result.nativeScenes.push_back(std::move(item));
            continue;
        }
        item.profileId = profileIdForEntry(entry);
        const auto profile = catalog_.importProfiles.find(item.profileId);
        if (profile != catalog_.importProfiles.end())
            item.encoder = profile->second.textureEncoder;
        const auto artifact = artifactStatuses_.find(
            statusKey(entry.id, item.profileId));
        if (artifact != artifactStatuses_.end()) {
            item.artifactState = artifactStateName(artifact->second.state);
            item.artifactReason = artifact->second.reason;
        }
        const auto validation = validationStatuses_.find(entry.id);
        if (validation != validationStatuses_.end()) {
            item.validationState =
                assetValidationStateName(validation->second.state);
            item.validationReason = validation->second.reason;
            item.validationReportPath = validation->second.reportPath;
        }
        item.canAuthor = config_.importMode == AssetImportMode::OnDemand;
        item.canLoadSource = config_.importMode != AssetImportMode::CookedOnly;
        item.canEditCatalog = projectContext_.catalogWritable &&
                              config_.importMode == AssetImportMode::OnDemand;
        item.canInstantiate = canInstantiate && item.available &&
                              catalog_.findModel(entry.id) != nullptr;
        result.models.push_back(std::move(item));
    }
    return result;
}

AssetWorkflowSnapshot SceneWorkflowController::assetSnapshot() const {
    AssetWorkflowSnapshot result;
    result.projectId = catalog_.projectId;
    result.mode = importModeName(config_.importMode);
    result.catalogPath = projectContext_.catalogPath.string();
    result.cachePath = config_.cacheRoot.string();
    result.authoringCompiled = config_.authoringCompiled;
    if (artifactUsage_) {
        result.hasUsage = true;
        result.indexRecords = artifactUsage_->records;
        result.readyRecords = artifactUsage_->readyRecords;
        result.cacheBlobFiles = artifactUsage_->cacheBlobFiles;
        result.cacheBlobBytes = artifactUsage_->cacheBlobBytes;
        result.unreferencedBlobFiles =
            artifactUsage_->unreferencedBlobFiles;
        result.unreferencedBlobBytes =
            artifactUsage_->unreferencedBlobBytes;
        result.referencedBlobs = artifactUsage_->referencedBlobs;
        result.referencedBlobBytes = artifactUsage_->referencedBlobBytes;
        result.indexSchema = ArtifactIndex::kSchemaVersion;
    }

    if (selectedEntryIndex_ >= 0 &&
        selectedEntryIndex_ < static_cast<int>(entries_.size()) &&
        entries_[selectedEntryIndex_].isModelPreview()) {
        const SceneEntry &entry = entries_[selectedEntryIndex_];
        AssetArtifactSnapshot artifact;
        artifact.modelName = entry.name;
        artifact.profileId = profileIdForEntry(entry);
        const auto profile =
            catalog_.importProfiles.find(artifact.profileId);
        if (profile != catalog_.importProfiles.end())
            artifact.encoder = profile->second.textureEncoder;
        const auto status = artifactStatuses_.find(
            statusKey(entry.id, artifact.profileId));
        if (status != artifactStatuses_.end()) {
            artifact.state = artifactStateName(status->second.state);
            artifact.reason = status->second.reason;
            artifact.payloadKind = status->second.payloadKind;
            artifact.entryCount = status->second.entryCount;
            artifact.blobBytes = status->second.blobBytes;
        } else {
            artifact.state = "Unknown";
        }
        if (const auto record = artifactRecord(selectedEntryIndex_)) {
            artifact.failureCode = record->failureCode;
            artifact.failureMessage = record->failureMessage;
        }
        result.selectedModel = std::move(artifact);
    }

    for (const CatalogEnvironment &environment : catalog_.environments) {
        EnvironmentAssetSnapshot item;
        item.id = environment.id;
        item.displayName = environment.displayName;
        item.source = environment.source.generic_string();
        item.profileId = environment.environmentProfile;
        item.artifactState = "Unknown";
        if (!projectContext_.cookedPackage) {
            try {
                const ArtifactStatus status = inspectEnvironmentArtifacts(
                    {config_.cacheRoot,
                     projectContext_.resolveProjectPath(environment.source),
                     catalog_.projectId, environment.id,
                     environment.environmentProfile});
                item.artifactState = artifactStateName(status.state);
                item.artifactReason = status.reason;
                item.entryCount = status.entryCount;
                item.blobBytes = status.blobBytes;
                item.ready = status.ready();
            } catch (const std::exception &error) {
                item.artifactState = "Invalid";
                item.artifactReason = error.what();
            }
        }
        result.environments.push_back(std::move(item));
    }
    result.canEditEnvironments =
        projectContext_.catalogWritable &&
        config_.importMode == AssetImportMode::OnDemand;
    result.canBuildEnvironments =
        assetImportManager_ && !projectContext_.cookedPackage;
    result.environmentStatus = environmentStatus_;
    result.environmentError = environmentError_;
    result.activeTask = activeAssetTask();
    result.recentTasks = recentAssetTasks(8);
    return result;
}

void SceneWorkflowController::acknowledgeImportDialog() {
    modelImport_.openDialog = false;
}

void SceneWorkflowController::processModelImport() {
#if !VKL_ENABLE_ASSET_AUTHORING
    return;
#else
    if (modelImport_.validationTask &&
        isTerminalAssetImportState(
            modelImport_.validationTask->state.load())) {
        const auto task = modelImport_.validationTask;
        modelImport_.validationTask.reset();
        try {
            std::filesystem::path reportPath;
            std::string taskError;
            {
                std::lock_guard<std::mutex> lock(task->mutex);
                reportPath = std::filesystem::u8path(task->manifestPath);
                taskError = task->error;
            }
            if (reportPath.empty()) {
                throw std::runtime_error(
                    taskError.empty() ? "Validator produced no report"
                                      : taskError);
            }
            AssetValidationReport report;
            std::string reportError;
            if (!loadAssetValidationReport(reportPath, report, reportError)) {
                throw std::runtime_error(
                    "Could not load validation report: " + reportError);
            }
            modelImport_.preflight =
                ModelImportService::preflight(task->sourcePath);
            modelImport_.validationReport = std::move(report);
            modelImport_.validationReportPath = std::move(reportPath);
            modelImport_.profileIds.clear();
            for (const auto &profile : catalog_.importProfiles)
                modelImport_.profileIds.push_back(profile.first);
            std::sort(modelImport_.profileIds.begin(),
                      modelImport_.profileIds.end());
            const auto selected = std::find(
                modelImport_.profileIds.begin(),
                modelImport_.profileIds.end(), catalog_.defaultImportProfile);
            modelImport_.profileIndex =
                selected == modelImport_.profileIds.end()
                    ? 0
                    : static_cast<int>(selected -
                                       modelImport_.profileIds.begin());
            modelImport_.referenceExisting = pathIsWithin(
                projectContext_.projectRoot,
                modelImport_.preflight->sourcePath);
            modelImport_.openDialog = true;
            modelImport_.status.clear();
            modelImport_.error.clear();
        } catch (const std::exception &error) {
            modelImport_.error = error.what();
            modelImport_.status.clear();
        }
    }

    if (modelImport_.importFuture.valid() &&
        modelImport_.importFuture.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
        try {
            const ModelImportResult result = modelImport_.importFuture.get();
            const bool loadAfter = modelImport_.loadAfterActiveImport;
            modelImport_.worker.reset();
            dismissModelImport();
            modelImport_.status = "Imported " + result.model.displayName;
            modelImport_.error.clear();
            refresh(result.model.id);
            const ImportProfile &profile =
                catalog_.profile(result.model.importProfile);
            setTextureLimit(profile.textureLimit);
            if (callbacks_.textureLimitChanged)
                callbacks_.textureLimitChanged(profile.textureLimit);
            const int index = findEntryById(result.model.id);
            if (index >= 0) {
                requestEntry(index, false, loadAfter,
                             SceneWorkflowRequestReason::SceneRegistration,
                             false);
                modelImport_.status =
                    "Registered " + result.model.displayName +
                    "; derived texture import queued";
            }
        } catch (const std::exception &error) {
            modelImport_.worker.reset();
            modelImport_.error = error.what();
            modelImport_.status.clear();
        }
    }
#endif
}

void SceneWorkflowController::processAssetTasks() {
    if (!assetImportManager_)
        return;
    const auto tasks = assetImportManager_->history();
    for (const auto &task : tasks) {
        if (!isTerminalAssetImportState(task->state.load()) ||
            !processedImports_.insert(task->id).second) {
            continue;
        }

        const int entryIndex = findEntryById(task->sceneId);
        const AssetImportState state = task->state.load();
        if (task->kind == AssetImportKind::SceneValidation) {
            if (entryIndex >= 0)
                refreshValidationStatus(entryIndex);
            if (state == AssetImportState::Completed) {
                status_ = "Validation " + task->sceneId + ": " +
                          assetValidationStateName(
                              task->validationState.load());
                error_.clear();
            } else if (state == AssetImportState::Failed) {
                std::lock_guard<std::mutex> lock(task->mutex);
                error_ = task->error.empty() ? "Scene validation failed"
                                             : task->error;
            } else if (state == AssetImportState::Cancelled) {
                status_ = "Scene validation cancelled";
            }
            continue;
        }

        if (task->kind == AssetImportKind::Environment) {
            if (state == AssetImportState::Completed) {
                reloadArtifactIndex();
                environmentStatus_ =
                    "Built environment artifacts for " + task->sceneId;
                environmentError_.clear();
                if (callbacks_.invalidateEnvironment) {
                    callbacks_.invalidateEnvironment(task->sceneId,
                                                     task->profileId);
                }
                if (callbacks_.environmentArtifactsReady)
                    callbacks_.environmentArtifactsReady(task->sceneId);
            } else if (state == AssetImportState::Failed) {
                std::lock_guard<std::mutex> lock(task->mutex);
                environmentError_ = task->error.empty()
                                        ? "Environment bake failed"
                                        : task->error;
            } else if (state == AssetImportState::Cancelled) {
                environmentStatus_ = "Environment bake cancelled";
            }
            continue;
        }

        if (state == AssetImportState::Completed) {
            reloadArtifactIndex();
            if (callbacks_.invalidateModel) {
                callbacks_.invalidateModel(ModelAssetId(task->sceneId),
                                           task->profileId);
            }
            if (artifactIndex_) {
                artifactIndex_->recordImportSuccess(
                    task->sceneId, task->profileId, task->id);
                persistArtifactIndex();
            }
        } else if (state == AssetImportState::Failed && artifactIndex_) {
            std::lock_guard<std::mutex> lock(task->mutex);
            artifactIndex_->recordFailure(
                task->sceneId, task->profileId, "import_failed",
                task->error.empty() ? "Asset import failed" : task->error,
                task->logPath);
            persistArtifactIndex();
        }
        if (entryIndex >= 0)
            refreshArtifactStatus(entryIndex);

        if (state == AssetImportState::Completed && entryIndex >= 0) {
            const auto status = artifactStatuses_.find(
                statusKey(task->sceneId, task->profileId));
            const bool ready = status != artifactStatuses_.end() &&
                               status->second.ready();
            if (!ready) {
                error_ =
                    "Import completed but artifacts failed validation: " +
                    (status == artifactStatuses_.end()
                         ? std::string("status unavailable")
                         : status->second.reason);
            } else if (const auto selected =
                           loadCoordinator_.takeLatestScene(task->id)) {
                try {
                    if (!callbacks_.requestSceneLoad) {
                        throw std::logic_error(
                            "Scene runtime load callback is missing");
                    }
                    const uint64_t loadTask =
                        callbacks_.requestSceneLoad(*selected, false, false);
                    importToLoadTask_[task->id] = loadTask;
                    status_ = "Loading " + entries_[*selected].name;
                } catch (const std::exception &error) {
                    error_ = error.what();
                }
            }
        } else if (state == AssetImportState::Failed) {
            std::lock_guard<std::mutex> lock(task->mutex);
            error_ = task->error.empty() ? "Asset import failed"
                                         : task->error;
        } else if (state == AssetImportState::Cancelled) {
            status_ = "Asset import cancelled";
        }
        loadCoordinator_.discard(task->id);
    }
}

AssetTaskSnapshot SceneWorkflowController::makeAssetTaskSnapshot(
    const std::shared_ptr<AssetImportTask> &task) const {
    AssetTaskSnapshot result;
    if (!task)
        return result;
    const AssetImportState state = task->state.load();
    result.id = task->id;
    result.kind = assetImportKindName(task->kind);
    result.assetId = task->sceneId;
    result.profileId = task->profileId;
    result.sourcePath = task->sourcePath;
    result.state = assetImportStateName(state);
    result.phase = isTerminalAssetImportState(state) ? "complete" : "importing";
    result.completed = task->completedArtifacts.load();
    result.total = task->totalArtifacts.load();
    result.encoded = task->encodedArtifacts.load();
    result.reused = task->reusedArtifacts.load();
    result.failed = task->failedArtifacts.load();
    result.workers = task->workers.load();
    result.activeImage = task->activeImage.load();
    result.estimatedMemoryBytes = task->estimatedMemoryBytes.load();
    result.elapsedSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      task->requestedAt)
            .count();
    result.logPath = task->logPath;
    result.processExitCode = static_cast<int>(task->processExitCode);
    result.terminal = isTerminalAssetImportState(state);
    result.validationState =
        assetValidationStateName(task->validationState.load());
    result.validationErrors = task->validationErrors.load();
    result.validationWarnings = task->validationWarnings.load();
    result.linkedSceneLoadTaskId = linkedSceneLoadTask(task->id);
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        result.error = task->error;
        result.manifestPath = std::filesystem::u8path(task->manifestPath);
        if (task->kind == AssetImportKind::SceneValidation)
            result.reportPath = result.manifestPath;
        result.validationReportKey = task->validationReportKey;
        result.validationInputFingerprint =
            task->validationInputFingerprint;
        result.validationFailureReason = task->validationFailureReason;
    }
    return result;
}

void SceneWorkflowController::validateEntryIndex(int index) const {
    if (index < 0 || index >= static_cast<int>(entries_.size()))
        throw SceneWorkflowError("invalid_scene", "Invalid scene index.");
}

std::string SceneWorkflowController::artifactKeyForEntry(
    const SceneEntry &entry) const {
    return statusKey(entry.id, profileIdForEntry(entry));
}

} // namespace vkr
