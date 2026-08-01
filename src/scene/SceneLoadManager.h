#pragma once

#include "SceneLoadTask.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace vkr {

class SceneLoadManager {
  public:
    SceneLoadManager();
    ~SceneLoadManager();

    SceneLoadManager(const SceneLoadManager &) = delete;
    SceneLoadManager &operator=(const SceneLoadManager &) = delete;

    std::shared_ptr<SceneLoadTask>
    request(int sceneIndex, const std::string &sceneName,
            const std::string &modelId, const std::string &profileId,
            uint32_t textureLimit, ModelAssetHandle modelAsset,
            bool repositoryHit, bool coalescedRequest);
    bool cancel(uint64_t taskId);
    void cancelActive();
    std::shared_ptr<SceneLoadTask> task(uint64_t taskId) const;
    std::shared_ptr<SceneLoadTask> latestTask() const;
    void refresh(const std::shared_ptr<SceneLoadTask> &task);
    void releaseAsset(const std::shared_ptr<SceneLoadTask> &task);
    void shutdown();

  private:
    void cancelTask(const std::shared_ptr<SceneLoadTask> &task);
    void pruneHistoryLocked();

    mutable std::mutex mutex_;
    bool stopping_ = false;
    uint64_t nextTaskId_ = 1;
    uint64_t generation_ = 0;
    std::shared_ptr<SceneLoadTask> latest_;
    std::deque<uint64_t> historyOrder_;
    std::unordered_map<uint64_t, std::shared_ptr<SceneLoadTask>> tasks_;
};

} // namespace vkr
