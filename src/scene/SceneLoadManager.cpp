#include "SceneLoadManager.h"

#include "diagnostics/Profiling.h"

#include <stdexcept>
#include <utility>

namespace vkr {

SceneLoadManager::SceneLoadManager()
    : worker_(&SceneLoadManager::workerLoop, this) {}

SceneLoadManager::~SceneLoadManager() { shutdown(); }

std::shared_ptr<SceneLoadTask> SceneLoadManager::request(
    int sceneIndex, const std::string &sceneName, const std::string &modelId,
    const std::string &profileId, uint32_t textureLimit,
    ModelAssetHandle modelAsset, bool repositoryHit,
    bool coalescedRequest) {
    auto task = std::make_shared<SceneLoadTask>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return {};
        if (latest_ && !isTerminalSceneLoadState(latest_->state.load()))
            cancelTask(latest_);

        task->id = nextTaskId_++;
        task->generation = ++generation_;
        task->sceneIndex = sceneIndex;
        task->kind = SceneLoadKind::ModelPreview;
        task->sceneName = sceneName;
        task->modelId = modelId;
        task->profileId = profileId;
        task->textureLimit = textureLimit;
        task->repositoryHit = repositoryHit;
        task->coalescedRequest = coalescedRequest;
        task->modelAsset = std::move(modelAsset);
        task->modelGeneration = task->modelAsset.generation();
        task->stats.taskId = task->id;
        task->stats.generation = task->generation;
        task->stats.sceneName = sceneName;
        task->stats.maxTextureSize = textureLimit;
        task->state = SceneLoadState::Queued;
        task->phase = SceneLoadPhase::Queued;
        latest_ = task;
        tasks_[task->id] = task;
        historyOrder_.push_back(task->id);
        pruneHistoryLocked();
    }
    refresh(task);
    return task;
}

std::shared_ptr<SceneLoadTask> SceneLoadManager::requestNative(
    int sceneIndex, const std::string &sceneName,
    const std::string &sceneDocumentId,
    const std::filesystem::path &documentPath,
    const std::filesystem::path &projectRoot,
    SceneDocumentReferences references) {
    auto task = std::make_shared<SceneLoadTask>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return {};
        if (latest_ && !isTerminalSceneLoadState(latest_->state.load()))
            cancelTask(latest_);
        if (pendingNative_)
            cancelTask(pendingNative_->task);

        task->id = nextTaskId_++;
        task->generation = ++generation_;
        task->sceneIndex = sceneIndex;
        task->kind = SceneLoadKind::NativeScene;
        task->sceneName = sceneName;
        task->modelId.clear();
        task->profileId.clear();
        task->stats.taskId = task->id;
        task->stats.generation = task->generation;
        task->stats.sceneName = sceneName;
        task->state = SceneLoadState::Queued;
        task->phase = SceneLoadPhase::Queued;
        latest_ = task;
        tasks_[task->id] = task;
        historyOrder_.push_back(task->id);
        pruneHistoryLocked();
        pendingNative_ = std::make_unique<NativeWorkItem>(NativeWorkItem{
            task, sceneDocumentId, documentPath, projectRoot,
            std::move(references)});
    }
    condition_.notify_one();
    return task;
}

bool SceneLoadManager::cancel(uint64_t taskId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = tasks_.find(taskId);
    if (found == tasks_.end() ||
        isTerminalSceneLoadState(found->second->state.load())) {
        return false;
    }
    cancelTask(found->second);
    return true;
}

void SceneLoadManager::cancelActive() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (latest_ && !isTerminalSceneLoadState(latest_->state.load()))
        cancelTask(latest_);
}

std::shared_ptr<SceneLoadTask>
SceneLoadManager::task(uint64_t taskId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = tasks_.find(taskId);
    return found == tasks_.end() ? nullptr : found->second;
}

std::shared_ptr<SceneLoadTask> SceneLoadManager::latestTask() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

void SceneLoadManager::refresh(const std::shared_ptr<SceneLoadTask> &task) {
    if (!task || isTerminalSceneLoadState(task->state.load()))
        return;
    if (task->cancellation->load()) {
        task->modelAsset.reset();
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            task->nativeModels.clear();
            task->nativeEnvironments.clear();
        }
        task->state = SceneLoadState::Cancelled;
        task->phase = SceneLoadPhase::Complete;
        return;
    }

    if (task->kind == SceneLoadKind::NativeScene)
        return;

    const ModelAssetHandleSnapshot snapshot = task->modelAsset.snapshot();
    task->modelGeneration = snapshot.generation;
    task->progress.completedTextures = snapshot.texturesCompleted;
    task->progress.totalTextures = snapshot.texturesTotal;
    task->progress.completedMeshes = snapshot.meshesCompleted;
    task->progress.totalMeshes = snapshot.meshesTotal;
    task->progress.uploadedTextures = snapshot.texturesUploaded;
    task->progress.uploadTextureTotal = snapshot.textureUploadTotal;
    task->progress.uploadedMeshes = snapshot.meshesUploaded;
    task->progress.uploadMeshTotal = snapshot.meshUploadTotal;
    task->progress.processedBytes = snapshot.processedBytes;

    switch (snapshot.state) {
    case ModelAssetState::Unloaded:
    case ModelAssetState::Queued:
        task->state = SceneLoadState::Queued;
        task->phase = SceneLoadPhase::Queued;
        break;
    case ModelAssetState::PreparingCpu:
        task->state = SceneLoadState::PreparingCpu;
        task->phase = SceneLoadPhase::PreparingModel;
        break;
    case ModelAssetState::ReadyForUpload:
        task->state = SceneLoadState::ReadyForUpload;
        break;
    case ModelAssetState::Uploading:
        task->state = SceneLoadState::Uploading;
        task->phase = SceneLoadPhase::UploadingModel;
        break;
    case ModelAssetState::WaitingForGpu:
        task->state = SceneLoadState::WaitingForGpu;
        task->phase = SceneLoadPhase::UploadingModel;
        break;
    case ModelAssetState::Ready:
    case ModelAssetState::Retiring:
        task->state = SceneLoadState::ReadyToPublish;
        task->phase = SceneLoadPhase::PublishingWorld;
        break;
    case ModelAssetState::Failed: {
        std::lock_guard<std::mutex> lock(task->mutex);
        task->error = snapshot.error;
        if (snapshot.terminalStats)
            task->stats = *snapshot.terminalStats;
        task->state = SceneLoadState::Failed;
        task->phase = SceneLoadPhase::Complete;
        break;
    }
    case ModelAssetState::Cancelled:
        task->state = SceneLoadState::Cancelled;
        task->phase = SceneLoadPhase::Complete;
        break;
    }
}

void SceneLoadManager::releaseAsset(
    const std::shared_ptr<SceneLoadTask> &task) {
    if (task)
        task->modelAsset.reset();
}

void SceneLoadManager::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return;
        stopping_ = true;
        for (auto &pair : tasks_) {
            if (!isTerminalSceneLoadState(pair.second->state.load()))
                cancelTask(pair.second);
            pair.second->modelAsset.reset();
            std::lock_guard<std::mutex> taskLock(pair.second->mutex);
            pair.second->nativeModels.clear();
            pair.second->nativeEnvironments.clear();
        }
        pendingNative_.reset();
        latest_.reset();
    }
    condition_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void SceneLoadManager::cancelTask(
    const std::shared_ptr<SceneLoadTask> &task) {
    if (!task || isTerminalSceneLoadState(task->state.load()))
        return;
    task->cancellation->store(true);
    task->modelAsset.reset();
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        task->nativeModels.clear();
        task->nativeEnvironments.clear();
    }
    task->state = SceneLoadState::Cancelled;
    task->phase = SceneLoadPhase::Complete;
}

void SceneLoadManager::pruneHistoryLocked() {
    constexpr size_t kMaxHistory = 32;
    while (historyOrder_.size() > kMaxHistory) {
        const uint64_t id = historyOrder_.front();
        const auto found = tasks_.find(id);
        if (found != tasks_.end() &&
            !isTerminalSceneLoadState(found->second->state.load())) {
            break;
        }
        historyOrder_.pop_front();
        tasks_.erase(id);
    }
}

void SceneLoadManager::workerLoop() {
    for (;;) {
        std::unique_ptr<NativeWorkItem> work;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [&] {
                return stopping_ || pendingNative_ != nullptr;
            });
            if (stopping_)
                return;
            work = std::move(pendingNative_);
        }
        const auto &task = work->task;
        if (task->cancellation->load())
            continue;
        task->state = SceneLoadState::PreparingCpu;
        task->phase = SceneLoadPhase::ParsingDocument;
        try {
            VKL_PROFILE_ZONE("SceneDocument Parse");
            LoadedSceneDocument loaded = SceneDocumentService::load(
                work->documentPath, work->projectRoot, &work->references);
            if (loaded.document.id.value() != work->expectedDocumentId) {
                throw std::runtime_error(
                    "Scene document ID does not match Catalog entry: " +
                    work->expectedDocumentId);
            }
            if (task->cancellation->load()) {
                task->state = SceneLoadState::Cancelled;
                task->phase = SceneLoadPhase::Complete;
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(task->mutex);
                task->loadedDocument = std::move(loaded);
            }
            task->phase = SceneLoadPhase::ResolvingModels;
            task->state = SceneLoadState::ReadyForUpload;
        } catch (const std::exception &error) {
            std::lock_guard<std::mutex> lock(task->mutex);
            task->error = error.what();
            task->state = SceneLoadState::Failed;
            task->phase = SceneLoadPhase::Complete;
        }
    }
}

} // namespace vkr
