#pragma once

#include "ModelAssetHandle.h"
#include "ModelGpuBuilder.h"
#include "ModelPrepareFactory.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

class DescriptorAllocator;
class Device;
class MaterialSystem;

enum class ModelAssetRequestPolicy { UseCached, Reload };

struct ModelAssetRequest {
    ModelAssetKey key;
    std::string displayName;
    std::filesystem::path sourcePath;
    ModelPrepareFactory prepareFactory;
    SceneLoadContext loadContext;
    ModelAssetRequestPolicy policy = ModelAssetRequestPolicy::UseCached;
};

struct ModelAssetRecordSnapshot {
    ModelAssetKey key;
    uint64_t generation = 0;
    ModelAssetState state = ModelAssetState::Unloaded;
    uint64_t consumerCount = 0;
    uint64_t textureCount = 0;
    uint64_t meshCount = 0;
    uint64_t materialCount = 0;
    uint64_t primitiveCount = 0;
    std::string error;
};

struct AssetRepositorySnapshot {
    uint64_t recordCount = 0;
    uint64_t readyCount = 0;
    uint64_t loadingCount = 0;
    uint64_t failedCount = 0;
    uint64_t retiringCount = 0;
    uint64_t cpuPrepareStarts = 0;
    uint64_t gpuBuildStarts = 0;
    uint64_t readyHits = 0;
    uint64_t coalescedRequests = 0;
    std::vector<ModelAssetRecordSnapshot> records;
};

class AssetRepository {
  public:
    AssetRepository(Device &device, DescriptorAllocator &descriptorAllocator,
                    MaterialSystem &materialSystem);
    ~AssetRepository();

    AssetRepository(const AssetRepository &) = delete;
    AssetRepository &operator=(const AssetRepository &) = delete;

    ModelAssetHandle requestModel(const ModelAssetRequest &request,
                                  bool *repositoryHit = nullptr,
                                  bool *coalesced = nullptr);
    void pump(const ModelGpuBuilder::Budget &budget = {});
    void invalidate(const ModelAssetId &modelId,
                    std::optional<std::string> profileId = std::nullopt);
    void releaseUnused(uint64_t lastSubmittedSerial,
                       uint64_t completedSerial);
    AssetRepositorySnapshot snapshot() const;
    uint64_t pendingUploadCount() const;
    uint64_t pendingTextureCount() const;
    uint64_t pendingMeshCount() const;
    uint32_t inFlightUploadBatches() const;
    uint64_t stagingBytesInUse() const;
    void shutdown();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vkr
