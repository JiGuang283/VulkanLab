#pragma once

#include "render/EnvironmentGpuResources.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace vkr {

enum class EnvironmentAssetState : uint32_t {
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

const char *environmentAssetStateName(EnvironmentAssetState state);

struct EnvironmentAssetKey {
    std::string environmentId;
    std::string profileId;

    friend bool operator==(const EnvironmentAssetKey &left,
                           const EnvironmentAssetKey &right) {
        return left.environmentId == right.environmentId &&
               left.profileId == right.profileId;
    }
};

struct EnvironmentAssetKeyHash {
    size_t operator()(const EnvironmentAssetKey &key) const noexcept;
};

struct EnvironmentAssetHandleSnapshot {
    EnvironmentAssetKey key;
    uint64_t generation = 0;
    EnvironmentAssetState state = EnvironmentAssetState::Unloaded;
    uint32_t uploadedImages = 0;
    uint32_t totalImages = 4;
    std::string error;
};

namespace detail {
struct EnvironmentAssetLease;
}

class EnvironmentAssetHandle {
  public:
    EnvironmentAssetHandle() = default;
    explicit EnvironmentAssetHandle(
        std::shared_ptr<detail::EnvironmentAssetLease> lease)
        : lease_(std::move(lease)) {}

    explicit operator bool() const { return lease_ != nullptr; }
    EnvironmentAssetKey key() const;
    uint64_t taskId() const;
    uint64_t generation() const;
    EnvironmentAssetState state() const;
    std::shared_ptr<EnvironmentGpuResources> asset() const;
    EnvironmentAssetHandleSnapshot snapshot() const;
    void reset() { lease_.reset(); }

  private:
    std::shared_ptr<detail::EnvironmentAssetLease> lease_;
};

} // namespace vkr
