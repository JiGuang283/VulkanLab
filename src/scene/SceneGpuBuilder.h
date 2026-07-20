#pragma once

#include "SceneLoadTask.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace vkr {

class DescriptorAllocator;
class Device;
class FallbackTextures;
class IncrementalUploadQueue;
class MaterialInstance;
class MaterialTemplate;
class Mesh;
class Scene;
class Texture;
struct PreparedSceneData;

class SceneGpuBuilder {
  public:
    struct Budget {
        uint64_t maxUploadBytes = 32ull * 1024ull * 1024ull;
        double maxRecordMs = 2.0;
    };

    SceneGpuBuilder(Device &device,
                    DescriptorAllocator &descriptorAllocator,
                    std::shared_ptr<SceneLoadTask> task,
                    std::unique_ptr<PreparedSceneData> prepared);
    ~SceneGpuBuilder();

    SceneGpuBuilder(const SceneGpuBuilder &) = delete;
    SceneGpuBuilder &operator=(const SceneGpuBuilder &) = delete;

    void pump(const Budget &budget = {});
    void cancel();
    bool ready() const;
    bool finished() const;
    bool cancelled() const;
    std::unique_ptr<Scene> takeScene();
    std::shared_ptr<SceneLoadTask> task() const { return task_; }
    uint64_t pendingUploadCount() const;
    uint64_t pendingTextureCount() const;
    uint64_t pendingMeshCount() const;
    uint32_t inFlightUploadBatches() const;

  private:
    enum class Phase {
        Fallbacks,
        Textures,
        Meshes,
        WaitingForGpu,
        Materials,
        Objects,
        Ready,
        Cancelling,
        Cancelled,
        Failed,
    };

    bool budgetExpired(const std::chrono::steady_clock::time_point &start,
                       uint64_t bytes, const Budget &budget) const;
    void submitRecorded();
    void fail(const std::exception &error);

    Device *device_ = nullptr;
    DescriptorAllocator *descriptorAllocator_ = nullptr;
    std::shared_ptr<SceneLoadTask> task_;
    std::unique_ptr<PreparedSceneData> prepared_;
    std::unique_ptr<IncrementalUploadQueue> uploadQueue_;
    std::unique_ptr<Scene> scene_;
    std::shared_ptr<MaterialTemplate> materialTemplate_;
    std::shared_ptr<FallbackTextures> fallbackTextures_;
    std::vector<std::shared_ptr<Texture>> fallbackBuildTextures_;
    std::vector<std::shared_ptr<Texture>> textures_;
    std::vector<std::shared_ptr<Mesh>> meshes_;
    std::vector<std::shared_ptr<MaterialInstance>> materials_;
    std::shared_ptr<MaterialInstance> fallbackMaterial_;
    size_t textureIndex_ = 0;
    size_t meshIndex_ = 0;
    size_t materialIndex_ = 0;
    size_t objectIndex_ = 0;
    Phase phase_ = Phase::Fallbacks;
    bool failurePending_ = false;
    std::chrono::steady_clock::time_point buildStart_ =
        std::chrono::steady_clock::now();
};

} // namespace vkr
