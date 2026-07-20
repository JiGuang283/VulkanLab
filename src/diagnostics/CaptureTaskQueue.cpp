#include "CaptureTaskQueue.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace vkr {

CaptureTaskQueue::CaptureTaskQueue(size_t maximumActiveTasks,
                                   size_t terminalHistoryLimit)
    : maximumActiveTasks_(maximumActiveTasks),
      terminalHistoryLimit_(terminalHistoryLimit) {
    if (maximumActiveTasks_ == 0 || terminalHistoryLimit_ == 0)
        throw std::invalid_argument(
            "capture task bounds must both be non-zero");
}

uint64_t CaptureTaskQueue::enqueue(std::filesystem::path relativeOutputPath,
                                   bool includeGui) {
    if (activeCount() >= maximumActiveTasks_)
        throw std::length_error("capture task queue is full");
    if (nextTaskId_ == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("capture task ID space is exhausted");

    const uint64_t taskId = nextTaskId_++;
    CaptureTaskSnapshot task;
    task.request.taskId = taskId;
    task.request.relativeOutputPath = std::move(relativeOutputPath);
    task.request.includeGui = includeGui;
    tasks_.emplace(taskId, std::move(task));
    queuedTaskIds_.push_back(taskId);
    taskOrder_.push_back(taskId);
    trimTerminalHistory();
    return taskId;
}

std::optional<uint64_t> CaptureTaskQueue::beginNext() {
    while (!queuedTaskIds_.empty()) {
        const uint64_t taskId = queuedTaskIds_.front();
        queuedTaskIds_.pop_front();
        CaptureTaskSnapshot *task = find(taskId);
        if (!task || task->state != CaptureTaskState::Queued)
            continue;
        transition(taskId, CaptureTaskState::Recording);
        return taskId;
    }
    return std::nullopt;
}

bool CaptureTaskQueue::cancel(uint64_t taskId) {
    CaptureTaskSnapshot *task = find(taskId);
    if (!task || isTerminalCaptureTaskState(task->state))
        return false;
    if (task->state == CaptureTaskState::Cancelling)
        return true;

    if (task->state == CaptureTaskState::Queued) {
        queuedTaskIds_.erase(
            std::remove(queuedTaskIds_.begin(), queuedTaskIds_.end(), taskId),
            queuedTaskIds_.end());
        transition(taskId, CaptureTaskState::Cancelled);
    } else {
        transition(taskId, CaptureTaskState::Cancelling);
    }
    return true;
}

void CaptureTaskQueue::transition(uint64_t taskId, CaptureTaskState state) {
    CaptureTaskSnapshot *task = find(taskId);
    if (!task)
        throw std::out_of_range("capture task does not exist");
    if (!isValidCaptureTaskTransition(task->state, state))
        throw std::logic_error(std::string("invalid capture transition ") +
                               captureTaskStateName(task->state) + " -> " +
                               captureTaskStateName(state));
    task->state = state;
    if (isTerminalCaptureTaskState(state))
        trimTerminalHistory();
}

CaptureTaskSnapshot *CaptureTaskQueue::find(uint64_t taskId) {
    const auto found = tasks_.find(taskId);
    return found == tasks_.end() ? nullptr : &found->second;
}

const CaptureTaskSnapshot *CaptureTaskQueue::find(uint64_t taskId) const {
    const auto found = tasks_.find(taskId);
    return found == tasks_.end() ? nullptr : &found->second;
}

std::optional<CaptureTaskSnapshot>
CaptureTaskQueue::snapshot(uint64_t taskId) const {
    const CaptureTaskSnapshot *task = find(taskId);
    return task ? std::optional<CaptureTaskSnapshot>(*task) : std::nullopt;
}

std::vector<CaptureTaskSnapshot> CaptureTaskQueue::snapshots() const {
    std::vector<CaptureTaskSnapshot> result;
    result.reserve(taskOrder_.size());
    for (uint64_t taskId : taskOrder_) {
        if (const CaptureTaskSnapshot *task = find(taskId))
            result.push_back(*task);
    }
    return result;
}

size_t CaptureTaskQueue::activeCount() const {
    return static_cast<size_t>(std::count_if(
        tasks_.begin(), tasks_.end(), [](const auto &entry) {
            return !isTerminalCaptureTaskState(entry.second.state);
        }));
}

size_t CaptureTaskQueue::terminalCount() const {
    return static_cast<size_t>(std::count_if(
        tasks_.begin(), tasks_.end(), [](const auto &entry) {
            return isTerminalCaptureTaskState(entry.second.state);
        }));
}

void CaptureTaskQueue::trimTerminalHistory() {
    while (terminalCount() > terminalHistoryLimit_) {
        const auto removable = std::find_if(
            taskOrder_.begin(), taskOrder_.end(), [this](uint64_t taskId) {
                const CaptureTaskSnapshot *task = find(taskId);
                return task && isTerminalCaptureTaskState(task->state);
            });
        if (removable == taskOrder_.end())
            break;
        tasks_.erase(*removable);
        taskOrder_.erase(removable);
    }
}

} // namespace vkr
