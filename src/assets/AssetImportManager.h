#pragma once

#include <json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vkr {

enum class ImportReason { SceneLoad, SceneRegistration, ManualReimport };
enum class AssetImportState {
    Queued,
    Scanning,
    Importing,
    Publishing,
    Completed,
    Failed,
    Cancelling,
    Cancelled
};

const char *assetImportStateName(AssetImportState state);
bool isTerminalAssetImportState(AssetImportState state);

struct AssetImportRequest {
    std::string sceneId;
    std::string profileId;
    ImportReason reason = ImportReason::SceneLoad;
    bool force = false;
};

struct AssetImportTask {
    uint64_t id = 0;
    std::string sceneId;
    std::string profileId;
    ImportReason reason = ImportReason::SceneLoad;
    bool force = false;
    std::atomic<AssetImportState> state{AssetImportState::Queued};
    std::shared_ptr<std::atomic_bool> cancellation =
        std::make_shared<std::atomic_bool>(false);
    std::atomic<uint64_t> totalArtifacts{0};
    std::atomic<uint64_t> completedArtifacts{0};
    std::atomic<uint64_t> encodedArtifacts{0};
    std::atomic<uint64_t> reusedArtifacts{0};
    std::atomic<uint64_t> failedArtifacts{0};
    std::atomic<uint32_t> workers{0};
    std::atomic<uint64_t> estimatedMemoryBytes{0};
    std::atomic<uint64_t> activeImage{UINT64_MAX};
    std::chrono::steady_clock::time_point requestedAt =
        std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point completedAt{};
    std::filesystem::path logPath;
    mutable std::mutex mutex;
    std::string error;
    std::string manifestPath;
    uint32_t processExitCode = 0;
    bool completedEventReceived = false;
    bool protocolValidated = false;
};

struct AssetImportManagerOptions {
    std::filesystem::path projectRoot;
    std::filesystem::path cacheRoot;
    std::filesystem::path assetToolPath;
    uint32_t workers = 0;
    uint64_t memoryBudgetMiB = 2048;
};

struct AssetImportExecutionResult {
    uint32_t exitCode = 0;
    bool cancelled = false;
    std::string error;
};

using AssetImportEventCallback =
    std::function<void(const nlohmann::json &)>;
using AssetImportLogCallback = std::function<void(const std::string &)>;
using AssetImportExecutor = std::function<AssetImportExecutionResult(
    const AssetImportManagerOptions &, const AssetImportRequest &,
    const std::atomic_bool &, const AssetImportEventCallback &,
    const AssetImportLogCallback &)>;

class AssetImportManager {
  public:
    static constexpr uint64_t kTaskIdMask = uint64_t{1} << 63;

    explicit AssetImportManager(AssetImportManagerOptions options,
                                AssetImportExecutor executor = {});
    ~AssetImportManager();

    AssetImportManager(const AssetImportManager &) = delete;
    AssetImportManager &operator=(const AssetImportManager &) = delete;

    std::shared_ptr<AssetImportTask>
    request(const AssetImportRequest &request);
    bool cancel(uint64_t taskId);
    std::shared_ptr<AssetImportTask> task(uint64_t taskId) const;
    std::shared_ptr<AssetImportTask> activeTask() const;
    std::vector<std::shared_ptr<AssetImportTask>> history() const;
    void shutdown();

  private:
    void workerLoop();
    void applyEvent(const std::shared_ptr<AssetImportTask> &task,
                    const nlohmann::json &event);
    void pruneHistoryLocked();

    AssetImportManagerOptions options_;
    AssetImportExecutor executor_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::shared_ptr<AssetImportTask>> pending_;
    std::unordered_map<uint64_t, std::shared_ptr<AssetImportTask>> tasks_;
    std::vector<uint64_t> historyIds_;
    std::shared_ptr<AssetImportTask> active_;
    uint64_t nextTaskId_ = kTaskIdMask;
    bool stopping_ = false;
    std::thread worker_;
};

AssetImportExecutionResult runAssetImportProcess(
    const AssetImportManagerOptions &options,
    const AssetImportRequest &request,
    const std::atomic_bool &cancelRequested,
    const AssetImportEventCallback &eventCallback,
    const AssetImportLogCallback &logCallback);

} // namespace vkr
