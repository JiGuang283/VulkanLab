#pragma once

#include "SceneFactory.h"
#include "SceneLoadTask.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace vkr {

class SceneLoadManager {
  public:
    SceneLoadManager();
    ~SceneLoadManager();

    SceneLoadManager(const SceneLoadManager &) = delete;
    SceneLoadManager &operator=(const SceneLoadManager &) = delete;

    std::shared_ptr<SceneLoadTask> request(
        int sceneIndex, const std::string &sceneName,
        ScenePrepareFactory prepareFactory,
        const SceneLoadContext &context);
    bool cancel(uint64_t taskId);
    void cancelActive();
    std::shared_ptr<SceneLoadTask> task(uint64_t taskId) const;
    std::shared_ptr<SceneLoadTask> latestTask() const;
    std::unique_ptr<PreparedSceneData> takePrepared(uint64_t taskId);
    void shutdown();

  private:
    struct WorkItem {
        std::shared_ptr<SceneLoadTask> task;
        ScenePrepareFactory factory;
        SceneLoadContext context;
    };

    void workerLoop();
    void cancelTask(const std::shared_ptr<SceneLoadTask> &task);
    void pruneHistoryLocked();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    bool stopping_ = false;
    uint64_t nextTaskId_ = 1;
    uint64_t generation_ = 0;
    std::unique_ptr<WorkItem> pending_;
    std::shared_ptr<SceneLoadTask> active_;
    std::shared_ptr<SceneLoadTask> latest_;
    std::deque<uint64_t> historyOrder_;
    std::unordered_map<uint64_t, std::shared_ptr<SceneLoadTask>> tasks_;
};

} // namespace vkr
