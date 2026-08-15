#pragma once

#include "ModelAsset.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace vkr {

class DescriptorAllocator;
class Device;
class MaterialSystem;
class IncrementalUploadQueue;
class MaterialInstance;
class MaterialTemplate;
class Mesh;
class Texture;
struct PreparedModelData;
struct SceneLoadProgress;
struct SceneLoadStats;

class ModelGpuBuilder {
  public:
    struct Budget {
        uint64_t maxUploadBytes = 32ull * 1024ull * 1024ull;
        double maxRecordMs = 2.0;
    };

    struct Context {
        std::string modelId;
        std::string profileId;
        std::string displayName;
        uint64_t taskId = 0;
        uint64_t generation = 0;
        std::chrono::steady_clock::time_point requestedAt =
            std::chrono::steady_clock::now();
        SceneLoadProgress *progress = nullptr;
        SceneLoadStats *stats = nullptr;
        std::shared_ptr<std::atomic_bool> cancellation;
        std::shared_ptr<MaterialTemplate> materialTemplate;
    };

    ModelGpuBuilder(Device &device, MaterialSystem &materialSystem,
                    Context context,
                    std::unique_ptr<PreparedModelData> prepared);
    ~ModelGpuBuilder();

    ModelGpuBuilder(const ModelGpuBuilder &) = delete;
    ModelGpuBuilder &operator=(const ModelGpuBuilder &) = delete;

    void pump(const Budget &budget = {});
    void cancel();
    bool ready() const;
    bool finished() const;
    bool cancelled() const;
    std::shared_ptr<const ModelAsset> takeAsset();
    const std::string &error() const { return error_; }
    uint64_t pendingUploadCount() const;
    uint64_t pendingTextureCount() const;
    uint64_t pendingMeshCount() const;
    uint32_t inFlightUploadBatches() const;
    uint64_t stagingBytesInUse() const;

  private:
    enum class Phase {
        Textures,
        Meshes,
        WaitingForGpu,
        Materials,
        Finalize,
        Ready,
        Cancelling,
        Cancelled,
        Failed,
    };

    bool budgetExpired(const std::chrono::steady_clock::time_point &start,
                       uint64_t bytes, const Budget &budget) const;
    void submitRecorded();
    void fail(const std::exception &error);
    void finalizeAsset();

    Device *device_ = nullptr;
    MaterialSystem *materialSystem_ = nullptr;
    Context context_;
    std::unique_ptr<PreparedModelData> prepared_;
    std::unique_ptr<IncrementalUploadQueue> uploadQueue_;
    std::shared_ptr<ModelAsset> asset_;
    std::vector<std::shared_ptr<Texture>> textures_;
    std::vector<std::shared_ptr<Mesh>> meshes_;
    std::vector<std::shared_ptr<MaterialInstance>> materials_;
    std::shared_ptr<MaterialInstance> fallbackMaterial_;
    size_t textureIndex_ = 0;
    size_t meshIndex_ = 0;
    size_t materialIndex_ = 0;
    Phase phase_ = Phase::Textures;
    bool failurePending_ = false;
    std::string error_;
    std::chrono::steady_clock::time_point buildStart_ =
        std::chrono::steady_clock::now();
};

} // namespace vkr
