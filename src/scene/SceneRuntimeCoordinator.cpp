#include "SceneRuntimeCoordinator.h"

#include "assets/ArtifactStatus.h"
#include "assets/EnvironmentLoadManager.h"
#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/Log.h"
#include "diagnostics/Profiling.h"
#include "render/material/MaterialSystem.h"
#include "render/Renderer.h"
#include "scene/Camera.h"
#include "scene/ModelSourceResolver.h"
#include "scene/RuntimeWorld.h"
#include "scene/Scene.h"
#include "scene/SceneEntry.h"
#include "scene/SceneLoadManager.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace vkr {
namespace {

double bytesToMiB(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

template <typename Callback>
void invokeCallbackNoexcept(const char *name, Callback &&callback) noexcept {
    try {
        std::forward<Callback>(callback)();
    } catch (const std::exception &error) {
        VKR_LOG_WARN("SceneRuntime", "{} callback failed: {}", name,
                     error.what());
    } catch (...) {
        VKR_LOG_WARN("SceneRuntime", "{} callback failed", name);
    }
}

void populateLightStats(SceneLoadStats &stats, const IRenderWorld &world) {
    stats.lightInstanceCount = world.lights().size();
    stats.directionalLightCount = 0;
    stats.pointLightCount = 0;
    stats.spotLightCount = 0;
    for (const SceneLight &light : world.lights()) {
        switch (light.type) {
        case LightType::Directional:
            ++stats.directionalLightCount;
            break;
        case LightType::Point:
            ++stats.pointLightCount;
            break;
        case LightType::Spot:
            ++stats.spotLightCount;
            break;
        }
    }
}

void validateLoadStats(const SceneLoadStats &stats) {
    const double detailedMax = std::max(
        {stats.deviceIdleMs, stats.teardownMs, stats.sceneFactoryMs,
         stats.gltfParseMs, stats.textureFileReadMs, stats.textureDecodeMs,
         stats.resources.textureResizeMs,
         stats.resources.derivedTextureReadMs,
         stats.resources.derivedTextureTranscodeMs,
         stats.resources.textureUploadMs, stats.materialSetupMs,
         stats.meshCpuMs, stats.resources.meshUploadMs,
         stats.resources.batchSubmitWaitMs, stats.hierarchyMs});
    if (stats.totalMs + 0.01 < detailedMax) {
        VKR_LOG_WARN("LoadStats",
                     "Scene '{}' has inconsistent timing: total={:.2f}ms, "
                     "largest stage={:.2f}ms.",
                     stats.sceneName, stats.totalMs, detailedMax);
    }
    if (stats.resources.gpuTextureCount > 0 &&
        stats.resources.textureUploadBytes == 0) {
        VKR_LOG_WARN("LoadStats",
                     "Scene '{}' created textures but recorded no texture "
                     "upload bytes.",
                     stats.sceneName);
    }
    if (stats.resources.gpuMeshCount > 0 &&
        stats.resources.vertexUploadBytes == 0) {
        VKR_LOG_WARN("LoadStats",
                     "Scene '{}' created meshes but recorded no vertex "
                     "upload bytes.",
                     stats.sceneName);
    }
    if (stats.resources.fenceWaitCalls > stats.resources.batchSubmits ||
        stats.resources.completedBatchSubmits >
            stats.resources.batchSubmits) {
        VKR_LOG_WARN("LoadStats",
                     "Scene '{}' has inconsistent upload synchronization "
                     "statistics.",
                     stats.sceneName);
    }
}

void logLoadStats(const SceneLoadStats &stats) {
    const int64_t allocationDelta =
        memoryDelta(stats.allocatorAfter.allocationBytes,
                    stats.allocatorBefore.allocationBytes);
    const int64_t blockDelta = memoryDelta(stats.allocatorAfter.blockBytes,
                                           stats.allocatorBefore.blockBytes);
    VKR_LOG_INFO(
        "LoadStats",
        "scene='{}' success={} limit={} modelGen={} repoHit={} "
        "coalesced={} total={:.2f}ms textures={} meshes={} materials={} "
        "objects={} lights={} nativeBc7={} transcodes={} upload={:.2f}MiB "
        "batchSubmits={} vmaAllocationDelta={:.2f}MiB "
        "vmaBlockDelta={:.2f}MiB",
        stats.sceneName, stats.success,
        stats.maxTextureSize == 0 ? std::string("Full")
                                  : std::to_string(stats.maxTextureSize),
        stats.modelGeneration, stats.repositoryHit,
        stats.coalescedRequest, stats.totalMs,
        stats.resources.gpuTextureCount, stats.resources.gpuMeshCount,
        stats.materialCount, stats.objectCount, stats.lightInstanceCount,
        stats.resources.nativeBc7CacheHits,
        stats.resources.basisTranscodeCount,
        bytesToMiB(stats.resources.textureUploadBytes +
                   stats.resources.vertexUploadBytes +
                   stats.resources.indexUploadBytes),
        stats.resources.batchSubmits,
        static_cast<double>(allocationDelta) / (1024.0 * 1024.0),
        static_cast<double>(blockDelta) / (1024.0 * 1024.0));
}

} // namespace

SceneRuntimeCoordinator::SceneRuntimeCoordinator(
    Device &device, DescriptorAllocator &descriptorAllocator,
    MaterialSystem &materialSystem, Renderer &renderer,
    FrameSync &frameSync, Camera &camera,
    const ProjectContext &projectContext, const SceneCatalog &catalog,
    const std::vector<SceneEntry> &sceneRegistry, SceneLoadContext &loadContext,
    SceneRuntimeCallbacks callbacks)
    : device_(&device), materialSystem_(&materialSystem),
      renderer_(&renderer), frameSync_(&frameSync), camera_(&camera),
      projectContext_(&projectContext), catalog_(&catalog),
      sceneRegistry_(&sceneRegistry), loadContext_(&loadContext),
      callbacks_(std::move(callbacks)),
      assetRepository_(std::make_unique<AssetRepository>(
          device, descriptorAllocator, materialSystem)),
      sceneLoadManager_(std::make_unique<SceneLoadManager>()),
      environmentAssetRepository_(
          std::make_unique<EnvironmentAssetRepository>(device)) {}

SceneRuntimeCoordinator::~SceneRuntimeCoordinator() { shutdown(); }

uint64_t SceneRuntimeCoordinator::requestSceneLoad(
    int index, bool sourceFallback, bool reloadAsset) {
    if (index < 0 || index >= static_cast<int>(sceneRegistry_->size()))
        throw SceneRuntimeError("invalid_scene", "Invalid scene index.");
    const SceneEntry &entry = sceneRegistry_->at(index);
    if (!entry.available)
        throw SceneRuntimeError("scene_unavailable", entry.unavailableReason);
    if (entry.isNativeScene()) {
        if (sourceFallback)
            throw SceneRuntimeError(
                "source_fallback_not_applicable",
                "Source fallback applies only to model previews.");
        latestSceneLoadTask_ = sceneLoadManager_->requestNative(
            index, entry.name, entry.id,
            std::filesystem::u8path(entry.sourcePath),
            projectContext_->projectRoot, catalog_->documentReferences());
        if (!latestSceneLoadTask_)
            throw std::runtime_error("Scene load manager is shutting down");
        latestSceneLoadTask_->stats.allocatorBefore =
            device_->allocatorMemorySnapshot();
        VKR_LOG_INFO("Scene", "Queued native scene task {} for {}",
                     latestSceneLoadTask_->id, entry.name);
        return latestSceneLoadTask_->id;
    }

    loadContext_->sceneId = entry.id;
    loadContext_->modelId = entry.id;
    if (sourceFallback) {
        loadContext_->profileId = "explicit_source_fallback";
    } else {
        const auto preferred =
            catalog_->importProfiles.find(entry.profileId);
        if (preferred != catalog_->importProfiles.end() &&
            preferred->second.textureLimit ==
                loadContext_->maxTextureSize) {
            loadContext_->profileId = preferred->first;
        } else {
            loadContext_->profileId.clear();
            for (const auto &candidate : catalog_->importProfiles) {
                if (candidate.second.textureLimit ==
                    loadContext_->maxTextureSize) {
                    loadContext_->profileId = candidate.first;
                    break;
                }
            }
            if (loadContext_->profileId.empty()) {
                loadContext_->profileId =
                    loadContext_->maxTextureSize == 0
                        ? "runtime_full"
                        : "runtime_" +
                              std::to_string(loadContext_->maxTextureSize);
            }
        }
    }
    if (!entry.prepareFactory)
        throw SceneRuntimeError(
            "model_prepare_unavailable",
            "Model Preview has no asynchronous prepare factory.");

    SceneLoadContext context = *loadContext_;
    context.loadStats = nullptr;
    ModelAssetRequest request{};
    request.key = {ModelAssetId(entry.id), loadContext_->profileId};
    request.displayName = entry.name;
    request.sourcePath = std::filesystem::u8path(entry.sourcePath);
    request.prepareFactory = entry.prepareFactory;
    request.loadContext = std::move(context);
    request.policy = reloadAsset ? ModelAssetRequestPolicy::Reload
                                 : ModelAssetRequestPolicy::UseCached;
    bool repositoryHit = false;
    bool coalesced = false;
    ModelAssetHandle handle = assetRepository_->requestModel(
        request, &repositoryHit, &coalesced);
    latestSceneLoadTask_ = sceneLoadManager_->request(
        index, entry.name, entry.id, loadContext_->profileId,
        loadContext_->maxTextureSize, std::move(handle), repositoryHit,
        coalesced);
    if (!latestSceneLoadTask_)
        throw std::runtime_error("Scene load manager is shutting down");
    latestSceneLoadTask_->stats.allocatorBefore =
        device_->allocatorMemorySnapshot();
    VKR_LOG_INFO(
        "Scene",
        "Queued preview task {} for {} modelGeneration={} hit={} "
        "coalesced={}",
        latestSceneLoadTask_->id, entry.name,
        latestSceneLoadTask_->modelGeneration, repositoryHit, coalesced);
    return latestSceneLoadTask_->id;
}

bool SceneRuntimeCoordinator::cancelSceneLoad(uint64_t taskId) {
    return sceneLoadManager_->cancel(taskId);
}

void SceneRuntimeCoordinator::pump() {
    VKL_PROFILE_ZONE("Scene Runtime Pump");
    assetRepository_->pump();
    environmentAssetRepository_->pump();
    updateEnvironmentLoading();
    updateSceneLoading();
}

uint64_t SceneRuntimeCoordinator::queueEnvironment(
    const CatalogEnvironment &environment, bool reload) {
    EnvironmentAssetHandle handle =
        requestEnvironmentAsset(environment, reload);
    latestEnvironmentLoadTask_ =
        environmentAssetRepository_->task(handle.taskId());
    if (!latestEnvironmentLoadTask_)
        throw std::logic_error("Environment repository lost its task");
    selectedEnvironmentId_ = environment.id;
    pendingEnvironmentAsset_ =
        std::make_unique<EnvironmentAssetHandle>(std::move(handle));
    VKR_LOG_INFO("Environment", "Queued environment load task {} for {}",
                 latestEnvironmentLoadTask_->id,
                 environment.displayName);
    return latestEnvironmentLoadTask_->id;
}

EnvironmentAssetHandle SceneRuntimeCoordinator::requestEnvironmentAsset(
    const CatalogEnvironment &environment, bool reload,
    bool *repositoryHit, bool *coalesced) {
    (void)catalog_->environmentProfile(environment.environmentProfile);
    const std::filesystem::path source =
        projectContext_->resolveProjectPath(environment.source);
    if (!projectContext_->cookedPackage) {
        const ArtifactStatus status = inspectEnvironmentArtifacts(
            {std::filesystem::u8path(loadContext_->derivedTextureCachePath),
             source, catalog_->projectId, environment.id,
             environment.environmentProfile});
        if (!status.ready()) {
            throw SceneRuntimeError(
                "environment_artifacts_unavailable",
                "Environment artifacts are " +
                std::string(artifactStateName(status.state)) + ": " +
                status.reason +
                ". Build them with VulkanLabAssetTool environment-cache "
                "build.");
        }
    }
    EnvironmentAssetRequest request{};
    request.key = {environment.id, environment.environmentProfile};
    request.displayName = environment.displayName;
    request.cacheRoot =
        std::filesystem::u8path(loadContext_->derivedTextureCachePath);
    request.sourcePath = source;
    request.projectId = catalog_->projectId;
    request.validateSource = !projectContext_->cookedPackage;
    request.policy = reload ? EnvironmentAssetRequestPolicy::Reload
                            : EnvironmentAssetRequestPolicy::UseCached;
    return environmentAssetRepository_->request(request, repositoryHit,
                                                coalesced);
}

bool SceneRuntimeCoordinator::cancelEnvironmentLoad(uint64_t taskId) {
    return environmentAssetRepository_->cancel(taskId);
}

void SceneRuntimeCoordinator::clearEnvironment() {
    if (latestEnvironmentLoadTask_ &&
        !isTerminalEnvironmentLoadState(
            latestEnvironmentLoadTask_->state.load())) {
        cancelEnvironmentLoad(latestEnvironmentLoadTask_->id);
    }
    pendingEnvironmentAsset_.reset();
    activeEnvironmentAsset_.reset();
    selectedEnvironmentId_.clear();
    renderer_->clearEnvironment();
    VKR_LOG_INFO("Environment", "Environment cleared");
}

uint64_t SceneRuntimeCoordinator::reloadEnvironment() {
    if (selectedEnvironmentId_.empty())
        return 0;
    const CatalogEnvironment *environment =
        catalog_->findEnvironment(selectedEnvironmentId_);
    if (!environment)
        throw SceneRuntimeError("unknown_environment",
                                "Current environment is not in Catalog");
    return queueEnvironment(*environment, true);
}

ModelAssetHandle SceneRuntimeCoordinator::requestModel(
    const ModelAssetRequest &request, bool *repositoryHit,
    bool *coalesced) {
    return assetRepository_->requestModel(request, repositoryHit,
                                          coalesced);
}

void SceneRuntimeCoordinator::invalidateModel(
    const ModelAssetId &modelId, const std::string *profileId) {
    assetRepository_->invalidate(
        modelId, profileId ? std::optional<std::string>(*profileId)
                           : std::nullopt);
}

void SceneRuntimeCoordinator::invalidateEnvironment(
    const std::string &environmentId, const std::string *profileId) {
    environmentAssetRepository_->invalidate(environmentId, profileId);
}

void SceneRuntimeCoordinator::updateEnvironmentLoading() {
    if (!pendingEnvironmentAsset_)
        return;
    const EnvironmentAssetHandleSnapshot snapshot =
        pendingEnvironmentAsset_->snapshot();
    if (snapshot.state == EnvironmentAssetState::Ready) {
        std::shared_ptr<EnvironmentGpuResources> resources =
            pendingEnvironmentAsset_->asset();
        if (!resources)
            throw std::logic_error(
                "Ready environment asset has no GPU resources");
        renderer_->publishEnvironment(resources);
        activeEnvironmentAsset_ = std::move(pendingEnvironmentAsset_);
        if (callbacks_.environmentPublished)
            invokeCallbackNoexcept("environmentPublished", [&] {
                callbacks_.environmentPublished(snapshot.key);
            });
        VKR_LOG_INFO("Environment",
                     "Published environment '{}:{}' generation {}",
                     snapshot.key.environmentId, snapshot.key.profileId,
                     snapshot.generation);
    } else if (snapshot.state == EnvironmentAssetState::Failed ||
               snapshot.state == EnvironmentAssetState::Cancelled) {
        pendingEnvironmentAsset_.reset();
    }
}

void SceneRuntimeCoordinator::updateSceneLoading() {
    if (!latestSceneLoadTask_)
        return;
    sceneLoadManager_->refresh(latestSceneLoadTask_);
    if (latestSceneLoadTask_->kind == SceneLoadKind::NativeScene) {
        updateNativeSceneLoading(latestSceneLoadTask_);
        return;
    }
    const SceneLoadState state = latestSceneLoadTask_->state.load();
    if (state == SceneLoadState::ReadyToPublish) {
        if (const auto reason = publicationBlockReason()) {
            {
                std::lock_guard<std::mutex> lock(
                    latestSceneLoadTask_->mutex);
                latestSceneLoadTask_->error = *reason;
            }
            latestSceneLoadTask_->state = SceneLoadState::Failed;
            latestSceneLoadTask_->phase = SceneLoadPhase::Complete;
            sceneLoadManager_->releaseAsset(latestSceneLoadTask_);
            latestSceneLoadTask_->stats.allocatorAfter =
                device_->allocatorMemorySnapshot();
            finalizeLoad(latestSceneLoadTask_, false);
            return;
        }
        publishPreview(latestSceneLoadTask_);
    } else if ((state == SceneLoadState::Failed ||
                state == SceneLoadState::Cancelled) &&
               latestSceneLoadTask_->id != lastFinalizedTaskId_) {
        latestSceneLoadTask_->stats.allocatorAfter =
            device_->allocatorMemorySnapshot();
        sceneLoadManager_->releaseAsset(latestSceneLoadTask_);
        finalizeLoad(latestSceneLoadTask_, false);
    }
}

void SceneRuntimeCoordinator::publishPreview(
    const std::shared_ptr<SceneLoadTask> &task) {
    const ModelAssetHandleSnapshot snapshot = task->modelAsset.snapshot();
    const std::shared_ptr<const ModelAsset> asset = task->modelAsset.asset();
    if (!asset)
        return;
    const AllocatorMemorySnapshot operationStart =
        task->stats.allocatorBefore;
    if (snapshot.terminalStats && !task->repositoryHit &&
        !task->coalescedRequest) {
        task->stats = *snapshot.terminalStats;
    } else {
        task->stats = {};
        task->stats.allocatorBefore = operationStart;
    }
    task->stats.taskId = task->id;
    task->stats.generation = task->generation;
    task->stats.modelGeneration = asset->generation;
    task->stats.sceneName = task->sceneName;
    task->stats.maxTextureSize = task->textureLimit;
    task->stats.repositoryHit = task->repositoryHit;
    task->stats.coalescedRequest = task->coalescedRequest;
    task->stats.materialCount = asset->materials.size();
    task->stats.objectCount = asset->primitives.size();
    task->stats.primitiveCount = asset->primitives.size();

    auto preview = std::make_shared<Scene>();
    preview->initialCamera = asset->previewCamera;
    ModelInstance instance{};
    instance.asset = task->modelAsset;
    preview->addModelInstance(std::move(instance));
    populateLightStats(task->stats, *preview);

    retireCurrentWorld();
    currentWorld_ = preview;
    currentSceneIndex_ = task->sceneIndex;
    currentSceneId_ = sceneRegistry_->at(currentSceneIndex_).id;
    ++sceneGeneration_;
    if (const auto initialCamera = currentWorld_->initialEditorCamera()) {
        camera_->setPosition(initialCamera->position);
        camera_->setYawPitch(initialCamera->yaw, initialCamera->pitch);
    }
    applyCameraDefaults();
    if (callbacks_.worldPublished)
        invokeCallbackNoexcept("worldPublished", [&] {
            callbacks_.worldPublished(
                {SceneLoadKind::ModelPreview, currentSceneIndex_,
                 currentWorld_, std::nullopt});
        });
    task->stats.allocatorAfter = device_->allocatorMemorySnapshot();
    sceneLoadManager_->releaseAsset(task);
    finalizeLoad(task, true);
}

void SceneRuntimeCoordinator::resolveNativeSceneAssets(
    const std::shared_ptr<SceneLoadTask> &task) {
    LoadedSceneDocument loaded;
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        if (!task->loadedDocument)
            return;
        loaded = *task->loadedDocument;
    }

    std::vector<std::string> modelIds;
    std::unordered_set<std::string> seenModels;
    for (const SceneEntityDocument &entity : loaded.document.entities) {
        if (entity.modelInstance &&
            seenModels.insert(entity.modelInstance->model.value()).second) {
            modelIds.push_back(entity.modelInstance->model.value());
        }
    }

    std::vector<NativeSceneModelBinding> modelBindings;
    modelBindings.reserve(modelIds.size());
    bool allHits = !modelIds.empty();
    bool anyCoalesced = false;
    for (const std::string &modelId : modelIds) {
        const auto source = resolveModelSource(
            *catalog_, *projectContext_, ModelAssetId(modelId));
        if (!source)
            throw std::runtime_error("Scene references unknown model: " +
                                     modelId);
        if (!source->instanceable)
            throw std::runtime_error(
                "Builtin model cannot be instanced in a native scene: " +
                modelId);
        if (!source->available || !source->prepareFactory) {
            throw std::runtime_error(
                "Model is unavailable: " + modelId +
                (source->unavailableReason.empty()
                     ? std::string{}
                     : " (" + source->unavailableReason + ")"));
        }
        SceneLoadContext context = *loadContext_;
        context.sceneId = modelId;
        context.modelId = modelId;
        context.profileId = source->profileId;
        context.maxTextureSize = source->textureLimit;
        context.loadStats = nullptr;
        ModelAssetRequest request{};
        request.key = {ModelAssetId(modelId), source->profileId};
        request.displayName = source->displayName;
        request.sourcePath = source->sourcePath;
        request.prepareFactory = source->prepareFactory;
        request.loadContext = std::move(context);
        bool hit = false;
        bool coalesced = false;
        ModelAssetHandle handle = assetRepository_->requestModel(
            request, &hit, &coalesced);
        allHits = allHits && hit;
        anyCoalesced = anyCoalesced || coalesced;
        modelBindings.push_back(
            {ModelAssetId(modelId), source->profileId, std::move(handle)});
    }

    std::vector<std::pair<std::string, bool>> environmentIds;
    std::unordered_set<std::string> seenEnvironments;
    if (loaded.document.environment) {
        const std::string &id =
            loaded.document.environment->environmentId;
        seenEnvironments.insert(id);
        environmentIds.emplace_back(id, true);
    }
    for (const SceneEntityDocument &entity : loaded.document.entities) {
        if (!entity.reflectionProbe ||
            !entity.reflectionProbe->environmentId)
            continue;
        const std::string &id = *entity.reflectionProbe->environmentId;
        if (seenEnvironments.insert(id).second)
            environmentIds.emplace_back(id, false);
    }
    std::vector<NativeSceneEnvironmentBinding> environmentBindings;
    environmentBindings.reserve(environmentIds.size());
    for (const auto &[environmentId, globalEnvironment] : environmentIds) {
        const CatalogEnvironment *environment =
            catalog_->findEnvironment(environmentId);
        if (!environment) {
            throw std::runtime_error(
                "Scene references unknown environment: " + environmentId);
        }
        EnvironmentAssetHandle handle =
            requestEnvironmentAsset(*environment);
        environmentBindings.push_back(
            {environmentId, environment->environmentProfile,
             globalEnvironment, std::move(handle)});
    }

    {
        std::lock_guard<std::mutex> lock(task->mutex);
        task->nativeModels = std::move(modelBindings);
        task->nativeEnvironments = std::move(environmentBindings);
        task->uniqueModelCount = modelIds.size();
        task->readyModelCount = 0;
        task->repositoryHit = allHits;
        task->coalescedRequest = anyCoalesced;
        task->targetEnvironmentId =
            loaded.document.environment
                ? loaded.document.environment->environmentId
                : std::string{};
        task->uniqueEnvironmentCount = environmentIds.size();
        task->readyEnvironmentCount = 0;
        task->failedEnvironmentId.clear();
    }
    task->phase = SceneLoadPhase::LoadingModels;
    task->state = modelIds.empty() ? SceneLoadState::ReadyForUpload
                                   : SceneLoadState::PreparingCpu;
}

void SceneRuntimeCoordinator::updateNativeSceneLoading(
    const std::shared_ptr<SceneLoadTask> &task) {
    const SceneLoadState state = task->state.load();
    if ((state == SceneLoadState::Failed ||
         state == SceneLoadState::Cancelled) &&
        task->id != lastFinalizedTaskId_) {
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            for (const NativeSceneEnvironmentBinding &binding :
                 task->nativeEnvironments) {
                cancelEnvironmentLoad(binding.asset.taskId());
            }
            task->nativeEnvironments.clear();
        }
        task->stats.allocatorAfter = device_->allocatorMemorySnapshot();
        finalizeLoad(task, false);
        return;
    }

    try {
        SceneLoadPhase phase = task->phase.load();
        if (phase == SceneLoadPhase::ResolvingModels) {
            resolveNativeSceneAssets(task);
            phase = task->phase.load();
        }
        if (phase == SceneLoadPhase::LoadingModels) {
            uint64_t ready = 0;
            uint64_t textureDone = 0;
            uint64_t textureTotal = 0;
            uint64_t meshDone = 0;
            uint64_t meshTotal = 0;
            uint64_t processedBytes = 0;
            std::string failedModel;
            std::string failure;
            {
                std::lock_guard<std::mutex> lock(task->mutex);
                for (const NativeSceneModelBinding &binding :
                     task->nativeModels) {
                    const ModelAssetHandleSnapshot snapshot =
                        binding.asset.snapshot();
                    textureDone += snapshot.texturesCompleted;
                    textureTotal += snapshot.texturesTotal;
                    meshDone += snapshot.meshesCompleted;
                    meshTotal += snapshot.meshesTotal;
                    processedBytes += snapshot.processedBytes;
                    if (snapshot.state == ModelAssetState::Ready ||
                        snapshot.state == ModelAssetState::Retiring) {
                        ++ready;
                    } else if (snapshot.state == ModelAssetState::Failed ||
                               snapshot.state ==
                                   ModelAssetState::Cancelled) {
                        failedModel = binding.modelId.value();
                        failure = snapshot.error.empty()
                                      ? "Model load was cancelled"
                                      : snapshot.error;
                        break;
                    }
                }
                task->readyModelCount = ready;
                task->failedModelId = failedModel;
            }
            task->progress.completedTextures = textureDone;
            task->progress.totalTextures = textureTotal;
            task->progress.completedMeshes = meshDone;
            task->progress.totalMeshes = meshTotal;
            task->progress.processedBytes = processedBytes;
            if (!failedModel.empty())
                throw std::runtime_error("Model '" + failedModel +
                                         "' failed: " + failure);
            if (ready < task->uniqueModelCount) {
                task->state = SceneLoadState::Uploading;
                return;
            }
            if (task->uniqueEnvironmentCount == 0) {
                task->environmentReady = true;
                task->phase = SceneLoadPhase::PublishingWorld;
                task->state = SceneLoadState::ReadyToPublish;
            } else {
                task->phase = SceneLoadPhase::LoadingEnvironment;
                task->state = SceneLoadState::WaitingForGpu;
                return;
            }
        }

        if (task->phase.load() == SceneLoadPhase::LoadingEnvironment) {
            uint64_t ready = 0;
            std::string failedEnvironment;
            std::string failure;
            {
                std::lock_guard<std::mutex> lock(task->mutex);
                for (const NativeSceneEnvironmentBinding &binding :
                     task->nativeEnvironments) {
                    const EnvironmentAssetHandleSnapshot snapshot =
                        binding.asset.snapshot();
                    if (snapshot.state == EnvironmentAssetState::Ready ||
                        snapshot.state == EnvironmentAssetState::Retiring) {
                        ++ready;
                    } else if (snapshot.state ==
                                   EnvironmentAssetState::Failed ||
                               snapshot.state ==
                                   EnvironmentAssetState::Cancelled) {
                        failedEnvironment = binding.environmentId;
                        failure = snapshot.error.empty()
                                      ? "Environment load was cancelled"
                                      : snapshot.error;
                        break;
                    }
                }
                task->readyEnvironmentCount = ready;
                task->failedEnvironmentId = failedEnvironment;
            }
            if (!failedEnvironment.empty()) {
                throw std::runtime_error(
                    "Environment '" + failedEnvironment +
                    "' failed: " + failure);
            }
            if (ready < task->uniqueEnvironmentCount) {
                task->state = SceneLoadState::WaitingForGpu;
                return;
            }
            task->environmentReady = true;
            task->phase = SceneLoadPhase::PublishingWorld;
            task->state = SceneLoadState::ReadyToPublish;
        }

        if (task->phase.load() == SceneLoadPhase::PublishingWorld) {
            if (const auto reason = publicationBlockReason())
                throw std::runtime_error(*reason);
            publishNativeScene(task);
        }
    } catch (const std::exception &error) {
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            task->error = error.what();
        }
        task->state = SceneLoadState::Failed;
        task->phase = SceneLoadPhase::Complete;
    }
}

void SceneRuntimeCoordinator::publishNativeScene(
    const std::shared_ptr<SceneLoadTask> &task) {
    LoadedSceneDocument loaded;
    std::vector<ResolvedModelAsset> assets;
    std::vector<ResolvedEnvironmentAsset> environments;
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        if (!task->loadedDocument)
            throw std::runtime_error(
                "Native scene document is unavailable");
        loaded = *task->loadedDocument;
        assets.reserve(task->nativeModels.size());
        for (const NativeSceneModelBinding &binding : task->nativeModels) {
            assets.push_back(
                {binding.modelId, binding.profileId, binding.asset});
        }
        environments.reserve(task->nativeEnvironments.size());
        for (const NativeSceneEnvironmentBinding &binding :
             task->nativeEnvironments) {
            environments.push_back({binding.environmentId,
                                    binding.profileId, binding.asset});
        }
    }
    std::shared_ptr<RuntimeWorld> world(
        RuntimeWorld::fromDocument(loaded.document, assets, environments));
    populateLightStats(task->stats, *world);
    task->stats.materialCount = world->materials().size();
    task->stats.objectCount = world->renderableCount();
    task->stats.primitiveCount = task->stats.objectCount;

    if (loaded.document.environment) {
        const std::string &environmentId =
            loaded.document.environment->environmentId;
        const auto found = std::find_if(
            task->nativeEnvironments.begin(),
            task->nativeEnvironments.end(),
            [&](const NativeSceneEnvironmentBinding &binding) {
                return binding.environmentId == environmentId;
            });
        if (found == task->nativeEnvironments.end() ||
            !found->asset.asset()) {
            throw std::runtime_error(
                "Global environment is not ready for publication");
        }
        renderer_->publishEnvironment(found->asset.asset());
        activeEnvironmentAsset_ =
            std::make_unique<EnvironmentAssetHandle>(found->asset);
        selectedEnvironmentId_ = environmentId;
        if (callbacks_.environmentPublished)
            invokeCallbackNoexcept("environmentPublished", [&] {
                callbacks_.environmentPublished(found->asset.key());
            });
    } else {
        renderer_->clearEnvironment();
        activeEnvironmentAsset_.reset();
        selectedEnvironmentId_.clear();
    }

    retireCurrentWorld();
    currentWorld_ = world;
    currentSceneIndex_ = task->sceneIndex;
    currentSceneId_ = sceneRegistry_->at(currentSceneIndex_).id;
    ++sceneGeneration_;
    applyCameraDefaults();
    if (callbacks_.worldPublished)
        invokeCallbackNoexcept("worldPublished", [&] {
            callbacks_.worldPublished({SceneLoadKind::NativeScene,
                                       currentSceneIndex_, currentWorld_,
                                       loaded});
        });
    task->stats.allocatorAfter = device_->allocatorMemorySnapshot();
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        task->nativeModels.clear();
        task->nativeEnvironments.clear();
    }
    finalizeLoad(task, true);
}

void SceneRuntimeCoordinator::finalizeLoad(
    const std::shared_ptr<SceneLoadTask> &task, bool success) {
    if (!task || task->id == lastFinalizedTaskId_)
        return;
    task->stats.taskId = task->id;
    task->stats.generation = task->generation;
    task->stats.modelGeneration = task->modelGeneration;
    task->stats.sceneName = task->sceneName;
    task->stats.maxTextureSize = task->textureLimit;
    task->stats.repositoryHit = task->repositoryHit;
    task->stats.coalescedRequest = task->coalescedRequest;
    task->stats.success = success;
    task->stats.totalMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - task->requestedAt)
            .count();
    if (success)
        task->state = SceneLoadState::Completed;
    else if (task->cancellation->load())
        task->state = SceneLoadState::Cancelled;
    else
        task->state = SceneLoadState::Failed;
    task->stats.finalState = sceneLoadStateName(task->state.load());
    task->phase = SceneLoadPhase::Complete;
    lastSceneLoadStats_ = task->stats;
    lastFinalizedTaskId_ = task->id;
    validateLoadStats(task->stats);
    logLoadStats(task->stats);
    task->finalized = true;
    if (callbacks_.loadFinalized)
        invokeCallbackNoexcept("loadFinalized", [&] {
            callbacks_.loadFinalized(task, success);
        });
}

void SceneRuntimeCoordinator::retireCurrentWorld() {
    if (!currentWorld_)
        return;
    retiredWorlds_.push_back(
        {frameSync_->lastSubmittedSerial(), std::move(currentWorld_)});
}

void SceneRuntimeCoordinator::collectRetired() {
    const uint64_t completed = frameSync_->completedSubmissionSerial();
    while (!retiredWorlds_.empty() &&
           retiredWorlds_.front().retireAfterSerial <= completed) {
        retiredWorlds_.pop_front();
    }
    assetRepository_->releaseUnused(frameSync_->lastSubmittedSerial(),
                                    completed);
    materialSystem_->updateSubmissionSerials(
        frameSync_->lastSubmittedSerial(), completed);
    environmentAssetRepository_->releaseUnused(
        frameSync_->lastSubmittedSerial(), completed);
}

void SceneRuntimeCoordinator::closeWorld() {
    if (latestSceneLoadTask_ &&
        !isTerminalSceneLoadState(latestSceneLoadTask_->state.load())) {
        cancelSceneLoad(latestSceneLoadTask_->id);
    }
    if (latestEnvironmentLoadTask_ &&
        !isTerminalEnvironmentLoadState(
            latestEnvironmentLoadTask_->state.load())) {
        cancelEnvironmentLoad(latestEnvironmentLoadTask_->id);
    }
    retireCurrentWorld();
    renderer_->clearEnvironment();
    activeEnvironmentAsset_.reset();
    pendingEnvironmentAsset_.reset();
    selectedEnvironmentId_.clear();
    currentSceneIndex_ = -1;
    currentSceneId_.clear();
    ++sceneGeneration_;
}

void SceneRuntimeCoordinator::remapCurrentSceneIndex() {
    currentSceneIndex_ = -1;
    if (currentSceneId_.empty())
        return;
    for (int index = 0;
         index < static_cast<int>(sceneRegistry_->size()); ++index) {
        if (sceneRegistry_->at(index).id == currentSceneId_) {
            currentSceneIndex_ = index;
            return;
        }
    }
}

void SceneRuntimeCoordinator::adoptCurrentSceneIndex(int index) {
    currentSceneIndex_ = index;
    currentSceneId_ =
        index >= 0 && index < static_cast<int>(sceneRegistry_->size())
            ? sceneRegistry_->at(index).id
            : std::string{};
}

void SceneRuntimeCoordinator::applyCameraDefaults() {
    constexpr float kFallbackNear = 0.05f;
    constexpr float kFallbackFar = 200.0f;
    constexpr float kMinSceneFar = 50.0f;
    constexpr float kRadiusFarScale = 4.0f;
    constexpr float kMinAutoNear = 0.01f;
    constexpr float kMaxAutoNear = 0.05f;
    if (!currentWorld_ || !currentWorld_->bounds().valid) {
        camera_->setClipPlanes(kFallbackNear, kFallbackFar);
        return;
    }
    const Bounds &bounds = currentWorld_->bounds();
    if (!currentWorld_->initialEditorCamera()) {
        const float distance = std::max(bounds.radius * 2.2f, 1.0f);
        const glm::vec3 direction =
            glm::normalize(glm::vec3(1.0f, 1.0f, 0.65f));
        camera_->setPosition(bounds.center + direction * distance);
        camera_->lookAt(bounds.center);
    }
    const float distanceToCenter =
        glm::length(camera_->position() - bounds.center);
    const float farPlane =
        std::max(kMinSceneFar,
                 distanceToCenter + bounds.radius * kRadiusFarScale);
    const float nearPlane = std::max(
        kMinAutoNear, std::min(kMaxAutoNear, farPlane / 10000.0f));
    camera_->setClipPlanes(nearPlane, farPlane);
}

std::optional<std::string>
SceneRuntimeCoordinator::publicationBlockReason() const {
    return callbacks_.publicationBlockReason
               ? callbacks_.publicationBlockReason()
               : std::nullopt;
}

std::shared_ptr<SceneLoadTask>
SceneRuntimeCoordinator::sceneLoadTask(uint64_t taskId) const {
    return sceneLoadManager_->task(taskId);
}

std::shared_ptr<EnvironmentLoadTask>
SceneRuntimeCoordinator::environmentLoadTask(uint64_t taskId) const {
    return environmentAssetRepository_->task(taskId);
}

AssetRepositorySnapshot
SceneRuntimeCoordinator::modelRepositorySnapshot() const {
    return assetRepository_->snapshot();
}

EnvironmentAssetRepositorySnapshot
SceneRuntimeCoordinator::environmentRepositorySnapshot() const {
    return environmentAssetRepository_->snapshot();
}

uint64_t SceneRuntimeCoordinator::pendingUploadCount() const {
    return assetRepository_->pendingUploadCount();
}

uint64_t SceneRuntimeCoordinator::pendingTextureCount() const {
    return assetRepository_->pendingTextureCount();
}

uint64_t SceneRuntimeCoordinator::pendingMeshCount() const {
    return assetRepository_->pendingMeshCount();
}

uint32_t SceneRuntimeCoordinator::inFlightUploadBatches() const {
    return assetRepository_->inFlightUploadBatches();
}

uint64_t SceneRuntimeCoordinator::stagingBytesInUse() const {
    return assetRepository_->stagingBytesInUse();
}

void SceneRuntimeCoordinator::shutdown() {
    if (shutdown_)
        return;
    shutdown_ = true;
    pendingEnvironmentAsset_.reset();
    activeEnvironmentAsset_.reset();
    environmentAssetRepository_->shutdown();
    sceneLoadManager_->shutdown();
    assetRepository_->shutdown();
}

} // namespace vkr
