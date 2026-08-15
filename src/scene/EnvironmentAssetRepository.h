#pragma once

#include "EnvironmentAssetHandle.h"
#include "scene/EnvironmentGpuBuilder.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace vkr {

class Device;
struct EnvironmentLoadTask;

enum class EnvironmentAssetRequestPolicy { UseCached, Reload };

struct EnvironmentAssetRequest {
    EnvironmentAssetKey key;
    std::string displayName;
    std::filesystem::path cacheRoot;
    std::filesystem::path sourcePath;
    std::string projectId;
    bool validateSource = true;
    EnvironmentAssetRequestPolicy policy =
        EnvironmentAssetRequestPolicy::UseCached;
};

struct EnvironmentAssetRecordSnapshot {
    EnvironmentAssetKey key;
    uint64_t generation = 0;
    EnvironmentAssetState state = EnvironmentAssetState::Unloaded;
    uint64_t consumerCount = 0;
    uint32_t uploadedImages = 0;
    std::string error;
};

struct EnvironmentAssetRepositorySnapshot {
    uint64_t recordCount = 0;
    uint64_t readyCount = 0;
    uint64_t loadingCount = 0;
    uint64_t failedCount = 0;
    uint64_t retiringCount = 0;
    uint64_t cpuPrepareStarts = 0;
    uint64_t gpuBuildStarts = 0;
    uint64_t readyHits = 0;
    uint64_t coalescedRequests = 0;
    std::vector<EnvironmentAssetRecordSnapshot> records;
};

class EnvironmentAssetRepository {
  public:
    explicit EnvironmentAssetRepository(Device &device);
    ~EnvironmentAssetRepository();

    EnvironmentAssetRepository(const EnvironmentAssetRepository &) = delete;
    EnvironmentAssetRepository &
    operator=(const EnvironmentAssetRepository &) = delete;

    EnvironmentAssetHandle request(
        const EnvironmentAssetRequest &request,
        bool *repositoryHit = nullptr, bool *coalesced = nullptr);
    void pump(const EnvironmentGpuBuilder::Budget &budget = {});
    bool cancel(uint64_t taskId);
    std::shared_ptr<EnvironmentLoadTask> task(uint64_t taskId) const;
    void invalidate(const std::string &environmentId,
                    const std::string *profileId = nullptr);
    void releaseUnused(uint64_t lastSubmittedSerial,
                       uint64_t completedSerial);
    EnvironmentAssetRepositorySnapshot snapshot() const;
    void shutdown();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vkr
