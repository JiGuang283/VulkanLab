#pragma once

#include "ModelAsset.h"
#include "diagnostics/SceneLoadStats.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace vkr {

enum class ModelAssetState : uint32_t {
    Unloaded,
    Queued,
    PreparingCpu,
    ReadyForUpload,
    Uploading,
    WaitingForGpu,
    Ready,
    Failed,
    Cancelled,
    Retiring,
};

const char *modelAssetStateName(ModelAssetState state);

struct ModelAssetKey {
    ModelAssetId modelId;
    std::string profileId;

    friend bool operator==(const ModelAssetKey &left,
                           const ModelAssetKey &right) {
        return left.modelId == right.modelId &&
               left.profileId == right.profileId;
    }
};

struct ModelAssetKeyHash {
    size_t operator()(const ModelAssetKey &key) const noexcept;
};

struct ModelAssetHandleSnapshot {
    ModelAssetKey key;
    uint64_t generation = 0;
    ModelAssetState state = ModelAssetState::Unloaded;
    uint64_t texturesCompleted = 0;
    uint64_t texturesTotal = 0;
    uint64_t meshesCompleted = 0;
    uint64_t meshesTotal = 0;
    uint64_t texturesUploaded = 0;
    uint64_t textureUploadTotal = 0;
    uint64_t meshesUploaded = 0;
    uint64_t meshUploadTotal = 0;
    uint64_t processedBytes = 0;
    std::string error;
    std::optional<SceneLoadStats> terminalStats;
};

namespace detail {
struct ModelAssetLease;
}

class ModelAssetHandle {
  public:
    ModelAssetHandle() = default;
    explicit ModelAssetHandle(std::shared_ptr<detail::ModelAssetLease> lease)
        : lease_(std::move(lease)) {}

    explicit operator bool() const { return lease_ != nullptr; }
    ModelAssetKey key() const;
    uint64_t generation() const;
    ModelAssetState state() const;
    std::shared_ptr<const ModelAsset> asset() const;
    ModelAssetHandleSnapshot snapshot() const;
    void reset() { lease_.reset(); }

  private:
    std::shared_ptr<detail::ModelAssetLease> lease_;
};

} // namespace vkr
