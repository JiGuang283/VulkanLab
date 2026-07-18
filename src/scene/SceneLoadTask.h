#pragma once

#include "diagnostics/SceneLoadStats.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace vkr {

struct PreparedSceneData;

enum class SceneLoadState : uint32_t {
    Queued,
    PreparingCpu,
    ReadyForUpload,
    ReleasingPreviousScene,
    Uploading,
    WaitingForGpu,
    ReadyToPublish,
    Completed,
    Cancelling,
    Cancelled,
    Failed,
};

const char *sceneLoadStateName(SceneLoadState state);
bool isTerminalSceneLoadState(SceneLoadState state);

class CancellationToken {
  public:
    CancellationToken() = default;
    explicit CancellationToken(std::shared_ptr<std::atomic_bool> cancelled)
        : cancelled_(std::move(cancelled)) {}

    bool cancelled() const {
        return cancelled_ && cancelled_->load(std::memory_order_relaxed);
    }

  private:
    std::shared_ptr<std::atomic_bool> cancelled_;
};

struct SceneLoadProgress {
    std::atomic<uint64_t> completedTextures{0};
    std::atomic<uint64_t> totalTextures{0};
    std::atomic<uint64_t> completedMeshes{0};
    std::atomic<uint64_t> totalMeshes{0};
    std::atomic<uint64_t> uploadedTextures{0};
    std::atomic<uint64_t> uploadTextureTotal{0};
    std::atomic<uint64_t> uploadedMeshes{0};
    std::atomic<uint64_t> uploadMeshTotal{0};
    std::atomic<uint64_t> processedBytes{0};
};

struct SceneLoadTask {
    uint64_t id = 0;
    uint64_t generation = 0;
    int      sceneIndex = -1;
    std::string sceneName;
    uint32_t textureLimit = 0;
    std::atomic<SceneLoadState> state{SceneLoadState::Queued};
    std::atomic_bool finalized{false};
    SceneLoadProgress progress;
    SceneLoadStats stats;
    std::chrono::steady_clock::time_point requestedAt =
        std::chrono::steady_clock::now();
    std::shared_ptr<std::atomic_bool> cancellation =
        std::make_shared<std::atomic_bool>(false);

    mutable std::mutex mutex;
    std::unique_ptr<PreparedSceneData> prepared;
    std::string error;
};

} // namespace vkr
