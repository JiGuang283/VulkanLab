#pragma once

#include "PreparedEnvironment.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace vkr {

enum class EnvironmentLoadState : uint32_t {
    Queued,
    PreparingCpu,
    ReadyForUpload,
    Uploading,
    WaitingForGpu,
    ReadyToPublish,
    Completed,
    Cancelling,
    Cancelled,
    Failed,
};

const char *environmentLoadStateName(EnvironmentLoadState state);
bool isTerminalEnvironmentLoadState(EnvironmentLoadState state);

struct EnvironmentLoadRequest {
    std::filesystem::path cacheRoot;
    std::filesystem::path sourcePath;
    std::string projectId;
    std::string environmentId;
    std::string displayName;
    std::string profileId;
    bool validateSource = true;
};

struct EnvironmentLoadTask {
    uint64_t id = 0;
    uint64_t generation = 0;
    std::string environmentId;
    std::string displayName;
    std::string profileId;
    std::atomic<EnvironmentLoadState> state{EnvironmentLoadState::Queued};
    std::atomic<uint32_t> uploadedImages{0};
    uint32_t totalImages = 4;
    std::shared_ptr<std::atomic_bool> cancellation =
        std::make_shared<std::atomic_bool>(false);
    mutable std::mutex mutex;
    std::unique_ptr<PreparedEnvironmentData> prepared;
    std::string error;
};

class EnvironmentLoadManager {
  public:
    static constexpr uint64_t kTaskIdMask = (uint64_t{1} << 61);

    EnvironmentLoadManager();
    ~EnvironmentLoadManager();

    EnvironmentLoadManager(const EnvironmentLoadManager &) = delete;
    EnvironmentLoadManager &operator=(const EnvironmentLoadManager &) = delete;

    std::shared_ptr<EnvironmentLoadTask>
    request(EnvironmentLoadRequest request);
    bool cancel(uint64_t taskId);
    std::shared_ptr<EnvironmentLoadTask> task(uint64_t taskId) const;
    std::shared_ptr<EnvironmentLoadTask> latestTask() const;
    std::unique_ptr<PreparedEnvironmentData> takePrepared(uint64_t taskId);
    void shutdown();

  private:
    struct WorkItem {
        std::shared_ptr<EnvironmentLoadTask> task;
        EnvironmentLoadRequest request;
    };

    void workerLoop();
    void cancelTask(const std::shared_ptr<EnvironmentLoadTask> &task);
    void pruneHistoryLocked();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    bool stopping_ = false;
    uint64_t nextTaskId_ = kTaskIdMask;
    uint64_t generation_ = 0;
    std::unique_ptr<WorkItem> pending_;
    std::shared_ptr<EnvironmentLoadTask> active_;
    std::shared_ptr<EnvironmentLoadTask> latest_;
    std::unordered_map<uint64_t, std::shared_ptr<EnvironmentLoadTask>> tasks_;
    std::deque<uint64_t> historyOrder_;
};

} // namespace vkr
