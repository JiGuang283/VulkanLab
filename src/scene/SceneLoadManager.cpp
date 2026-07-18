#include "SceneLoadManager.h"

#include "PreparedSceneData.h"
#include "core/Log.h"

#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

namespace vkr {

SceneLoadManager::SceneLoadManager()
    : worker_([this] { workerLoop(); }) {}

SceneLoadManager::~SceneLoadManager() { shutdown(); }

std::shared_ptr<SceneLoadTask> SceneLoadManager::request(
    int sceneIndex, const std::string &sceneName,
    ScenePrepareFactory prepareFactory, const SceneLoadContext &context) {
    if (!prepareFactory)
        throw std::invalid_argument("Scene load request has no prepare factory");

    auto task = std::make_shared<SceneLoadTask>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            throw std::runtime_error("SceneLoadManager is shutting down");

        if (latest_)
            cancelTask(latest_);
        if (pending_ && pending_->task != latest_)
            cancelTask(pending_->task);
        if (active_ && active_ != latest_)
            cancelTask(active_);

        task->id = nextTaskId_++;
        task->generation = ++generation_;
        task->sceneIndex = sceneIndex;
        task->sceneName = sceneName;
        task->textureLimit = context.maxTextureSize;
        task->stats.sceneName = sceneName;
        task->stats.maxTextureSize = context.maxTextureSize;
        task->stats.taskId = task->id;
        task->stats.generation = task->generation;

        SceneLoadContext taskContext = context;
        taskContext.loadStats = nullptr;
        pending_ = std::make_unique<WorkItem>(
            WorkItem{task, std::move(prepareFactory), taskContext});
        latest_ = task;
        tasks_[task->id] = task;
        historyOrder_.push_back(task->id);
        pruneHistoryLocked();
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
    if (pending_)
        cancelTask(pending_->task);
    if (active_)
        cancelTask(active_);
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

std::unique_ptr<PreparedSceneData>
SceneLoadManager::takePrepared(uint64_t taskId) {
    const auto found = task(taskId);
    if (!found || found->state.load() != SceneLoadState::ReadyForUpload)
        return {};
    std::lock_guard<std::mutex> lock(found->mutex);
    return std::move(found->prepared);
}

void SceneLoadManager::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return;
        stopping_ = true;
        if (pending_)
            cancelTask(pending_->task);
        if (active_)
            cancelTask(active_);
    }
    condition_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void SceneLoadManager::workerLoop() {
    for (;;) {
        std::unique_ptr<WorkItem> work;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock,
                            [this] { return stopping_ || pending_ != nullptr; });
            if (stopping_)
                return;
            work = std::move(pending_);
            active_ = work->task;
        }

        const auto start = std::chrono::steady_clock::now();
        auto &task = *work->task;
        task.stats.workerQueueWaitMs =
            std::chrono::duration<double, std::milli>(start - task.requestedAt)
                .count();
        task.state = SceneLoadState::PreparingCpu;
        try {
            SceneLoadContext context = work->context;
            context.loadStats = &task.stats;
            auto prepared = std::make_unique<PreparedSceneData>(
                work->factory(context, CancellationToken(task.cancellation),
                              task.progress));
            task.stats.cpuPrepareMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start)
                    .count();
            task.stats.sceneFactoryMs += task.stats.cpuPrepareMs;
            if (task.cancellation->load()) {
                task.state = SceneLoadState::Cancelled;
            } else {
                {
                    std::lock_guard<std::mutex> lock(task.mutex);
                    task.prepared = std::move(prepared);
                }
                task.state = SceneLoadState::ReadyForUpload;
            }
        } catch (const std::exception &error) {
            std::lock_guard<std::mutex> lock(task.mutex);
            if (task.cancellation->load()) {
                task.state = SceneLoadState::Cancelled;
            } else {
                task.error = error.what();
                task.state = SceneLoadState::Failed;
                VKR_LOG_ERROR("Scene", "CPU prepare for '{}' failed: {}",
                              task.sceneName, task.error);
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(task.mutex);
            task.error = "Unknown CPU preparation error";
            task.state = task.cancellation->load()
                             ? SceneLoadState::Cancelled
                             : SceneLoadState::Failed;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_ && active_->id == task.id)
                active_.reset();
        }
    }
}

void SceneLoadManager::cancelTask(
    const std::shared_ptr<SceneLoadTask> &task) {
    if (!task || isTerminalSceneLoadState(task->state.load()))
        return;
    task->cancellation->store(true);
    const SceneLoadState state = task->state.load();
    if (state == SceneLoadState::Queued) {
        task->state = SceneLoadState::Cancelled;
    } else if (state == SceneLoadState::ReadyForUpload) {
        std::lock_guard<std::mutex> lock(task->mutex);
        task->prepared.reset();
        task->state = SceneLoadState::Cancelled;
    } else if (state != SceneLoadState::ReadyForUpload) {
        task->state = SceneLoadState::Cancelling;
    }
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

} // namespace vkr
