#pragma once

#include "CaptureTypes.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

namespace vkr {

class CaptureTaskQueue {
  public:
    explicit CaptureTaskQueue(size_t maximumActiveTasks = 8,
                              size_t terminalHistoryLimit = 32);

    uint64_t nextTaskId() const { return nextTaskId_; }
    uint64_t enqueue(std::filesystem::path relativeOutputPath,
                     bool includeGui,
                     CaptureSourceKind source = CaptureSourceKind::Viewport);
    std::optional<uint64_t> beginNext();
    bool cancel(uint64_t taskId);
    void transition(uint64_t taskId, CaptureTaskState state);

    CaptureTaskSnapshot *find(uint64_t taskId);
    const CaptureTaskSnapshot *find(uint64_t taskId) const;
    std::optional<CaptureTaskSnapshot> snapshot(uint64_t taskId) const;
    std::vector<CaptureTaskSnapshot> snapshots() const;

    size_t queuedCount() const { return queuedTaskIds_.size(); }
    size_t activeCount() const;
    size_t terminalCount() const;

  private:
    void trimTerminalHistory();

    size_t maximumActiveTasks_ = 0;
    size_t terminalHistoryLimit_ = 0;
    uint64_t nextTaskId_ = kCaptureTaskIdBase;
    std::deque<uint64_t> queuedTaskIds_;
    std::deque<uint64_t> taskOrder_;
    std::unordered_map<uint64_t, CaptureTaskSnapshot> tasks_;
};

} // namespace vkr
