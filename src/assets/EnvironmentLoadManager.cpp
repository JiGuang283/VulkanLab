#include "EnvironmentLoadManager.h"

#include "DerivedEnvironmentCache.h"
#include "core/Log.h"
#include "diagnostics/Profiling.h"

#include <stdexcept>
#include <utility>

namespace vkr {

const char *environmentLoadStateName(EnvironmentLoadState state) {
    switch (state) {
    case EnvironmentLoadState::Queued:
        return "Queued";
    case EnvironmentLoadState::PreparingCpu:
        return "PreparingCpu";
    case EnvironmentLoadState::ReadyForUpload:
        return "ReadyForUpload";
    case EnvironmentLoadState::Uploading:
        return "Uploading";
    case EnvironmentLoadState::WaitingForGpu:
        return "WaitingForGpu";
    case EnvironmentLoadState::ReadyToPublish:
        return "ReadyToPublish";
    case EnvironmentLoadState::Completed:
        return "Completed";
    case EnvironmentLoadState::Cancelling:
        return "Cancelling";
    case EnvironmentLoadState::Cancelled:
        return "Cancelled";
    case EnvironmentLoadState::Failed:
        return "Failed";
    }
    return "Unknown";
}

bool isTerminalEnvironmentLoadState(EnvironmentLoadState state) {
    return state == EnvironmentLoadState::Completed ||
           state == EnvironmentLoadState::Cancelled ||
           state == EnvironmentLoadState::Failed;
}

EnvironmentLoadManager::EnvironmentLoadManager()
    : worker_([this] { workerLoop(); }) {}

EnvironmentLoadManager::~EnvironmentLoadManager() { shutdown(); }

std::shared_ptr<EnvironmentLoadTask>
EnvironmentLoadManager::request(EnvironmentLoadRequest request) {
    if (request.environmentId.empty() || request.profileId.empty())
        throw std::invalid_argument(
            "Environment load request identity is incomplete");
    auto task = std::make_shared<EnvironmentLoadTask>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            throw std::runtime_error(
                "EnvironmentLoadManager is shutting down");
        if (pending_)
            cancelTask(pending_->task);
        if (active_)
            cancelTask(active_);
        task->id = nextTaskId_++;
        task->generation = ++generation_;
        task->environmentId = request.environmentId;
        task->displayName = request.displayName;
        task->profileId = request.profileId;
        pending_ =
            std::make_unique<WorkItem>(WorkItem{task, std::move(request)});
        latest_ = task;
        tasks_[task->id] = task;
        historyOrder_.push_back(task->id);
        pruneHistoryLocked();
    }
    condition_.notify_one();
    return task;
}

bool EnvironmentLoadManager::cancel(uint64_t taskId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = tasks_.find(taskId);
    if (found == tasks_.end() ||
        isTerminalEnvironmentLoadState(found->second->state.load())) {
        return false;
    }
    cancelTask(found->second);
    return true;
}

std::shared_ptr<EnvironmentLoadTask>
EnvironmentLoadManager::task(uint64_t taskId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = tasks_.find(taskId);
    return found == tasks_.end() ? nullptr : found->second;
}

std::shared_ptr<EnvironmentLoadTask>
EnvironmentLoadManager::latestTask() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

std::unique_ptr<PreparedEnvironmentData>
EnvironmentLoadManager::takePrepared(uint64_t taskId) {
    const auto found = task(taskId);
    if (!found ||
        found->state.load() != EnvironmentLoadState::ReadyForUpload) {
        return {};
    }
    std::lock_guard<std::mutex> lock(found->mutex);
    return std::move(found->prepared);
}

void EnvironmentLoadManager::shutdown() {
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

void EnvironmentLoadManager::workerLoop() {
    profileSetThreadName("EnvironmentPrepare");
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
        auto &task = *work->task;
        VKL_PROFILE_ZONE("Environment Prepare Task");
        VKL_PROFILE_TEXT(task.environmentId);
        task.state = EnvironmentLoadState::PreparingCpu;
        try {
            DerivedEnvironmentCache cache(
                work->request.cacheRoot, work->request.sourcePath,
                work->request.projectId, work->request.environmentId,
                work->request.displayName, work->request.profileId,
                work->request.validateSource);
            auto prepared = std::make_unique<PreparedEnvironmentData>(
                cache.load());
            if (task.cancellation->load()) {
                task.state = EnvironmentLoadState::Cancelled;
            } else {
                {
                    std::lock_guard<std::mutex> lock(task.mutex);
                    task.prepared = std::move(prepared);
                }
                task.state = EnvironmentLoadState::ReadyForUpload;
            }
        } catch (const std::exception &error) {
            std::lock_guard<std::mutex> lock(task.mutex);
            if (task.cancellation->load()) {
                task.state = EnvironmentLoadState::Cancelled;
            } else {
                task.error = error.what();
                task.state = EnvironmentLoadState::Failed;
                VKR_LOG_ERROR("Environment", "Load '{}' failed: {}",
                              task.environmentId, task.error);
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_ && active_->id == task.id)
                active_.reset();
        }
    }
}

void EnvironmentLoadManager::cancelTask(
    const std::shared_ptr<EnvironmentLoadTask> &task) {
    if (!task ||
        isTerminalEnvironmentLoadState(task->state.load()))
        return;
    task->cancellation->store(true);
    const EnvironmentLoadState state = task->state.load();
    if (state == EnvironmentLoadState::Queued ||
        state == EnvironmentLoadState::ReadyForUpload) {
        if (state == EnvironmentLoadState::ReadyForUpload) {
            std::lock_guard<std::mutex> taskLock(task->mutex);
            task->prepared.reset();
        }
        task->state = EnvironmentLoadState::Cancelled;
    } else {
        task->state = EnvironmentLoadState::Cancelling;
    }
}

void EnvironmentLoadManager::pruneHistoryLocked() {
    constexpr size_t kMaxHistory = 32;
    while (historyOrder_.size() > kMaxHistory) {
        const uint64_t id = historyOrder_.front();
        const auto found = tasks_.find(id);
        if (found != tasks_.end() &&
            !isTerminalEnvironmentLoadState(found->second->state.load())) {
            break;
        }
        historyOrder_.pop_front();
        tasks_.erase(id);
    }
}

} // namespace vkr
