#pragma once

#include "diagnostics/SceneLoadStats.h"
#include "scene/AssetRepository.h"
#include "scene/EnvironmentAssetRepository.h"
#include "render/IRenderWorld.h"
#include "scene/ModelPrepareFactory.h"
#include "scene/SceneLoadTask.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vkr {

class Camera;
class DescriptorAllocator;
class Device;
class FrameSync;
class MaterialSystem;
class Renderer;
class SceneCatalog;
class SceneLoadManager;
struct CatalogEnvironment;
struct ProjectContext;
struct SceneEntry;

class SceneRuntimeError : public std::runtime_error {
  public:
    SceneRuntimeError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code_(std::move(code)) {}

    const std::string &code() const noexcept { return code_; }

  private:
    std::string code_;
};

struct SceneRuntimePublication {
    SceneLoadKind kind = SceneLoadKind::ModelPreview;
    int sceneIndex = -1;
    std::shared_ptr<IRenderWorld> world;
    std::optional<LoadedSceneDocument> document;
};

struct SceneRuntimeCallbacks {
    std::function<std::optional<std::string>()> publicationBlockReason;
    std::function<void(const SceneRuntimePublication &)> worldPublished;
    std::function<void(const std::shared_ptr<SceneLoadTask> &, bool)>
        loadFinalized;
    std::function<void(const EnvironmentAssetKey &)> environmentPublished;
};

class SceneRuntimeCoordinator {
  public:
    SceneRuntimeCoordinator(
        Device &device, DescriptorAllocator &descriptorAllocator,
        MaterialSystem &materialSystem, Renderer &renderer,
        FrameSync &frameSync, Camera &camera,
        const ProjectContext &projectContext, const SceneCatalog &catalog,
        const std::vector<SceneEntry> &sceneRegistry,
        SceneLoadContext &loadContext, SceneRuntimeCallbacks callbacks);
    ~SceneRuntimeCoordinator();

    SceneRuntimeCoordinator(const SceneRuntimeCoordinator &) = delete;
    SceneRuntimeCoordinator &
    operator=(const SceneRuntimeCoordinator &) = delete;

    uint64_t requestSceneLoad(int index, bool sourceFallback = false,
                              bool reloadAsset = false);
    bool cancelSceneLoad(uint64_t taskId);
    void pump();
    void collectRetired();

    uint64_t queueEnvironment(const CatalogEnvironment &environment,
                              bool reload = false);
    EnvironmentAssetHandle requestEnvironmentAsset(
        const CatalogEnvironment &environment, bool reload = false,
        bool *repositoryHit = nullptr, bool *coalesced = nullptr);
    bool cancelEnvironmentLoad(uint64_t taskId);
    void clearEnvironment();
    uint64_t reloadEnvironment();

    ModelAssetHandle requestModel(const ModelAssetRequest &request,
                                  bool *repositoryHit = nullptr,
                                  bool *coalesced = nullptr);
    void invalidateModel(const ModelAssetId &modelId,
                         const std::string *profileId = nullptr);
    void invalidateEnvironment(const std::string &environmentId,
                               const std::string *profileId = nullptr);

    void closeWorld();
    void remapCurrentSceneIndex();
    void adoptCurrentSceneIndex(int index);
    void shutdown();

    const std::shared_ptr<IRenderWorld> &currentWorld() const {
        return currentWorld_;
    }
    int currentSceneIndex() const { return currentSceneIndex_; }
    uint64_t sceneGeneration() const { return sceneGeneration_; }
    const std::shared_ptr<SceneLoadTask> &latestSceneLoadTask() const {
        return latestSceneLoadTask_;
    }
    const std::shared_ptr<EnvironmentLoadTask> &
    latestEnvironmentLoadTask() const {
        return latestEnvironmentLoadTask_;
    }
    const std::optional<SceneLoadStats> &lastSceneLoadStats() const {
        return lastSceneLoadStats_;
    }
    const std::string &selectedEnvironmentId() const {
        return selectedEnvironmentId_;
    }

    std::shared_ptr<SceneLoadTask> sceneLoadTask(uint64_t taskId) const;
    std::shared_ptr<EnvironmentLoadTask>
    environmentLoadTask(uint64_t taskId) const;
    AssetRepositorySnapshot modelRepositorySnapshot() const;
    EnvironmentAssetRepositorySnapshot
    environmentRepositorySnapshot() const;
    uint64_t pendingUploadCount() const;
    uint64_t pendingTextureCount() const;
    uint64_t pendingMeshCount() const;
    uint32_t inFlightUploadBatches() const;
    uint64_t stagingBytesInUse() const;

  private:
    struct RetiredWorld {
        uint64_t retireAfterSerial = 0;
        std::shared_ptr<IRenderWorld> world;
    };

    void updateSceneLoading();
    void updateNativeSceneLoading(
        const std::shared_ptr<SceneLoadTask> &task);
    void resolveNativeSceneAssets(
        const std::shared_ptr<SceneLoadTask> &task);
    void publishPreview(const std::shared_ptr<SceneLoadTask> &task);
    void publishNativeScene(const std::shared_ptr<SceneLoadTask> &task);
    void updateEnvironmentLoading();
    void retireCurrentWorld();
    void finalizeLoad(const std::shared_ptr<SceneLoadTask> &task,
                      bool success);
    void applyCameraDefaults();
    std::optional<std::string> publicationBlockReason() const;

    Device *device_ = nullptr;
    MaterialSystem *materialSystem_ = nullptr;
    Renderer *renderer_ = nullptr;
    FrameSync *frameSync_ = nullptr;
    Camera *camera_ = nullptr;
    const ProjectContext *projectContext_ = nullptr;
    const SceneCatalog *catalog_ = nullptr;
    const std::vector<SceneEntry> *sceneRegistry_ = nullptr;
    SceneLoadContext *loadContext_ = nullptr;
    SceneRuntimeCallbacks callbacks_;

    std::unique_ptr<AssetRepository> assetRepository_;
    std::unique_ptr<SceneLoadManager> sceneLoadManager_;
    std::unique_ptr<EnvironmentAssetRepository>
        environmentAssetRepository_;
    std::shared_ptr<IRenderWorld> currentWorld_;
    std::deque<RetiredWorld> retiredWorlds_;
    int currentSceneIndex_ = -1;
    std::string currentSceneId_;
    std::optional<SceneLoadStats> lastSceneLoadStats_;
    std::shared_ptr<SceneLoadTask> latestSceneLoadTask_;
    std::shared_ptr<EnvironmentLoadTask> latestEnvironmentLoadTask_;
    std::unique_ptr<EnvironmentAssetHandle> pendingEnvironmentAsset_;
    std::unique_ptr<EnvironmentAssetHandle> activeEnvironmentAsset_;
    std::string selectedEnvironmentId_;
    uint64_t lastFinalizedTaskId_ = 0;
    uint64_t sceneGeneration_ = 0;
    bool shutdown_ = false;
};

} // namespace vkr
