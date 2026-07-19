#include "Application.h"
#include "UniformData.h"

#include "assets/DerivedAssetPaths.h"
#include "assets/ArtifactIndex.h"
#include "assets/ArtifactStatus.h"
#include "assets/AssetLoadCoordinator.h"
#include "assets/AssetImportManager.h"
#include "assets/SceneImportService.h"
#include "assets/SceneCatalogEditor.h"
#include "control/NamedPipeServerWin32.h"
#include "control/RuntimeCommand.h"
#include "control/RuntimeControlProtocol.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/Log.h"
#include "core/ResourcePoolSelfTest.h"
#include "core/SwapChain.h"
#include "core/UploadContext.h"
#include "core/VulkanContext.h"
#include "diagnostics/BuildInfo.h"
#include "render/GuiSystem.h"
#include "render/MaterialInstance.h"
#include "render/MaterialTextureSlot.h"
#include "render/PipelineCache.h"
#include "render/Renderer.h"
#include "scene/PreparedSceneData.h"
#include "scene/SceneFactory.h"
#include "scene/SceneGpuBuilder.h"
#include "scene/SceneLight.h"
#include "scene/SceneLoadManager.h"
#include "scene/SceneLoadTask.h"
#include "scene/SceneRegistryBuilder.h"
#include "window/InputManager.h"
#include "window/Window.h"
#include "platform/FileDialogWin32.h"

#include <imgui.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace vkr {

namespace {

glm::vec3 normalizeOrFallback(const glm::vec3 &v,
                              const glm::vec3 &fallback) {
    const float len2 = glm::dot(v, v);
    if (len2 <= 1.0e-6f)
        return glm::normalize(fallback);
    return glm::normalize(v);
}

SceneLight makeDefaultSun(const glm::vec3 &direction, const glm::vec3 &color,
                          float intensity) {
    SceneLight light{};
    light.type = LightType::Directional;
    light.directionWS =
        normalizeOrFallback(direction, glm::vec3(0.3f, 0.8f, 0.5f));
    light.color = color;
    light.intensity = std::max(intensity, 0.0f);
    return light;
}

GpuLight makeGpuLight(const SceneLight &light) {
    GpuLight gpu{};
    const glm::vec3 direction = normalizeOrFallback(
        light.directionWS,
        light.type == LightType::Directional ? glm::vec3(0.3f, 0.8f, 0.5f)
                                             : glm::vec3(0.0f, -1.0f, 0.0f));

    float innerConeCos = glm::clamp(light.innerConeCos, -1.0f, 1.0f);
    float outerConeCos = glm::clamp(light.outerConeCos, -1.0f, 1.0f);
    if (light.type == LightType::Spot && innerConeCos < outerConeCos)
        std::swap(innerConeCos, outerConeCos);

    gpu.positionRange =
        glm::vec4(light.positionWS, std::max(light.range, 0.0f));
    gpu.directionInnerCos = glm::vec4(direction, innerConeCos);
    gpu.colorIntensity =
        glm::vec4(glm::max(light.color, glm::vec3(0.0f)),
                  std::max(light.intensity, 0.0f));
    gpu.params =
        glm::vec4(static_cast<float>(static_cast<uint32_t>(light.type)),
                  outerConeCos, 0.0f, 0.0f);
    return gpu;
}

const char *alphaModeName(AlphaMode mode) {
    switch (mode) {
    case AlphaMode::Opaque:
        return "Opaque";
    case AlphaMode::Mask:
        return "Mask";
    case AlphaMode::Blend:
        return "Blend";
    }
    return "Unknown";
}

const char *slotName(MaterialTextureSlot slot) {
    switch (slot) {
    case MaterialTextureSlot::BaseColor:
        return "BaseColor";
    case MaterialTextureSlot::Normal:
        return "Normal";
    case MaterialTextureSlot::MetallicRoughness:
        return "MetallicRoughness";
    case MaterialTextureSlot::Occlusion:
        return "Occlusion";
    case MaterialTextureSlot::Emissive:
        return "Emissive";
    case MaterialTextureSlot::Count:
        break;
    }
    return "Unknown";
}

bool isTransparentMaterial(const MaterialParams &params) {
    return params.alphaMode == AlphaMode::Blend ||
           params.transmissionFactor > 0.0f;
}

const char *textureLimitLabel(uint32_t limit) {
    switch (limit) {
    case 0:
        return "Full";
    case 512:
        return "512";
    case 1024:
        return "1024";
    case 2048:
        return "2048";
    default:
        return "Custom";
    }
}

double bytesToMiB(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

double signedBytesToMiB(int64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

bool asciiEqualsIgnoreCase(const std::string &a, const std::string &b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto left = static_cast<unsigned char>(a[i]);
        const auto right = static_cast<unsigned char>(b[i]);
        if (std::tolower(left) != std::tolower(right))
            return false;
    }
    return true;
}

const char *assetImportModeName(AssetImportMode mode) {
    switch (mode) {
    case AssetImportMode::OnDemand:
        return "OnDemand";
    case AssetImportMode::ReadOnly:
        return "ReadOnly";
    case AssetImportMode::CookedOnly:
        return "CookedOnly";
    }
    return "Unknown";
}

std::string artifactStatusKey(const std::string &sceneId,
                              const std::string &profileId) {
    return sceneId + '\n' + profileId;
}

ControlJson assetImportTaskToJson(
    const std::shared_ptr<AssetImportTask> &task) {
    if (!task)
        return nullptr;
    const AssetImportState state = task->state.load();
    ControlJson result = {
        {"taskId", task->id},
        {"sceneId", task->sceneId},
        {"profileId", task->profileId},
        {"state", assetImportStateName(state)},
        {"phase", isTerminalAssetImportState(state) ? "complete" : "importing"},
        {"terminal", isTerminalAssetImportState(state)},
        {"progress",
         {{"completed", task->completedArtifacts.load()},
          {"total", task->totalArtifacts.load()},
          {"encoded", task->encodedArtifacts.load()},
          {"reused", task->reusedArtifacts.load()},
          {"failed", task->failedArtifacts.load()},
          {"workers", task->workers.load()},
          {"activeImage", task->activeImage.load()},
          {"estimatedMemoryBytes", task->estimatedMemoryBytes.load()}}},
        {"logPath", task->logPath.string()},
        {"exitCode", task->processExitCode}};
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        if (!task->error.empty())
            result["error"] = task->error;
        if (!task->manifestPath.empty())
            result["manifest"] = task->manifestPath;
    }
    return result;
}

ControlJson allocatorSnapshotToJson(const AllocatorMemorySnapshot &snapshot) {
    return {{"allocationCount", snapshot.allocationCount},
            {"allocationBytes", snapshot.allocationBytes},
            {"blockBytes", snapshot.blockBytes}};
}

ControlJson sceneLoadStatsToJson(const SceneLoadStats &stats) {
    const ResourceLoadStats &r = stats.resources;
    return {
        {"scene", stats.sceneName},
        {"taskId", stats.taskId},
        {"generation", stats.generation},
        {"finalState", stats.finalState},
        {"textureLimit", stats.maxTextureSize},
        {"success", stats.success},
        {"timingsMs",
         {{"total", stats.totalMs},
          {"deviceIdle", stats.deviceIdleMs},
          {"teardown", stats.teardownMs},
          {"sceneFactory", stats.sceneFactoryMs},
          {"gltfParse", stats.gltfParseMs},
          {"textureFileRead", stats.textureFileReadMs},
          {"textureDecode", stats.textureDecodeMs},
          {"textureResize", r.textureResizeMs},
          {"derivedTextureRead", r.derivedTextureReadMs},
          {"derivedTextureTranscode", r.derivedTextureTranscodeMs},
          {"textureUpload", r.textureUploadMs},
          {"materialSetup", stats.materialSetupMs},
          {"meshCpu", stats.meshCpuMs},
          {"meshUpload", r.meshUploadMs},
          {"batchSubmitWait", r.batchSubmitWaitMs},
          {"hierarchy", stats.hierarchyMs},
          {"workerQueueWait", stats.workerQueueWaitMs},
          {"cpuPrepare", stats.cpuPrepareMs},
          {"gpuBuild", stats.gpuBuildMs},
          {"timeToFirstUpload", stats.timeToFirstUploadMs},
          {"maxUploadPump", r.maxUploadPumpMs}}},
        {"counts",
         {{"deviceWaitIdleCalls", stats.deviceWaitIdleCalls},
          {"materials", stats.materialCount},
          {"objects", stats.objectCount},
          {"textureDecodes", r.textureDecodeCount},
          {"gpuTextures", r.gpuTextureCount},
          {"resizedTextures", r.resizedTextureCount},
          {"derivedTextureLookups", r.derivedTextureLookups},
          {"derivedTextureHits", r.derivedTextureHits},
          {"derivedTextureMisses", r.derivedTextureMisses},
          {"derivedTextureInvalid", r.derivedTextureInvalid},
          {"bc7Textures", r.bc7TextureCount},
          {"rgbaTranscodeFallbacks", r.rgbaTranscodeFallbackCount},
          {"prebuiltMipTextures", r.prebuiltMipTextureCount},
          {"gpuMeshes", r.gpuMeshCount},
          {"vertices", r.vertexCount},
          {"indices", r.indexCount}}},
        {"bytes",
         {{"encodedSources", r.encodedSourceBytes},
          {"decodedRgba", r.decodedRgbaBytes},
          {"derivedTextureRead", r.derivedTextureReadBytes},
          {"textureUpload", r.textureUploadBytes},
          {"textureGpuEstimated", r.textureGpuBytesEstimated},
          {"vertexUpload", r.vertexUploadBytes},
          {"indexUpload", r.indexUploadBytes},
          {"peakStaging", r.peakStagingBytes}}},
        {"async",
         {{"preparedCpuBytes", stats.preparedCpuBytes},
          {"uploadPumpCalls", r.uploadPumpCalls},
          {"maxUploadBytesPerPump", r.maxUploadBytesPerPump}}},
        {"synchronization",
         {{"legacySubmits", r.singleTimeSubmits},
          {"queueWaitIdleCalls", r.queueWaitIdleCalls},
          {"batchSubmits", r.batchSubmits},
          {"completedBatchSubmits", r.completedBatchSubmits},
          {"fenceWaitCalls", r.fenceWaitCalls},
          {"fencePollCalls", r.fencePollCalls},
          {"peakInFlightBatches", r.peakInFlightBatches}}},
        {"vma",
         {{"before", allocatorSnapshotToJson(stats.allocatorBefore)},
          {"after", allocatorSnapshotToJson(stats.allocatorAfter)},
          {"delta",
           {{"allocationCount",
             memoryDelta(stats.allocatorAfter.allocationCount,
                         stats.allocatorBefore.allocationCount)},
            {"allocationBytes",
             memoryDelta(stats.allocatorAfter.allocationBytes,
                         stats.allocatorBefore.allocationBytes)},
            {"blockBytes", memoryDelta(stats.allocatorAfter.blockBytes,
                                        stats.allocatorBefore.blockBytes)}}}}}};
}

ControlJson sceneLoadTaskToJson(
    const std::shared_ptr<SceneLoadTask> &task,
    bool allowUnfinalizedTerminal = false) {
    if (!task)
        return nullptr;
    const SceneLoadState state = task->state.load();
    const bool finalized = task->finalized.load();
    const bool terminal =
        finalized ||
        (allowUnfinalizedTerminal && isTerminalSceneLoadState(state));
    ControlJson result = {
        {"taskId", task->id},
        {"generation", task->generation},
        {"scene", task->sceneName},
        {"sceneIndex", task->sceneIndex},
        {"textureLimit", task->textureLimit},
        {"state", sceneLoadStateName(state)},
        {"terminal", terminal},
        {"finalized", finalized},
        {"progress",
         {{"texturesCompleted", task->progress.completedTextures.load()},
          {"texturesTotal", task->progress.totalTextures.load()},
          {"meshesCompleted", task->progress.completedMeshes.load()},
          {"meshesTotal", task->progress.totalMeshes.load()},
          {"texturesUploaded", task->progress.uploadedTextures.load()},
          {"textureUploadTotal", task->progress.uploadTextureTotal.load()},
          {"meshesUploaded", task->progress.uploadedMeshes.load()},
          {"meshUploadTotal", task->progress.uploadMeshTotal.load()},
          {"processedBytes", task->progress.processedBytes.load()}}}};
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        if (!task->error.empty())
            result["error"] = task->error;
    }
    if (finalized)
        result["loadStats"] = sceneLoadStatsToJson(task->stats);
    return result;
}

void validateSceneLoadStats(const SceneLoadStats &stats) {
    const double detailedMax =
        std::max({stats.deviceIdleMs, stats.teardownMs, stats.sceneFactoryMs,
                  stats.gltfParseMs, stats.textureFileReadMs,
                  stats.textureDecodeMs, stats.resources.textureResizeMs,
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
    if (stats.resources.fenceWaitCalls > stats.resources.batchSubmits) {
        VKR_LOG_WARN("LoadStats",
                     "Scene '{}' has invalid batch synchronization: "
                     "submits={}, fence waits={}.",
                     stats.sceneName, stats.resources.batchSubmits,
                     stats.resources.fenceWaitCalls);
    }
    if (stats.resources.completedBatchSubmits >
        stats.resources.batchSubmits) {
        VKR_LOG_WARN("LoadStats",
                     "Scene '{}' completed more upload batches than it "
                     "submitted: completed={}, submits={}.",
                     stats.sceneName,
                     stats.resources.completedBatchSubmits,
                     stats.resources.batchSubmits);
    }
}

void logSceneLoadStats(const SceneLoadStats &stats) {
    const int64_t allocationDelta =
        memoryDelta(stats.allocatorAfter.allocationBytes,
                    stats.allocatorBefore.allocationBytes);
    const int64_t blockDelta = memoryDelta(stats.allocatorAfter.blockBytes,
                                           stats.allocatorBefore.blockBytes);

    VKR_LOG_INFO(
        "LoadStats",
        "scene='{}' success={} limit={} total={:.2f}ms factory={:.2f}ms "
        "textures={} meshes={} materials={} objects={} cache={}/{} "
        "upload={:.2f}MiB "
        "legacySubmits={} batchSubmits={} queueWaits={} fenceWaits={} "
        "fencePolls={} "
        "vmaAllocationDelta={:.2f}MiB vmaBlockDelta={:.2f}MiB",
        stats.sceneName, stats.success,
        stats.maxTextureSize == 0 ? std::string("Full")
                                  : std::to_string(stats.maxTextureSize),
        stats.totalMs, stats.sceneFactoryMs,
        stats.resources.gpuTextureCount, stats.resources.gpuMeshCount,
        stats.materialCount, stats.objectCount,
        stats.resources.derivedTextureHits,
        stats.resources.derivedTextureLookups,
        bytesToMiB(stats.resources.textureUploadBytes +
                   stats.resources.vertexUploadBytes +
                   stats.resources.indexUploadBytes),
        stats.resources.singleTimeSubmits,
        stats.resources.batchSubmits,
        stats.resources.queueWaitIdleCalls,
        stats.resources.fenceWaitCalls,
        stats.resources.fencePollCalls,
        signedBytesToMiB(allocationDelta), signedBytesToMiB(blockDelta));

    VKR_LOG_DEBUG(
        "LoadStats",
        "timings idle={:.2f}ms teardown={:.2f}ms parse={:.2f}ms "
        "imageRead={:.2f}ms decode={:.2f}ms resize={:.2f}ms "
        "textureUpload={:.2f}ms material={:.2f}ms meshCpu={:.2f}ms "
        "meshUpload={:.2f}ms batchWait={:.2f}ms hierarchy={:.2f}ms; "
        "peakStaging={:.2f}MiB; "
        "textureBytes "
        "encoded={:.2f}MiB decoded={:.2f}MiB baseUpload={:.2f}MiB "
        "gpuEstimated={:.2f}MiB; VMA allocations {} -> {}, bytes "
        "{:.2f}MiB -> {:.2f}MiB, blocks {:.2f}MiB -> {:.2f}MiB",
        stats.deviceIdleMs, stats.teardownMs, stats.gltfParseMs,
        stats.textureFileReadMs, stats.textureDecodeMs,
        stats.resources.textureResizeMs, stats.resources.textureUploadMs,
        stats.materialSetupMs, stats.meshCpuMs,
        stats.resources.meshUploadMs, stats.resources.batchSubmitWaitMs,
        stats.hierarchyMs,
        bytesToMiB(stats.resources.peakStagingBytes),
        bytesToMiB(stats.resources.encodedSourceBytes),
        bytesToMiB(stats.resources.decodedRgbaBytes),
        bytesToMiB(stats.resources.textureUploadBytes),
        bytesToMiB(stats.resources.textureGpuBytesEstimated),
        stats.allocatorBefore.allocationCount,
        stats.allocatorAfter.allocationCount,
        bytesToMiB(stats.allocatorBefore.allocationBytes),
        bytesToMiB(stats.allocatorAfter.allocationBytes),
        bytesToMiB(stats.allocatorBefore.blockBytes),
        bytesToMiB(stats.allocatorAfter.blockBytes));
}

} // namespace

struct SceneImportWorkerState {
    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> completedBytes{0};
    std::atomic<uint64_t> totalBytes{0};
    std::mutex mutex;
    std::string currentFile;
};

struct SceneImportUiState {
    std::future<SceneImportPreflight> preflightFuture;
    std::future<SceneImportResult> importFuture;
    std::optional<SceneImportPreflight> preflight;
    std::shared_ptr<SceneImportWorkerState> worker;
    std::array<char, 192> displayName{};
    std::array<char, 128> sceneId{};
    std::vector<std::string> profileIds;
    int profileIndex = 0;
    bool requestOpenModal = false;
    bool referenceExisting = false;
    bool loadAfterImport = true;
    bool loadAfterActiveImport = true;
    std::string status;
    std::string error;
};

struct SceneAssetOperationState {
    AssetLoadCoordinator coordinator;
    std::unordered_map<uint64_t, uint64_t> importToLoadTask;
    std::unordered_set<uint64_t> processedImports;
    std::unordered_map<std::string, ArtifactStatus> statuses;
    int selectedSceneIndex = -1;
    std::array<char, 128> search{};
    std::string status;
    std::string error;
};

namespace {

ControlJson loadOperationToJson(
    const std::shared_ptr<AssetImportTask> &importTask,
    const std::shared_ptr<SceneLoadTask> &loadTask) {
    ControlJson result = assetImportTaskToJson(importTask);
    if (!importTask || !loadTask)
        return result;

    const uint64_t operationId = importTask->id;
    ControlJson load = sceneLoadTaskToJson(loadTask, true);
    ControlJson import = result;
    result["importTask"] = std::move(import);
    result["loadTask"] = load;
    result["taskId"] = operationId;
    result["state"] = load.value("state", std::string("Queued"));
    result["terminal"] = load.value("terminal", false);
    const SceneLoadState state = loadTask->state.load();
    result["phase"] =
        state == SceneLoadState::Uploading ||
                state == SceneLoadState::WaitingForGpu ||
                state == SceneLoadState::ReadyToPublish ||
                state == SceneLoadState::ReleasingPreviousScene
            ? "uploading"
            : (isTerminalSceneLoadState(state) ? "complete" : "preparing");
    if (load.contains("error"))
        result["error"] = load["error"];
    if (load.contains("loadStats"))
        result["loadStats"] = load["loadStats"];
    return result;
}

ControlJson artifactStatusToJson(const SceneEntry &entry,
                                 const std::string &profileId,
                                 const ArtifactStatus &status) {
    return {{"sceneId", entry.id},
            {"scene", entry.name},
            {"source", entry.sourcePath},
            {"profileId", profileId},
            {"state", artifactStateName(status.state)},
            {"ready", status.ready()},
            {"reason", status.reason},
            {"manifest", status.manifestPath.string()},
            {"artifactCount", status.entryCount},
            {"blobBytes", status.blobBytes}};
}

} // namespace

Application::Application(const Config &config, ProjectContext projectContext,
                         SceneCatalog catalog)
    : config_(config), projectContext_(std::move(projectContext)),
      catalog_(std::move(catalog)) {
    if (config_.derivedTextureCachePath.empty())
        config_.derivedTextureCachePath = projectContext_.cacheRoot.string();
    sceneRegistry_ = buildSceneRegistry(catalog_, projectContext_, config_);
    sceneImportUi_ = std::make_unique<SceneImportUiState>();
    sceneAssetOperations_ = std::make_unique<SceneAssetOperationState>();
}

Application::~Application() {
    if (runtimeControlServer_)
        runtimeControlServer_->stop();
    if (sceneImportUi_) {
        if (sceneImportUi_->worker)
            sceneImportUi_->worker->cancel = true;
        if (sceneImportUi_->preflightFuture.valid())
            sceneImportUi_->preflightFuture.wait();
        if (sceneImportUi_->importFuture.valid())
            sceneImportUi_->importFuture.wait();
    }
    if (assetImportManager_)
        assetImportManager_->shutdown();
    if (sceneGpuBuilder_)
        sceneGpuBuilder_->cancel();
    sceneGpuBuilder_.reset();
    if (sceneLoadManager_)
        sceneLoadManager_->shutdown();
    if (device_)
        vkDeviceWaitIdle(device_->logicalDevice());
}

void Application::run() {
    init();
    if (config_.enableRuntimeControl) {
        runtimeCommandQueue_ = std::make_unique<RuntimeCommandQueue>();
        runtimeControlServer_ =
            std::make_unique<NamedPipeServerWin32>(*runtimeCommandQueue_);
        runtimeControlServer_->start();
    } else {
        VKR_LOG_INFO(
            "Control",
            "Runtime control disabled; pass --runtime-control to enable.");
    }
    try {
        mainLoop();
    } catch (...) {
        if (runtimeControlServer_)
            runtimeControlServer_->stop();
        throw;
    }
    if (runtimeControlServer_)
        runtimeControlServer_->stop();
}

void Application::registerScene(SceneEntry entry) {
    sceneRegistry_.push_back(std::move(entry));
}

void Application::init() {
#ifndef NDEBUG
    runResourcePoolSelfTest();
#endif

    shaderVariants_.assign(kShaderVariants.begin(), kShaderVariants.end());
    for (ShaderVariant &variant : shaderVariants_) {
        variant.vertSpvPath =
            projectContext_.resolveRuntimePath(variant.vertSpvPath).string();
        variant.fragSpvPath =
            projectContext_.resolveRuntimePath(variant.fragSpvPath).string();
    }

    window_ = std::make_unique<Window>(
        config_.windowWidth, config_.windowHeight, config_.windowTitle);
    input_ = std::make_unique<InputManager>(*window_);

    auto extensions = Window::getRequiredVulkanExtensions();
    context_ = std::make_unique<VulkanContext>(
        [this](VkInstance inst) { return window_->createSurface(inst); },
        std::move(extensions), config_.enableValidation);
    device_ = std::make_unique<Device>(*context_);
    descriptorAllocator_ = std::make_unique<DescriptorAllocator>(*device_);
    swapChain_ =
        std::make_unique<SwapChain>(*device_, context_->surface(), [this]() {
            return window_->framebufferExtent();
        });
    frameSync_ = std::make_unique<FrameSync>(*device_, *swapChain_);
    renderer_ = std::make_unique<Renderer>(
        *device_, *swapChain_, *frameSync_, *descriptorAllocator_,
        sizeof(GlobalUBO));

    window_->setResizeCallback(
        [this](int, int) { frameSync_->notifyResize(); });

    camera_.setAspect(static_cast<float>(swapChain_->extent().width) /
                      static_cast<float>(swapChain_->extent().height));

    if (sceneRegistry_.empty())
        throw std::runtime_error("No scenes registered; call "
                                 "Application::registerScene before run().");

    const int start = std::clamp(config_.defaultSceneIndex, 0,
                                 static_cast<int>(sceneRegistry_.size()) - 1);
    pipelineCache_ = std::make_unique<PipelineCache>(*device_);
    sceneLoadManager_ = std::make_unique<SceneLoadManager>();
    if (config_.assetImportMode == AssetImportMode::OnDemand) {
        assetImportManager_ = std::make_unique<AssetImportManager>(
            AssetImportManagerOptions{
                projectContext_.projectRoot, config_.derivedTextureCachePath,
                config_.assetToolPath, config_.assetImportWorkers,
                config_.assetImportMemoryBudgetMiB});
    }
    sceneLoadContext_.maxTextureSize = config_.gltfMaxTextureSize;
    sceneLoadContext_.derivedTextureCachePath =
        config_.derivedTextureCachePath;
    sceneLoadContext_.projectId = catalog_.projectId;
    sceneLoadContext_.textureTranscodeTarget =
        device_->textureTranscodeTarget();
    sceneLoadContext_.requireDerivedTextures =
        config_.assetImportMode == AssetImportMode::CookedOnly;
    reloadArtifactIndex();
    refreshAllArtifactStatuses();
    sceneAssetOperations_->selectedSceneIndex = start;
    if (sceneRegistry_[start].builtin)
        loadScene(start);
    else
        requestSceneOperation(start);

    // ImGui on top of the main render pass.
    gui_ = std::make_unique<GuiSystem>(
        context_->instance(), *device_, renderer_->renderPass(),
        window_->handle(), swapChain_->imageCount(), swapChain_->imageCount());
}

void Application::loadScene(int index, bool replaceCurrent) {
    const auto &entry = sceneRegistry_[index];
    if (!entry.available)
        throw RuntimeCommandError("scene_unavailable",
                                  entry.unavailableReason);
    sceneLoadContext_.sceneId = entry.id;
    sceneLoadContext_.profileId = profileIdForTextureLimit(entry);
    SceneLoadStats stats{};
    stats.sceneName = entry.name;
    stats.maxTextureSize = sceneLoadContext_.maxTextureSize;
    const auto totalStart = std::chrono::steady_clock::now();

    if (replaceCurrent) {
        {
            ScopedLoadTimer idleTimer(&stats.deviceIdleMs);
            ++stats.deviceWaitIdleCalls;
            vkDeviceWaitIdle(device_->logicalDevice());
        }
        {
            ScopedLoadTimer teardownTimer(&stats.teardownMs);
            pipelineCache_->clear();
            currentScene_.reset();
        }
    }

    stats.allocatorBefore = device_->allocatorMemorySnapshot();
    const UploadSyncCounters syncBefore = frameSync_->uploadSyncCounters();
    sceneLoadContext_.loadStats = &stats;

    try {
        std::unique_ptr<Scene> loadedScene;
        {
            ScopedLoadTimer factoryTimer(&stats.sceneFactoryMs);
            if (entry.factory) {
                UploadContext upload(*device_, &stats.resources);
                loadedScene = entry.factory(*device_, upload,
                                            *descriptorAllocator_,
                                            sceneLoadContext_);
                upload.finish();
            } else if (entry.prepareFactory) {
                auto task = std::make_shared<SceneLoadTask>();
                task->sceneIndex = index;
                task->sceneName = entry.name;
                task->textureLimit = sceneLoadContext_.maxTextureSize;
                task->stats = stats;
                SceneLoadContext context = sceneLoadContext_;
                context.loadStats = &task->stats;
                auto prepared = std::make_unique<PreparedSceneData>(
                    entry.prepareFactory(context,
                                         CancellationToken(task->cancellation),
                                         task->progress));
                SceneGpuBuilder builder(*device_, *descriptorAllocator_, task,
                                        std::move(prepared));
                const SceneGpuBuilder::Budget startupBudget{
                    std::numeric_limits<uint64_t>::max(), 1000.0};
                while (!builder.finished()) {
                    builder.pump(startupBudget);
                    std::this_thread::yield();
                }
                if (!builder.ready()) {
                    std::lock_guard<std::mutex> lock(task->mutex);
                    throw std::runtime_error(
                        task->error.empty() ? "Initial scene load failed"
                                            : task->error);
                }
                loadedScene = builder.takeScene();
                stats = task->stats;
            } else {
                throw std::runtime_error("Scene entry has no load factory");
            }
        }
        sceneLoadContext_.loadStats = nullptr;

        stats.materialCount = loadedScene ? loadedScene->materials().size() : 0;
        stats.objectCount = loadedScene ? loadedScene->objects().size() : 0;
        stats.allocatorAfter = device_->allocatorMemorySnapshot();
        const UploadSyncCounters syncAfter = frameSync_->uploadSyncCounters();
        stats.resources.singleTimeSubmits +=
            syncAfter.singleTimeSubmits - syncBefore.singleTimeSubmits;
        stats.resources.queueWaitIdleCalls +=
            syncAfter.queueWaitIdleCalls - syncBefore.queueWaitIdleCalls;

        currentScene_ = std::move(loadedScene);
        currentSceneIndex_ = index;
        if (currentScene_->initialCamera) {
            const auto &p = *currentScene_->initialCamera;
            camera_.setPosition(p.position);
            camera_.setYawPitch(p.yaw, p.pitch);
        }
        applySceneCameraDefaults();
        stats.success = true;
        if (artifactIndex_ && !entry.builtin) {
            artifactIndex_->touch(entry.id, sceneLoadContext_.profileId);
            persistArtifactIndex();
        }
    } catch (...) {
        sceneLoadContext_.loadStats = nullptr;
        stats.allocatorAfter = device_->allocatorMemorySnapshot();
        const UploadSyncCounters syncAfter = frameSync_->uploadSyncCounters();
        stats.resources.singleTimeSubmits +=
            syncAfter.singleTimeSubmits - syncBefore.singleTimeSubmits;
        stats.resources.queueWaitIdleCalls +=
            syncAfter.queueWaitIdleCalls - syncBefore.queueWaitIdleCalls;
        stats.totalMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - totalStart)
                            .count();
        lastSceneLoadStats_ = stats;
        validateSceneLoadStats(stats);
        logSceneLoadStats(stats);
        throw;
    }

    stats.totalMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - totalStart)
                        .count();
    lastSceneLoadStats_ = stats;
    validateSceneLoadStats(stats);
    logSceneLoadStats(stats);
}

uint64_t Application::reloadCurrentScene() {
    if (currentSceneIndex_ < 0 ||
        currentSceneIndex_ >= static_cast<int>(sceneRegistry_.size()))
        return 0;

    const int index = currentSceneIndex_;
    const uint64_t taskId = requestSceneOperation(index);
    if (taskId != 0) {
        VKR_LOG_INFO("Scene", "Requested reload of {} with glTF texture limit {}",
                     sceneRegistry_[index].name,
                     sceneLoadContext_.maxTextureSize == 0
                         ? std::string("Full")
                         : std::to_string(sceneLoadContext_.maxTextureSize));
        return taskId;
    }

    VKR_LOG_INFO("Scene", "Reloaded {} with glTF texture limit {}",
                 sceneRegistry_[index].name,
                 sceneLoadContext_.maxTextureSize == 0
                     ? std::string("Full")
                     : std::to_string(sceneLoadContext_.maxTextureSize));
    return taskId;
}

void Application::switchScene(int index) {
    if (index < 0 || index >= static_cast<int>(sceneRegistry_.size()))
        return;
    if (index == currentSceneIndex_ && !sceneGpuBuilder_ &&
        (!latestSceneLoadTask_ ||
         isTerminalSceneLoadState(latestSceneLoadTask_->state.load())))
        return;

    const uint64_t taskId = requestSceneOperation(index);
    if (taskId != 0) {
        VKR_LOG_INFO("Scene", "Requested switch to {}",
                     sceneRegistry_[index].name);
        return;
    }

    VKR_LOG_INFO("Scene", "Switched to {}", sceneRegistry_[index].name);
}

uint64_t Application::setTextureLimit(uint32_t limit) {
    if (limit != 0 && limit != 512 && limit != 1024 && limit != 2048)
        throw RuntimeCommandError("invalid_texture_limit",
                                  "Texture limit must be 0, 512, 1024, or "
                                  "2048.");
    if (sceneLoadContext_.maxTextureSize == limit)
        return 0;
    if (config_.assetImportMode == AssetImportMode::CookedOnly) {
        throw RuntimeCommandError(
            "texture_limit_locked",
            "Texture limit is fixed by the cooked package profile.");
    }

    sceneLoadContext_.maxTextureSize = limit;
    refreshAllArtifactStatuses();
    VKR_LOG_INFO("Renderer", "glTF texture limit set to {}",
                 textureLimitLabel(limit));
    if (latestSceneLoadTask_ &&
        !isTerminalSceneLoadState(latestSceneLoadTask_->state.load())) {
        return requestSceneOperation(latestSceneLoadTask_->sceneIndex);
    } else {
        return reloadCurrentScene();
    }
}

uint64_t Application::requestSceneLoad(int index, bool sourceFallback) {
    if (index < 0 || index >= static_cast<int>(sceneRegistry_.size()))
        throw RuntimeCommandError("invalid_scene", "Invalid scene index.");
    const SceneEntry &entry = sceneRegistry_[index];
    if (!entry.available)
        throw RuntimeCommandError("scene_unavailable",
                                  entry.unavailableReason);
    sceneLoadContext_.sceneId = entry.id;
    sceneLoadContext_.profileId =
        sourceFallback ? std::string("explicit_source_fallback")
                       : profileIdForTextureLimit(entry);
    if (!entry.prepareFactory) {
        if (sceneLoadManager_) {
            if (latestSceneLoadTask_)
                sceneLoadManager_->cancel(latestSceneLoadTask_->id);
            else
                sceneLoadManager_->cancelActive();
        }
        if (sceneGpuBuilder_) {
            const auto cancelledTask = sceneGpuBuilder_->task();
            sceneGpuBuilder_->cancel();
            vkDeviceWaitIdle(device_->logicalDevice());
            sceneGpuBuilder_.reset();
            cancelledTask->stats.allocatorAfter =
                device_->allocatorMemorySnapshot();
            finalizeSceneLoad(cancelledTask, false);
        }
        latestSceneLoadTask_.reset();
        loadScene(index, true);
        return 0;
    }
    if (sceneGpuBuilder_)
        sceneGpuBuilder_->cancel();
    latestSceneLoadTask_ = sceneLoadManager_->request(
        index, entry.name, entry.prepareFactory, sceneLoadContext_);
    latestSceneLoadTask_->stats.allocatorBefore =
        device_->allocatorMemorySnapshot();
    VKR_LOG_INFO("Scene", "Queued load task {} for {}",
                 latestSceneLoadTask_->id, entry.name);
    return latestSceneLoadTask_->id;
}

bool Application::cancelSceneLoad(uint64_t taskId) {
    if (sceneGpuBuilder_ && sceneGpuBuilder_->task()->id == taskId) {
        sceneGpuBuilder_->cancel();
        return true;
    }
    return sceneLoadManager_ && sceneLoadManager_->cancel(taskId);
}

void Application::updateSceneLoading() {
    if (sceneGpuBuilder_) {
        sceneGpuBuilder_->pump();
        const auto task = sceneGpuBuilder_->task();
        if (sceneGpuBuilder_->ready()) {
            currentScene_ = sceneGpuBuilder_->takeScene();
            currentSceneIndex_ = task->sceneIndex;
            if (currentScene_ && currentScene_->initialCamera) {
                const auto &pose = *currentScene_->initialCamera;
                camera_.setPosition(pose.position);
                camera_.setYawPitch(pose.yaw, pose.pitch);
            }
            applySceneCameraDefaults();
            sceneGpuBuilder_.reset();
            task->stats.allocatorAfter = device_->allocatorMemorySnapshot();
            finalizeSceneLoad(task, true);
        } else if (sceneGpuBuilder_->finished()) {
            sceneGpuBuilder_.reset();
            task->stats.allocatorAfter = device_->allocatorMemorySnapshot();
            finalizeSceneLoad(task, false);
        }
    }

    if (sceneGpuBuilder_ || !latestSceneLoadTask_)
        return;

    const SceneLoadState state = latestSceneLoadTask_->state.load();
    if (state == SceneLoadState::ReadyForUpload) {
        auto prepared =
            sceneLoadManager_->takePrepared(latestSceneLoadTask_->id);
        if (!prepared)
            return;
        latestSceneLoadTask_->state =
            SceneLoadState::ReleasingPreviousScene;
        {
            ScopedLoadTimer idleTimer(
                &latestSceneLoadTask_->stats.deviceIdleMs);
            ++latestSceneLoadTask_->stats.deviceWaitIdleCalls;
            vkDeviceWaitIdle(device_->logicalDevice());
        }
        {
            ScopedLoadTimer teardownTimer(
                &latestSceneLoadTask_->stats.teardownMs);
            pipelineCache_->clear();
            currentScene_.reset();
            currentSceneIndex_ = -1;
        }
        latestSceneLoadTask_->stats.allocatorBefore =
            device_->allocatorMemorySnapshot();
        sceneGpuBuilder_ = std::make_unique<SceneGpuBuilder>(
            *device_, *descriptorAllocator_, latestSceneLoadTask_,
            std::move(prepared));
    } else if ((state == SceneLoadState::Failed ||
                state == SceneLoadState::Cancelled) &&
               latestSceneLoadTask_->id != lastFinalizedTaskId_) {
        latestSceneLoadTask_->stats.allocatorAfter =
            device_->allocatorMemorySnapshot();
        finalizeSceneLoad(latestSceneLoadTask_, false);
    }
}

void Application::finalizeSceneLoad(
    const std::shared_ptr<SceneLoadTask> &task, bool success) {
    if (!task || task->id == lastFinalizedTaskId_)
        return;
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
    lastSceneLoadStats_ = task->stats;
    lastFinalizedTaskId_ = task->id;
    validateSceneLoadStats(task->stats);
    logSceneLoadStats(task->stats);
    if (success && artifactIndex_ && task->sceneIndex >= 0 &&
        task->sceneIndex < static_cast<int>(sceneRegistry_.size())) {
        const SceneEntry &entry = sceneRegistry_[task->sceneIndex];
        if (!entry.builtin) {
            std::string profileId;
            const auto preferred = catalog_.importProfiles.find(entry.profileId);
            if (preferred != catalog_.importProfiles.end() &&
                preferred->second.textureLimit == task->textureLimit) {
                profileId = preferred->first;
            } else {
                for (const auto &candidate : catalog_.importProfiles) {
                    if (candidate.second.textureLimit == task->textureLimit) {
                        profileId = candidate.first;
                        break;
                    }
                }
            }
            if (!profileId.empty()) {
                artifactIndex_->touch(entry.id, profileId);
                persistArtifactIndex();
            }
        }
    }
    task->finalized = true;
}

void Application::setShaderVariant(int index) {
    if (index < 0 || index >= static_cast<int>(shaderVariants_.size()))
        throw RuntimeCommandError("invalid_shader", "Invalid shader index.");
    if (currentShaderVariantIndex_ == index)
        return;
    currentShaderVariantIndex_ = index;
    VKR_LOG_INFO("Renderer", "Shader variant switched to {}",
                 shaderVariants_[index].displayName);
}

int Application::findSceneIndexByName(const std::string &name) const {
    for (int i = 0; i < static_cast<int>(sceneRegistry_.size()); ++i) {
        if (asciiEqualsIgnoreCase(sceneRegistry_[i].name, name))
            return i;
    }
    return -1;
}

int Application::findShaderVariantIndexByName(const std::string &name) const {
    for (int i = 0; i < static_cast<int>(shaderVariants_.size()); ++i) {
        if (asciiEqualsIgnoreCase(shaderVariants_[i].displayName, name))
            return i;
    }
    return -1;
}

std::string
Application::profileIdForTextureLimit(const SceneEntry &entry) const {
    const auto preferred = catalog_.importProfiles.find(entry.profileId);
    if (preferred != catalog_.importProfiles.end() &&
        preferred->second.textureLimit == sceneLoadContext_.maxTextureSize)
        return preferred->first;
    for (const auto &pair : catalog_.importProfiles) {
        if (pair.second.textureLimit == sceneLoadContext_.maxTextureSize)
            return pair.first;
    }
    return sceneLoadContext_.maxTextureSize == 0
               ? std::string("runtime_full")
               : std::string("runtime_") +
                     std::to_string(sceneLoadContext_.maxTextureSize);
}

void Application::reloadArtifactIndex() {
    try {
        bool rebuilt = false;
        std::string diagnostic;
        artifactIndex_ = std::make_unique<ArtifactIndex>(
            ArtifactIndex::loadOrRebuild(
                sceneLoadContext_.derivedTextureCachePath,
                projectContext_.projectRoot, catalog_, &rebuilt,
                &diagnostic));
        artifactUsage_ = artifactIndex_->usage();
        VKR_LOG_INFO("Assets", "Artifact index {} at {} ({} records)",
                     rebuilt ? "rebuilt" : "loaded",
                     artifactIndex_->path().string(),
                     artifactIndex_->records().size());
        if (rebuilt && diagnostic != "index not found")
            VKR_LOG_WARN("Assets", "Artifact index rebuild reason: {}",
                         diagnostic);
    } catch (const std::exception &error) {
        artifactIndex_.reset();
        artifactUsage_.reset();
        VKR_LOG_WARN("Assets",
                     "Artifact index is unavailable; using manifest scans: {}",
                     error.what());
    }
}

void Application::persistArtifactIndex() {
    if (!artifactIndex_ ||
        config_.assetImportMode == AssetImportMode::CookedOnly)
        return;
    try {
        artifactIndex_->save();
        artifactUsage_ = artifactIndex_->usage();
    } catch (const std::exception &error) {
        VKR_LOG_WARN("Assets", "Could not persist ArtifactIndex telemetry: {}",
                     error.what());
    }
}

void Application::refreshArtifactStatus(int sceneIndex, bool admission) {
    if (sceneIndex < 0 ||
        sceneIndex >= static_cast<int>(sceneRegistry_.size()))
        return;
    const SceneEntry &entry = sceneRegistry_[sceneIndex];
    const std::string profileId = profileIdForTextureLimit(entry);
    const std::string key = artifactStatusKey(entry.id, profileId);
    if (entry.builtin) {
        ArtifactStatus status;
        status.state = ArtifactState::Ready;
        status.reason = "builtin scene";
        sceneAssetOperations_->statuses[key] = std::move(status);
        return;
    }

    ArtifactStatus status;
    if (!entry.available) {
        status.state = ArtifactState::Missing;
        status.reason = entry.unavailableReason;
    } else {
        const ArtifactStatusRequest request{
            sceneLoadContext_.derivedTextureCachePath, entry.sourcePath,
            catalog_.projectId, entry.id, profileId,
            sceneLoadContext_.maxTextureSize};
        status = artifactIndex_
                     ? artifactIndex_->query(
                           request, admission ? ArtifactValidationMode::Admission
                                              : ArtifactValidationMode::Fast)
                     : inspectTextureArtifacts(request);
    }
    if (assetImportManager_) {
        for (const auto &task : assetImportManager_->history()) {
            if (task->sceneId == entry.id && task->profileId == profileId &&
                !isTerminalAssetImportState(task->state.load())) {
                status.state = ArtifactState::Importing;
                status.reason = std::string("asset import ") +
                                assetImportStateName(task->state.load());
                break;
            }
        }
    }
    sceneAssetOperations_->statuses[key] = std::move(status);
}

void Application::refreshAllArtifactStatuses() {
    if (!sceneAssetOperations_)
        return;
    sceneAssetOperations_->statuses.clear();
    for (int i = 0; i < static_cast<int>(sceneRegistry_.size()); ++i)
        refreshArtifactStatus(i);
    if (artifactIndex_)
        artifactUsage_ = artifactIndex_->usage();
}

uint64_t Application::requestSceneOperation(int index, bool sourceFallback,
                                            bool loadAfter,
                                            ImportReason reason,
                                            bool forceReimport) {
    if (index < 0 || index >= static_cast<int>(sceneRegistry_.size()))
        throw RuntimeCommandError("invalid_scene", "Invalid scene index.");
    const SceneEntry &entry = sceneRegistry_[index];
    if (!entry.available)
        throw RuntimeCommandError("scene_unavailable",
                                  entry.unavailableReason);

    const uint64_t generation =
        sceneAssetOperations_->coordinator.beginOperation();
    sceneAssetOperations_->selectedSceneIndex = index;
    sceneAssetOperations_->error.clear();

    if (entry.builtin) {
        return loadAfter ? requestSceneLoad(index) : 0;
    }
    if (sourceFallback) {
        if (config_.assetImportMode == AssetImportMode::CookedOnly) {
            throw RuntimeCommandError(
                "source_fallback_disabled",
                "Source fallback is disabled in CookedOnly mode.");
        }
        return loadAfter ? requestSceneLoad(index, true) : 0;
    }

    refreshArtifactStatus(index, true);
    const std::string profileId = profileIdForTextureLimit(entry);
    const auto found = sceneAssetOperations_->statuses.find(
        artifactStatusKey(entry.id, profileId));
    const bool ready = found != sceneAssetOperations_->statuses.end() &&
                       found->second.ready();
    if (ready && !forceReimport)
        return loadAfter ? requestSceneLoad(index) : 0;

    if (config_.assetImportMode != AssetImportMode::OnDemand) {
        const std::string mode = assetImportModeName(config_.assetImportMode);
        throw RuntimeCommandError(
            "artifact_not_ready",
            "Derived artifacts are not ready for scene '" + entry.name +
                "' (mode=" + mode + "). " +
                (found == sceneAssetOperations_->statuses.end()
                     ? std::string("No artifact status is available.")
                     : found->second.reason));
    }

    auto task = assetImportManager_->request(
        {entry.id, profileId, reason, forceReimport});
    if (loadAfter) {
        sceneAssetOperations_->coordinator.attach(task->id, generation, index);
    }
    refreshArtifactStatus(index);
    sceneAssetOperations_->status =
        "Importing " + entry.name + " (" + profileId + ")";
    VKR_LOG_INFO("Assets", "Queued import task {} for scene '{}' profile '{}'",
                 task->id, entry.id, profileId);
    return task->id;
}

void Application::updateAssetImports() {
    if (!assetImportManager_)
        return;
    const auto tasks = assetImportManager_->history();
    for (const auto &task : tasks) {
        if (!isTerminalAssetImportState(task->state.load()))
            continue;
        if (!sceneAssetOperations_->processedImports.insert(task->id).second)
            continue;

        int sceneIndex = -1;
        for (int i = 0; i < static_cast<int>(sceneRegistry_.size()); ++i) {
            if (sceneRegistry_[i].id == task->sceneId) {
                sceneIndex = i;
                break;
            }
        }
        const AssetImportState state = task->state.load();
        if (state == AssetImportState::Completed) {
            reloadArtifactIndex();
            if (artifactIndex_) {
                artifactIndex_->recordImportSuccess(
                    task->sceneId, task->profileId, task->id);
                persistArtifactIndex();
            }
        } else if (state == AssetImportState::Failed && artifactIndex_) {
            std::lock_guard<std::mutex> lock(task->mutex);
            artifactIndex_->recordFailure(
                task->sceneId, task->profileId, "import_failed",
                task->error.empty() ? "Asset import failed" : task->error,
                task->logPath);
            persistArtifactIndex();
        }
        if (sceneIndex >= 0)
            refreshArtifactStatus(sceneIndex);

        if (state == AssetImportState::Completed && sceneIndex >= 0) {
            const auto status = sceneAssetOperations_->statuses.find(
                artifactStatusKey(task->sceneId, task->profileId));
            const bool ready =
                status != sceneAssetOperations_->statuses.end() &&
                status->second.ready();
            if (!ready) {
                sceneAssetOperations_->error =
                    "Import completed but artifacts failed validation: " +
                    (status == sceneAssetOperations_->statuses.end()
                         ? std::string("status unavailable")
                         : status->second.reason);
            } else if (const auto selected =
                           sceneAssetOperations_->coordinator.takeLatestScene(
                               task->id)) {
                const uint64_t loadTask = requestSceneLoad(*selected);
                sceneAssetOperations_->importToLoadTask[task->id] = loadTask;
                sceneAssetOperations_->status =
                    "Loading " + sceneRegistry_[*selected].name;
            }
        } else if (state == AssetImportState::Failed) {
            std::lock_guard<std::mutex> lock(task->mutex);
            sceneAssetOperations_->error =
                task->error.empty() ? "Asset import failed" : task->error;
        } else if (state == AssetImportState::Cancelled) {
            sceneAssetOperations_->status = "Asset import cancelled";
        }
        sceneAssetOperations_->coordinator.discard(task->id);
    }
}

bool Application::cancelLoadOperation(uint64_t taskId) {
    if ((taskId & AssetImportManager::kTaskIdMask) == 0)
        return cancelSceneLoad(taskId);
    const auto linked = sceneAssetOperations_->importToLoadTask.find(taskId);
    if (linked != sceneAssetOperations_->importToLoadTask.end() &&
        linked->second != 0)
        return cancelSceneLoad(linked->second);
    return assetImportManager_ && assetImportManager_->cancel(taskId);
}

void Application::processRuntimeCommand() {
    if (!runtimeCommandQueue_)
        return;
    std::shared_ptr<RuntimeCommand> command = runtimeCommandQueue_->popNext();
    if (!command)
        return;

    RuntimeDispatchResult dispatched =
        runtimeCommandDispatcher_.dispatch(*command, *this);
    command->response.set_value(std::move(dispatched.response));
    if (dispatched.requestQuit)
        pendingQuitCommand_ = std::move(command);
}

ControlJson Application::runtimeSystemInfo() {
    const BuildInfo &build = currentBuildInfo();
    ControlJson capabilities = {"async_scene_load", "load_status",
                                "load_cancel", "asset_catalog"};
    if (assetImportManager_) {
        capabilities.push_back("asset_import");
        capabilities.push_back("asset_cancel");
    }
    return {
        {"application", "VulkanLab"},
        {"protocolVersion", control::kProtocolVersion},
        {"capabilities", std::move(capabilities)},
        {"pipe", control::kPipeNameUtf8},
        {"scene", currentScene_ && currentSceneIndex_ >= 0
                      ? ControlJson(sceneRegistry_[currentSceneIndex_].name)
                      : ControlJson(nullptr)},
        {"sceneId", currentScene_ && currentSceneIndex_ >= 0
                        ? ControlJson(sceneRegistry_[currentSceneIndex_].id)
                        : ControlJson(nullptr)},
        {"projectId", catalog_.projectId},
        {"build",
         {{"revision", build.revision},
          {"dirty", build.dirty},
          {"configuration", build.configuration},
          {"compiler", build.compiler},
          {"vulkanSdk", build.vulkanSdk},
          {"glslc", build.glslc}}},
        {"projectRoot", projectContext_.projectRoot.string()},
        {"runtimeRoot", projectContext_.runtimeRoot.string()},
        {"assetMode", assetImportModeName(config_.assetImportMode)},
        {"cookedPackage", projectContext_.cookedPackage},
        {"cacheRoot", sceneLoadContext_.derivedTextureCachePath},
        {"captureRoot", projectContext_.captureRoot.string()},
        {"textureLimit", sceneLoadContext_.maxTextureSize},
        {"shader", shaderVariants_.empty()
                       ? ControlJson(nullptr)
                       : ControlJson(currentShaderVariant().displayName)},
        {"loadTask", sceneLoadTaskToJson(latestSceneLoadTask_)}};
}

ControlJson Application::runtimeSceneList() {
    ControlJson scenes = ControlJson::array();
    ControlJson entries = ControlJson::array();
    for (const auto &entry : sceneRegistry_)
        scenes.push_back(entry.name);
    for (const auto &entry : sceneRegistry_) {
        entries.push_back({{"id", entry.id},
                           {"name", entry.name},
                           {"profileId", entry.profileId},
                           {"available", entry.available},
                           {"source", entry.sourcePath}});
    }
    return {{"scenes", std::move(scenes)},
            {"entries", std::move(entries)}};
}

ControlJson Application::runtimeSceneCurrent() {
    return {{"name", currentScene_ && currentSceneIndex_ >= 0
                         ? ControlJson(sceneRegistry_[currentSceneIndex_].name)
                         : ControlJson(nullptr)},
            {"id", currentScene_ && currentSceneIndex_ >= 0
                       ? ControlJson(sceneRegistry_[currentSceneIndex_].id)
                       : ControlJson(nullptr)}};
}

ControlJson Application::runtimeSceneOperationResult(int index,
                                                     uint64_t taskId) {
    ControlJson result;
    if ((taskId & AssetImportManager::kTaskIdMask) != 0) {
        result =
            loadOperationToJson(assetImportManager_->task(taskId), nullptr);
    } else if (taskId != 0) {
        result = sceneLoadTaskToJson(latestSceneLoadTask_);
    } else {
        result = {{"scene", sceneRegistry_[index].name}, {"completed", true}};
    }
    if (taskId == 0 && lastSceneLoadStats_)
        result["loadStats"] = sceneLoadStatsToJson(*lastSceneLoadStats_);
    return result;
}

ControlJson Application::runtimeSceneLoad(const std::string &name) {
    const int index = findSceneIndexByName(name);
    if (index < 0) {
        ControlJson candidates = ControlJson::array();
        for (const auto &entry : sceneRegistry_)
            candidates.push_back(entry.name);
        throw RuntimeCommandError(
            "scene_not_found",
            "Unknown scene '" + name + "'. Available scenes: " +
                candidates.dump());
    }
    pendingSceneIndex_ = -1;
    return runtimeSceneOperationResult(index, requestSceneOperation(index));
}

ControlJson Application::runtimeSceneReload() {
    if (currentSceneIndex_ < 0)
        throw RuntimeCommandError("no_current_scene",
                                  "No scene is currently loaded.");
    const int index = currentSceneIndex_;
    return runtimeSceneOperationResult(index, requestSceneOperation(index));
}

ControlJson
Application::runtimeLoadStatus(std::optional<uint64_t> requestedTaskId) {
    uint64_t taskId = requestedTaskId.value_or(0);
    if (taskId == 0) {
        const auto activeImport =
            assetImportManager_ ? assetImportManager_->activeTask()
                                : std::shared_ptr<AssetImportTask>{};
        taskId = activeImport ? activeImport->id
                              : (latestSceneLoadTask_ ? latestSceneLoadTask_->id
                                                      : 0);
    }
    if ((taskId & AssetImportManager::kTaskIdMask) != 0) {
        if (!assetImportManager_)
            throw RuntimeCommandError("load_not_found",
                                      "Load task was not found.");
        const auto importTask = assetImportManager_->task(taskId);
        if (!importTask)
            throw RuntimeCommandError("load_not_found",
                                      "Load task was not found.");
        std::shared_ptr<SceneLoadTask> loadTask;
        const auto linked =
            sceneAssetOperations_->importToLoadTask.find(taskId);
        if (linked != sceneAssetOperations_->importToLoadTask.end() &&
            linked->second != 0)
            loadTask = sceneLoadManager_->task(linked->second);
        return loadOperationToJson(importTask, loadTask);
    }

    std::shared_ptr<SceneLoadTask> task = sceneLoadManager_->task(taskId);
    if (!task)
        throw RuntimeCommandError("load_not_found",
                                  "Load task was not found.");
    const bool superseded =
        !latestSceneLoadTask_ || latestSceneLoadTask_->id != task->id;
    return sceneLoadTaskToJson(task, superseded);
}

ControlJson
Application::runtimeLoadCancel(std::optional<uint64_t> requestedTaskId) {
    const auto activeImport =
        assetImportManager_ ? assetImportManager_->activeTask()
                            : std::shared_ptr<AssetImportTask>{};
    const uint64_t taskId = requestedTaskId.value_or(
        activeImport ? activeImport->id
                     : (latestSceneLoadTask_ ? latestSceneLoadTask_->id : 0));
    if (taskId == 0 || !cancelLoadOperation(taskId))
        throw RuntimeCommandError("load_not_cancellable",
                                  "Load task cannot be cancelled.");
    return {{"taskId", taskId}, {"cancelRequested", true}};
}

ControlJson Application::runtimeTextureLimitGet() {
    return {{"value", sceneLoadContext_.maxTextureSize}};
}

ControlJson Application::runtimeTextureLimitSet(uint32_t value) {
    const uint64_t taskId = setTextureLimit(value);
    ControlJson result = {{"value", sceneLoadContext_.maxTextureSize}};
    if ((taskId & AssetImportManager::kTaskIdMask) != 0) {
        result["loadTask"] =
            loadOperationToJson(assetImportManager_->task(taskId), nullptr);
        result["taskId"] = taskId;
    } else if (latestSceneLoadTask_ &&
               !isTerminalSceneLoadState(latestSceneLoadTask_->state.load())) {
        result["loadTask"] = sceneLoadTaskToJson(latestSceneLoadTask_);
    } else if (lastSceneLoadStats_) {
        result["loadStats"] = sceneLoadStatsToJson(*lastSceneLoadStats_);
    }
    return result;
}

ControlJson Application::runtimeIndexedArtifactStatus(
    int index, const std::string &profileId,
    const ArtifactStatus &status) const {
    const SceneEntry &entry = sceneRegistry_[index];
    ControlJson json = artifactStatusToJson(entry, profileId, status);
    if (!artifactIndex_)
        return json;
    const auto found = artifactIndex_->records().find(
        artifactIndexKey(entry.id, profileId));
    if (found == artifactIndex_->records().end())
        return json;
    const ArtifactIndexRecord &record = found->second;
    json["lastSuccessfulImportUnixMs"] = record.lastSuccessfulImportUnixMs;
    json["lastSuccessfulImportTaskId"] = record.lastSuccessfulImportTaskId;
    json["lastAccessUnixMs"] = record.lastAccessUnixMs;
    if (!record.failureCode.empty()) {
        json["lastFailure"] = {{"code", record.failureCode},
                               {"message", record.failureMessage},
                               {"log", record.failureLogPath},
                               {"unixMs", record.lastFailureUnixMs}};
    }
    return json;
}

int Application::runtimeAssetSceneIndex(const std::string &name) const {
    int index = findSceneIndexByName(name);
    if (index >= 0)
        return index;
    for (int i = 0; i < static_cast<int>(sceneRegistry_.size()); ++i) {
        if (asciiEqualsIgnoreCase(sceneRegistry_[i].id, name))
            return i;
    }
    return -1;
}

ControlJson Application::runtimeAssetCatalog() {
    refreshAllArtifactStatuses();
    ControlJson entries = ControlJson::array();
    for (int i = 0; i < static_cast<int>(sceneRegistry_.size()); ++i) {
        const SceneEntry &entry = sceneRegistry_[i];
        const std::string profileId = profileIdForTextureLimit(entry);
        const auto found = sceneAssetOperations_->statuses.find(
            artifactStatusKey(entry.id, profileId));
        if (found != sceneAssetOperations_->statuses.end()) {
            entries.push_back(
                runtimeIndexedArtifactStatus(i, profileId, found->second));
        }
    }
    return {{"projectId", catalog_.projectId},
            {"catalog", projectContext_.catalogPath.string()},
            {"mode", assetImportModeName(config_.assetImportMode)},
            {"entries", std::move(entries)}};
}

ControlJson Application::runtimeAssetStatus(
    const std::optional<std::string> &name) {
    const int index = name ? runtimeAssetSceneIndex(*name)
                           : sceneAssetOperations_->selectedSceneIndex;
    if (index < 0 || index >= static_cast<int>(sceneRegistry_.size()))
        throw RuntimeCommandError("scene_not_found",
                                  "Asset scene was not found.");
    refreshArtifactStatus(index);
    const SceneEntry &entry = sceneRegistry_[index];
    const std::string profileId = profileIdForTextureLimit(entry);
    return runtimeIndexedArtifactStatus(
        index, profileId,
        sceneAssetOperations_->statuses.at(
            artifactStatusKey(entry.id, profileId)));
}

ControlJson Application::runtimeAssetImport(const std::string &name,
                                            bool force, bool loadAfter) {
    if (config_.assetImportMode != AssetImportMode::OnDemand)
        throw RuntimeCommandError(
            "asset_import_disabled",
            "Asset import is only available in OnDemand mode.");
    const int index = runtimeAssetSceneIndex(name);
    if (index < 0)
        throw RuntimeCommandError("scene_not_found",
                                  "Asset scene was not found.");
    const uint64_t taskId = requestSceneOperation(
        index, false, loadAfter, ImportReason::ManualReimport, force);
    if (taskId != 0)
        return loadOperationToJson(assetImportManager_->task(taskId), nullptr);

    refreshArtifactStatus(index);
    const SceneEntry &entry = sceneRegistry_[index];
    const std::string profileId = profileIdForTextureLimit(entry);
    ControlJson result = runtimeIndexedArtifactStatus(
        index, profileId,
        sceneAssetOperations_->statuses.at(
            artifactStatusKey(entry.id, profileId)));
    result["terminal"] = true;
    return result;
}

ControlJson
Application::runtimeAssetCancel(std::optional<uint64_t> requestedTaskId) {
    uint64_t taskId = requestedTaskId.value_or(0);
    if (taskId == 0 && assetImportManager_) {
        if (const auto active = assetImportManager_->activeTask())
            taskId = active->id;
    }
    if (taskId == 0 ||
        (taskId & AssetImportManager::kTaskIdMask) == 0 ||
        !assetImportManager_ || !assetImportManager_->cancel(taskId)) {
        throw RuntimeCommandError("asset_not_cancellable",
                                  "Asset import cannot be cancelled.");
    }
    return {{"taskId", taskId}, {"cancelRequested", true}};
}

ControlJson Application::runtimeAssetCacheInfo() {
    const std::filesystem::path root =
        sceneLoadContext_.derivedTextureCachePath;
    if (artifactIndex_)
        artifactUsage_ = artifactIndex_->usage();
    const ArtifactIndexUsage usage =
        artifactUsage_.value_or(ArtifactIndexUsage{});
    return {{"root", root.string()},
            {"index", artifactIndex_ ? artifactIndex_->path().string() : ""},
            {"indexSchema", ArtifactIndex::kSchemaVersion},
            {"records", usage.records},
            {"readyRecords", usage.readyRecords},
            {"referencedBlobs", usage.referencedBlobs},
            {"referencedBlobBytes", usage.referencedBlobBytes},
            {"blobFiles", usage.cacheBlobFiles},
            {"blobBytes", usage.cacheBlobBytes},
            {"files", usage.cacheBlobFiles},
            {"bytes", usage.cacheBlobBytes},
            {"unreferencedBlobFiles", usage.unreferencedBlobFiles},
            {"unreferencedBlobBytes", usage.unreferencedBlobBytes},
            {"mode", assetImportModeName(config_.assetImportMode)}};
}

ControlJson Application::runtimeShaderList() {
    ControlJson shaders = ControlJson::array();
    for (const auto &variant : shaderVariants_)
        shaders.push_back(variant.displayName);
    return {{"shaders", std::move(shaders)}};
}

ControlJson Application::runtimeShaderCurrent() {
    return {{"name", shaderVariants_.empty()
                         ? ControlJson(nullptr)
                         : ControlJson(currentShaderVariant().displayName)}};
}

ControlJson Application::runtimeShaderSet(const std::string &name) {
    const int index = findShaderVariantIndexByName(name);
    if (index < 0) {
        ControlJson candidates = ControlJson::array();
        for (const auto &variant : shaderVariants_)
            candidates.push_back(variant.displayName);
        throw RuntimeCommandError(
            "shader_not_found",
            "Unknown shader '" + name + "'. Available shaders: " +
                candidates.dump());
    }
    setShaderVariant(index);
    return {{"shader", shaderVariants_[index].displayName}};
}

ControlJson Application::runtimeLastLoadStats() {
    if (!lastSceneLoadStats_)
        throw RuntimeCommandError("no_load_stats",
                                  "No scene load statistics exist.");
    return sceneLoadStatsToJson(*lastSceneLoadStats_);
}

ControlJson Application::runtimeQuit() {
    return {{"quitting", true}};
}

void Application::applySceneCameraDefaults() {
    constexpr float kFallbackNear = 0.05f;
    constexpr float kFallbackFar = 200.0f;
    constexpr float kMinSceneFar = 50.0f;
    constexpr float kRadiusFarScale = 4.0f;
    constexpr float kMinAutoNear = 0.01f;
    constexpr float kMaxAutoNear = 0.05f;

    if (!currentScene_ || !currentScene_->bounds().valid) {
        camera_.setClipPlanes(kFallbackNear, kFallbackFar);
        return;
    }

    const Bounds &bounds = currentScene_->bounds();
    if (!currentScene_->initialCamera) {
        const float distance = std::max(bounds.radius * 2.2f, 1.0f);
        const glm::vec3 direction =
            glm::normalize(glm::vec3(1.0f, 1.0f, 0.65f));
        camera_.setPosition(bounds.center + direction * distance);
        camera_.lookAt(bounds.center);
    }
    const float distanceToCenter =
        glm::length(camera_.position() - bounds.center);
    const float farPlane =
        std::max(kMinSceneFar,
                 distanceToCenter + bounds.radius * kRadiusFarScale);
    const float nearPlane =
        std::max(kMinAutoNear, std::min(kMaxAutoNear, farPlane / 10000.0f));
    camera_.setClipPlanes(nearPlane, farPlane);
}

void Application::updateInputMode() {
    auto &io = ImGui::GetIO();

    if (mode_ == InputMode::UI) {
        const bool pressed = input_->isMousePressed(MouseButton::Right);
        const bool overUI = io.WantCaptureMouse || ImGui::IsAnyItemActive();
        if (pressed && !overUI) {
            savedCursor_ = input_->cursorPos();
            input_->setCursorCaptured(true);
            io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
            mode_ = InputMode::CameraDrag;
        }
    } else { // CameraDrag
        if (input_->isMouseReleased(MouseButton::Right)) {
            input_->setCursorCaptured(false);
            input_->setCursorPos(savedCursor_);
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
            mode_ = InputMode::UI;
        }
    }
}

void Application::processCameraInput(float dt) {
    glm::vec3 move{0.0f};
    if (input_->isKeyDown(Key::W))
        move.z += config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::S))
        move.z -= config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::A))
        move.x -= config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::D))
        move.x += config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::Q))
        move.y -= config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::E))
        move.y += config_.moveSpeed * dt;
    camera_.translate(move);

    const auto d = input_->mouseDelta();
    camera_.rotate(-d.x * config_.mouseSensitivity,
                   -d.y * config_.mouseSensitivity);
}

void Application::updateUniforms(uint32_t frameIndex) {
    GlobalUBO ubo{};
    ubo.view = camera_.viewMatrix();
    ubo.proj = camera_.projectionMatrix();
    ubo.cameraPosWS = glm::vec4(camera_.position(), 1.0f);
    ubo.ambientColorIntensity =
        glm::vec4(glm::max(ambientColor_, glm::vec3(0.0f)),
                  std::max(ambientIntensity_, 0.0f));

    uint32_t directionalCount = 0;
    uint32_t punctualCount = 0;
    uint32_t ignoredCount = 0;

    const auto uploadLight = [&](const SceneLight &light) {
        switch (light.type) {
        case LightType::Directional:
            if (directionalCount < kMaxDirectionalLights) {
                ubo.directionalLights[directionalCount++] =
                    makeGpuLight(light);
            } else {
                ++ignoredCount;
            }
            break;
        case LightType::Point:
        case LightType::Spot:
            if (punctualCount < kMaxPunctualLights) {
                ubo.punctualLights[punctualCount++] = makeGpuLight(light);
            } else {
                ++ignoredCount;
            }
            break;
        }
    };

    const auto *sceneLights =
        currentScene_ ? &currentScene_->lights() : nullptr;
    if (sceneLights && !sceneLights->empty()) {
        for (const auto &light : *sceneLights)
            uploadLight(light);
    } else {
        uploadLight(makeDefaultSun(defaultSunDirection_, defaultSunColor_,
                                   defaultSunIntensity_));
    }

    ubo.lightCounts =
        glm::vec4(static_cast<float>(directionalCount),
                  static_cast<float>(punctualCount), 0.0f, 0.0f);
    lastUploadedDirectionalLights_ = directionalCount;
    lastUploadedPunctualLights_ = punctualCount;
    if (ignoredCount != lastIgnoredLights_) {
        if (ignoredCount > 0) {
            VKR_LOG_WARN("Lighting",
                         "Ignored {} scene lights beyond GPU light limits.",
                         ignoredCount);
        }
        lastIgnoredLights_ = ignoredCount;
    }
    std::memcpy(renderer_->mappedUniformBuffer(frameIndex), &ubo, sizeof(ubo));
}

void Application::refreshSceneRegistry(const std::string &selectSceneId) {
    std::string currentId;
    std::string selectedId = selectSceneId;
    if (currentSceneIndex_ >= 0 &&
        currentSceneIndex_ < static_cast<int>(sceneRegistry_.size()))
        currentId = sceneRegistry_[currentSceneIndex_].id;
    if (selectedId.empty() && sceneAssetOperations_ &&
        sceneAssetOperations_->selectedSceneIndex >= 0 &&
        sceneAssetOperations_->selectedSceneIndex <
            static_cast<int>(sceneRegistry_.size())) {
        selectedId =
            sceneRegistry_[sceneAssetOperations_->selectedSceneIndex].id;
    }

    catalog_ = SceneCatalog::load(projectContext_.catalogPath,
                                  projectContext_.projectRoot);
    sceneRegistry_ = buildSceneRegistry(catalog_, projectContext_, config_);
    reloadArtifactIndex();
    currentSceneIndex_ = -1;
    for (int i = 0; i < static_cast<int>(sceneRegistry_.size()); ++i) {
        if (sceneRegistry_[i].id == currentId)
            currentSceneIndex_ = i;
    }
    sceneAssetOperations_->selectedSceneIndex = -1;
    if (!selectedId.empty()) {
        for (int i = 0; i < static_cast<int>(sceneRegistry_.size()); ++i) {
            if (sceneRegistry_[i].id == selectedId) {
                sceneAssetOperations_->selectedSceneIndex = i;
                break;
            }
        }
    }
    refreshAllArtifactStatuses();
}

void Application::updateSceneImport() {
    SceneImportUiState &ui = *sceneImportUi_;
    if (ui.preflightFuture.valid() &&
        ui.preflightFuture.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
        try {
            ui.preflight = ui.preflightFuture.get();
            std::snprintf(ui.displayName.data(), ui.displayName.size(), "%s",
                          ui.preflight->suggestedDisplayName.c_str());
            std::snprintf(ui.sceneId.data(), ui.sceneId.size(), "%s",
                          ui.preflight->suggestedSceneId.c_str());
            ui.profileIds.clear();
            for (const auto &profile : catalog_.importProfiles)
                ui.profileIds.push_back(profile.first);
            std::sort(ui.profileIds.begin(), ui.profileIds.end());
            const auto selected = std::find(
                ui.profileIds.begin(), ui.profileIds.end(),
                catalog_.defaultImportProfile);
            ui.profileIndex =
                selected == ui.profileIds.end()
                    ? 0
                    : static_cast<int>(selected - ui.profileIds.begin());
            ui.referenceExisting =
                pathIsWithin(projectContext_.projectRoot,
                             ui.preflight->sourcePath);
            ui.requestOpenModal = true;
            ui.status.clear();
            ui.error.clear();
        } catch (const std::exception &error) {
            ui.error = error.what();
            ui.status.clear();
        }
    }

    if (ui.importFuture.valid() &&
        ui.importFuture.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
        try {
            const SceneImportResult result = ui.importFuture.get();
            const bool loadAfter = ui.loadAfterActiveImport;
            ui.worker.reset();
            ui.preflight.reset();
            ui.status = "Imported " + result.scene.displayName;
            ui.error.clear();
            refreshSceneRegistry(result.scene.id);
            const ImportProfile &profile =
                catalog_.profile(result.scene.importProfile);
            sceneLoadContext_.maxTextureSize = profile.textureLimit;
            for (int i = 0; i < static_cast<int>(sceneRegistry_.size()); ++i) {
                if (sceneRegistry_[i].id == result.scene.id) {
                    requestSceneOperation(
                        i, false, loadAfter,
                        ImportReason::SceneRegistration, false);
                    ui.status = "Registered " + result.scene.displayName +
                                "; derived texture import queued";
                    break;
                }
            }
        } catch (const std::exception &error) {
            ui.worker.reset();
            ui.error = error.what();
            ui.status.clear();
        }
    }
}

void Application::drawScenePanel() {
    SceneImportUiState &ui = *sceneImportUi_;
    updateSceneImport();

    ImGui::Begin("Scenes");
    const bool busy = ui.preflightFuture.valid() || ui.importFuture.valid();
    const bool canImportSource =
        config_.assetImportMode == AssetImportMode::OnDemand &&
        projectContext_.catalogWritable;
    if (config_.assetImportMode != AssetImportMode::CookedOnly) {
        ImGui::BeginDisabled(busy || !canImportSource);
        if (ImGui::Button("Import Scene...")) {
            try {
                const auto selected =
                    openGltfFileDialog(window_->nativeHandle());
                if (selected) {
                    ui.error.clear();
                    ui.status = "Inspecting scene dependencies...";
                    ui.preflightFuture = std::async(
                        std::launch::async, [path = *selected] {
                            return SceneImportService::preflight(path);
                        });
                }
            } catch (const std::exception &error) {
                ui.error = error.what();
            }
        }
        ImGui::EndDisabled();
        if (!canImportSource &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                config_.assetImportMode != AssetImportMode::OnDemand
                    ? "Scene registration is disabled outside OnDemand mode."
                    : "The source project Catalog is read-only.");
        }
        ImGui::SameLine();
    }

    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Refresh")) {
        try {
            refreshSceneRegistry();
            ui.status = "Catalog refreshed";
            ui.error.clear();
        } catch (const std::exception &error) {
            ui.error = error.what();
        }
    }
    ImGui::EndDisabled();

    if (!ui.status.empty())
        ImGui::TextWrapped("%s", ui.status.c_str());
    if (!ui.error.empty())
        ImGui::TextWrapped("Error: %s", ui.error.c_str());

    if (ui.worker) {
        const uint64_t total = ui.worker->totalBytes.load();
        const uint64_t completed = ui.worker->completedBytes.load();
        const float fraction =
            total == 0 ? 0.0f
                       : static_cast<float>(completed) /
                             static_cast<float>(total);
        ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f));
        std::string current;
        {
            std::lock_guard<std::mutex> lock(ui.worker->mutex);
            current = ui.worker->currentFile;
        }
        if (!current.empty())
            ImGui::Text("Copying: %s", current.c_str());
        if (ImGui::Button("Cancel Import"))
            ui.worker->cancel = true;
    }

    ImGui::Separator();
    ImGui::InputTextWithHint("##SceneSearch", "Search scenes...",
                             sceneAssetOperations_->search.data(),
                             sceneAssetOperations_->search.size());
    const std::string search = sceneAssetOperations_->search.data();
    auto containsIgnoreCase = [](const std::string &text,
                                 const std::string &query) {
        if (query.empty())
            return true;
        std::string foldedText = text;
        std::string foldedQuery = query;
        std::transform(foldedText.begin(), foldedText.end(),
                       foldedText.begin(), [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        std::transform(foldedQuery.begin(), foldedQuery.end(),
                       foldedQuery.begin(), [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        return foldedText.find(foldedQuery) != std::string::npos;
    };
    for (int i = 0; i < static_cast<int>(sceneRegistry_.size()); ++i) {
        const SceneEntry &entry = sceneRegistry_[i];
        if (!containsIgnoreCase(entry.name, search) &&
            !containsIgnoreCase(entry.id, search))
            continue;
        const bool selected =
            (i == sceneAssetOperations_->selectedSceneIndex);
        const std::string profileId = profileIdForTextureLimit(entry);
        const auto status = sceneAssetOperations_->statuses.find(
            artifactStatusKey(entry.id, profileId));
        const char *state = status == sceneAssetOperations_->statuses.end()
                                ? "Unknown"
                                : artifactStateName(status->second.state);
        const std::string label = entry.name + "  [" + state + "]" +
                                  (entry.available ? std::string{}
                                                   : " (Unavailable)");
        ImGui::BeginDisabled(!entry.available);
        if (ImGui::Selectable(label.c_str(), selected)) {
            sceneAssetOperations_->selectedSceneIndex = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                try {
                    requestSceneOperation(i);
                } catch (const std::exception &error) {
                    sceneAssetOperations_->error = error.what();
                }
            }
        }
        ImGui::EndDisabled();
        if (!entry.available &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", entry.unavailableReason.c_str());
    }

    const int selectedIndex = sceneAssetOperations_->selectedSceneIndex;
    if (selectedIndex >= 0 &&
        selectedIndex < static_cast<int>(sceneRegistry_.size())) {
        const SceneEntry &entry = sceneRegistry_[selectedIndex];
        const std::string profileId = profileIdForTextureLimit(entry);
        ImGui::Separator();
        ImGui::Text("Selected: %s", entry.name.c_str());
        ImGui::Text("ID: %s", entry.id.c_str());
        ImGui::Text("Profile: %s", profileId.c_str());
        if (!entry.sourcePath.empty())
            ImGui::TextWrapped("Source: %s", entry.sourcePath.c_str());
        ImGui::BeginDisabled(!entry.available);
        if (ImGui::Button("Load")) {
            try {
                requestSceneOperation(selectedIndex);
            } catch (const std::exception &error) {
                sceneAssetOperations_->error = error.what();
            }
        }
        ImGui::EndDisabled();
        if (!entry.builtin &&
            config_.assetImportMode == AssetImportMode::OnDemand) {
            ImGui::SameLine();
            if (ImGui::Button("Reimport")) {
                try {
                    requestSceneOperation(selectedIndex, false, false,
                                          ImportReason::ManualReimport, true);
                } catch (const std::exception &error) {
                    sceneAssetOperations_->error = error.what();
                }
            }
        }
        if (!entry.builtin &&
            config_.assetImportMode != AssetImportMode::CookedOnly) {
            ImGui::SameLine();
            if (ImGui::Button("Load Source Fallback")) {
                try {
                    requestSceneOperation(selectedIndex, true);
                } catch (const std::exception &error) {
                    sceneAssetOperations_->error = error.what();
                }
            }
        }
        if (projectContext_.catalogWritable &&
            config_.assetImportMode == AssetImportMode::OnDemand) {
            if (ImGui::Button("Save Current Camera")) {
                try {
                    SceneCatalogEditor::saveCamera(
                        projectContext_, entry.id,
                        CameraPose{camera_.position(), camera_.yaw(),
                                   camera_.pitch()});
                    sceneAssetOperations_->status =
                        "Saved camera for " + entry.name;
                    refreshSceneRegistry(entry.id);
                } catch (const std::exception &error) {
                    sceneAssetOperations_->error = error.what();
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(entry.builtin ||
                                 selectedIndex == currentSceneIndex_);
            if (ImGui::Button("Remove From Catalog"))
                ImGui::OpenPopup("Remove Scene");
            ImGui::EndDisabled();
            if (ImGui::BeginPopupModal("Remove Scene", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextWrapped(
                    "Remove '%s' from the Catalog? Source files and derived "
                    "artifacts will not be deleted.",
                    entry.name.c_str());
                if (ImGui::Button("Remove")) {
                    try {
                        SceneCatalogEditor::removeScene(projectContext_,
                                                        entry.id);
                        sceneAssetOperations_->status =
                            "Removed " + entry.name + " from Catalog";
                        refreshSceneRegistry();
                    } catch (const std::exception &error) {
                        sceneAssetOperations_->error = error.what();
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
    }
    if (!sceneAssetOperations_->status.empty())
        ImGui::TextWrapped("%s", sceneAssetOperations_->status.c_str());
    if (!sceneAssetOperations_->error.empty())
        ImGui::TextWrapped("Error: %s",
                           sceneAssetOperations_->error.c_str());

    if (ui.requestOpenModal) {
        ImGui::OpenPopup("Import Scene");
        ui.requestOpenModal = false;
    }
    if (ImGui::BeginPopupModal("Import Scene", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ui.preflight) {
            ImGui::TextWrapped("Source: %s",
                               ui.preflight->sourcePath.string().c_str());
            ImGui::Text("Dependencies: %llu",
                        static_cast<unsigned long long>(
                            ui.preflight->dependencies.size()));
            ImGui::InputText("Name", ui.displayName.data(),
                             ui.displayName.size());
            ImGui::InputText("Scene ID", ui.sceneId.data(),
                             ui.sceneId.size());
            if (!ui.profileIds.empty()) {
                const char *current = ui.profileIds[ui.profileIndex].c_str();
                if (ImGui::BeginCombo("Import Profile", current)) {
                    for (int i = 0;
                         i < static_cast<int>(ui.profileIds.size()); ++i) {
                        const bool selected = i == ui.profileIndex;
                        if (ImGui::Selectable(ui.profileIds[i].c_str(),
                                              selected))
                            ui.profileIndex = i;
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
            const bool projectLocal = pathIsWithin(
                projectContext_.projectRoot, ui.preflight->sourcePath);
            ImGui::BeginDisabled(!projectLocal);
            ImGui::Checkbox("Reference existing project file",
                            &ui.referenceExisting);
            ImGui::EndDisabled();
            if (!projectLocal)
                ui.referenceExisting = false;
            ImGui::Checkbox("Load scene after import", &ui.loadAfterImport);

            const bool valid = ui.displayName[0] != '\0' &&
                               isStableAssetId(ui.sceneId.data()) &&
                               !ui.profileIds.empty();
            ImGui::BeginDisabled(!valid);
            if (ImGui::Button("Import")) {
                SceneImportRequest request;
                request.sourcePath = ui.preflight->sourcePath;
                request.displayName = ui.displayName.data();
                request.sceneId = ui.sceneId.data();
                request.profileId = ui.profileIds[ui.profileIndex];
                request.placement =
                    ui.referenceExisting
                        ? SceneImportPlacement::ReferenceExisting
                        : SceneImportPlacement::CopyIntoProject;
                ui.loadAfterActiveImport = ui.loadAfterImport;
                ui.worker = std::make_shared<SceneImportWorkerState>();
                ui.worker->totalBytes = ui.preflight->totalBytes;
                const auto worker = ui.worker;
                const ProjectContext project = projectContext_;
                ui.importFuture = std::async(
                    std::launch::async,
                    [project, request, worker] {
                        return SceneImportService::importScene(
                            project, request,
                            [worker] { return worker->cancel.load(); },
                            [worker](const SceneImportProgress &progress) {
                                worker->completedBytes = progress.completedBytes;
                                worker->totalBytes = progress.totalBytes;
                                std::lock_guard<std::mutex> lock(worker->mutex);
                                worker->currentFile = progress.currentFile;
                            });
                    });
                ui.status = "Importing source files...";
                ui.error.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ui.preflight.reset();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

void Application::drawAssetsPanel() {
    ImGui::Begin("Assets");
    ImGui::Text("Project: %s", catalog_.projectId.c_str());
    ImGui::TextWrapped("Catalog: %s",
                       projectContext_.catalogPath.string().c_str());
    ImGui::TextWrapped("Cache: %s",
                       sceneLoadContext_.derivedTextureCachePath.c_str());
    ImGui::Text("Mode: %s", assetImportModeName(config_.assetImportMode));
    if (artifactUsage_) {
        ImGui::Text("Index: %llu records (%llu ready)",
                    static_cast<unsigned long long>(artifactUsage_->records),
                    static_cast<unsigned long long>(
                        artifactUsage_->readyRecords));
        ImGui::Text("Cache Blobs: %llu (%.2f MiB)",
                    static_cast<unsigned long long>(
                        artifactUsage_->cacheBlobFiles),
                    bytesToMiB(artifactUsage_->cacheBlobBytes));
        ImGui::Text("Unreferenced: %llu (%.2f MiB)",
                    static_cast<unsigned long long>(
                        artifactUsage_->unreferencedBlobFiles),
                    bytesToMiB(artifactUsage_->unreferencedBlobBytes));
    }

    const int selected = sceneAssetOperations_->selectedSceneIndex;
    if (selected >= 0 && selected < static_cast<int>(sceneRegistry_.size())) {
        const SceneEntry &entry = sceneRegistry_[selected];
        const std::string profileId = profileIdForTextureLimit(entry);
        const auto found = sceneAssetOperations_->statuses.find(
            artifactStatusKey(entry.id, profileId));
        ImGui::Separator();
        ImGui::Text("Scene: %s", entry.name.c_str());
        ImGui::Text("Profile: %s", profileId.c_str());
        if (found != sceneAssetOperations_->statuses.end()) {
            const ArtifactStatus &status = found->second;
            ImGui::Text("Artifacts: %s", artifactStateName(status.state));
            ImGui::TextWrapped("%s", status.reason.c_str());
            if (status.entryCount > 0) {
                ImGui::Text("Blobs: %llu (%.2f MiB)",
                            static_cast<unsigned long long>(status.entryCount),
                            bytesToMiB(status.blobBytes));
            }
        }
        if (artifactIndex_) {
            const auto record = artifactIndex_->records().find(
                artifactIndexKey(entry.id, profileId));
            if (record != artifactIndex_->records().end() &&
                !record->second.failureCode.empty()) {
                ImGui::Text("Last Failure: %s",
                            record->second.failureCode.c_str());
                ImGui::TextWrapped("%s",
                                   record->second.failureMessage.c_str());
            }
        }
    }

    const auto active = assetImportManager_
                            ? assetImportManager_->activeTask()
                            : std::shared_ptr<AssetImportTask>{};
    ImGui::Separator();
    ImGui::TextUnformatted("Current Import");
    if (active) {
        const uint64_t total = active->totalArtifacts.load();
        const uint64_t completed = active->completedArtifacts.load();
        const float fraction =
            total == 0
                ? 0.0f
                : static_cast<float>(completed) / static_cast<float>(total);
        const double elapsedMs = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() -
                                     active->requestedAt)
                                     .count();
        ImGui::Text("Task: %llu  %s",
                    static_cast<unsigned long long>(active->id),
                    assetImportStateName(active->state.load()));
        ImGui::Text("Scene/Profile: %s / %s", active->sceneId.c_str(),
                    active->profileId.c_str());
        ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f));
        ImGui::Text("Artifacts: %llu/%llu  encoded %llu  reused %llu  failed %llu",
                    static_cast<unsigned long long>(completed),
                    static_cast<unsigned long long>(total),
                    static_cast<unsigned long long>(
                        active->encodedArtifacts.load()),
                    static_cast<unsigned long long>(
                        active->reusedArtifacts.load()),
                    static_cast<unsigned long long>(
                        active->failedArtifacts.load()));
        ImGui::Text("Workers: %u  elapsed: %.1f s", active->workers.load(),
                    elapsedMs / 1000.0);
        ImGui::BeginDisabled(
            isTerminalAssetImportState(active->state.load()));
        if (ImGui::Button("Cancel Asset Import"))
            assetImportManager_->cancel(active->id);
        ImGui::EndDisabled();
        if (std::filesystem::is_regular_file(active->logPath)) {
            ImGui::SameLine();
            if (ImGui::Button("Open Log")) {
                ShellExecuteW(nullptr, L"open", active->logPath.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
    } else {
        ImGui::TextUnformatted("Idle");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Recent Imports");
    const auto history = assetImportManager_
                             ? assetImportManager_->history()
                             : std::vector<std::shared_ptr<AssetImportTask>>{};
    const size_t visible = std::min<size_t>(history.size(), 8);
    for (size_t i = 0; i < visible; ++i) {
        const auto &task = history[i];
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::TreeNode("Import", "%s / %s  [%s]##%llu",
                            task->sceneId.c_str(), task->profileId.c_str(),
                            assetImportStateName(task->state.load()),
                            static_cast<unsigned long long>(task->id))) {
            ImGui::Text("Encoded: %llu  Reused: %llu  Failed: %llu",
                        static_cast<unsigned long long>(
                            task->encodedArtifacts.load()),
                        static_cast<unsigned long long>(
                            task->reusedArtifacts.load()),
                        static_cast<unsigned long long>(
                            task->failedArtifacts.load()));
            {
                std::lock_guard<std::mutex> lock(task->mutex);
                if (!task->error.empty())
                    ImGui::TextWrapped("Error: %s", task->error.c_str());
            }
            if (std::filesystem::is_regular_file(task->logPath) &&
                ImGui::Button("Open Log")) {
                ShellExecuteW(nullptr, L"open", task->logPath.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::End();
}

void Application::drawGui() {
    ImGui::Begin("Renderer");
    if (!shaderVariants_.empty()) {
        const char *current =
            shaderVariants_[currentShaderVariantIndex_].displayName;
        if (ImGui::BeginCombo("Shader", current)) {
            for (int i = 0; i < static_cast<int>(shaderVariants_.size());
                 ++i) {
                const bool selected = (i == currentShaderVariantIndex_);
                if (ImGui::Selectable(shaderVariants_[i].displayName,
                                      selected))
                    setShaderVariant(i);
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    constexpr uint32_t textureLimits[] = {0, 2048, 1024, 512};
    ImGui::BeginDisabled(config_.assetImportMode ==
                         AssetImportMode::CookedOnly);
    if (ImGui::BeginCombo("Texture Limit",
                          textureLimitLabel(sceneLoadContext_.maxTextureSize))) {
        for (uint32_t limit : textureLimits) {
            const bool selected = sceneLoadContext_.maxTextureSize == limit;
            if (ImGui::Selectable(textureLimitLabel(limit), selected)) {
                if (!selected)
                    setTextureLimit(limit);
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::End();

    drawScenePanel();
    drawAssetsPanel();

    ImGui::Begin("Loading");
    if (!latestSceneLoadTask_) {
        ImGui::Text("No active load task");
    } else {
        const auto task = latestSceneLoadTask_;
        const SceneLoadState state = task->state.load();
        ImGui::Text("Task: %llu",
                    static_cast<unsigned long long>(task->id));
        ImGui::Text("Scene: %s", task->sceneName.c_str());
        ImGui::Text("State: %s", sceneLoadStateName(state));
        const uint64_t textureTotal = task->progress.totalTextures.load();
        const uint64_t textureDone =
            task->progress.completedTextures.load();
        const uint64_t meshTotal = task->progress.totalMeshes.load();
        const uint64_t meshDone = task->progress.completedMeshes.load();
        ImGui::Text("Textures: %llu / %llu",
                    static_cast<unsigned long long>(textureDone),
                    static_cast<unsigned long long>(textureTotal));
        ImGui::Text("Meshes: %llu / %llu",
                    static_cast<unsigned long long>(meshDone),
                    static_cast<unsigned long long>(meshTotal));
        ImGui::Text("GPU Textures: %llu / %llu",
                    static_cast<unsigned long long>(
                        task->progress.uploadedTextures.load()),
                    static_cast<unsigned long long>(
                        task->progress.uploadTextureTotal.load()));
        ImGui::Text("GPU Meshes: %llu / %llu",
                    static_cast<unsigned long long>(
                        task->progress.uploadedMeshes.load()),
                    static_cast<unsigned long long>(
                        task->progress.uploadMeshTotal.load()));
        ImGui::Text("Processed: %.2f MiB",
                    bytesToMiB(task->progress.processedBytes.load()));
        if (!isTerminalSceneLoadState(state) &&
            state != SceneLoadState::ReadyToPublish) {
            if (ImGui::Button("Cancel Load"))
                cancelSceneLoad(task->id);
        }
        std::lock_guard<std::mutex> lock(task->mutex);
        if (!task->error.empty())
            ImGui::TextWrapped("Error: %s", task->error.c_str());
    }
    ImGui::End();

    ImGui::Begin("Lighting");
    ImGui::ColorEdit3("Ambient Color", &ambientColor_.x);
    ImGui::DragFloat("Ambient Intensity", &ambientIntensity_, 0.01f, 0.0f,
                     10.0f);
    const size_t sceneLightCount = currentScene_ ? currentScene_->lights().size()
                                                 : 0;
    ImGui::Text("Scene lights: %zu", sceneLightCount);
    ImGui::Text("Uploaded: %u directional, %u punctual",
                lastUploadedDirectionalLights_, lastUploadedPunctualLights_);
    if (lastIgnoredLights_ > 0)
        ImGui::Text("Ignored: %u", lastIgnoredLights_);
    if (sceneLightCount == 0) {
        ImGui::Separator();
        ImGui::DragFloat3("Sun Direction", &defaultSunDirection_.x, 0.01f,
                          -1.0f, 1.0f);
        ImGui::ColorEdit3("Sun Color", &defaultSunColor_.x);
        ImGui::DragFloat("Sun Intensity", &defaultSunIntensity_, 0.05f, 0.0f,
                         20.0f);
    }
    ImGui::End();

    ImGui::Begin("Materials");
    if (!currentScene_) {
        ImGui::Text("Materials: 0");
    } else {
        const auto &materials = currentScene_->materials();
        ImGui::Text("Materials: %zu", materials.size());
        for (size_t i = 0; i < materials.size(); ++i) {
            const auto &material = materials[i];
            if (!material) {
                ImGui::Text("#%zu <null>", i);
                continue;
            }

            const auto &params = material->params();
            const std::string label =
                "#" + std::to_string(i) + " " +
                (params.debugName.empty() ? "<unnamed>" : params.debugName);
            if (!ImGui::TreeNode(label.c_str()))
                continue;

            ImGui::Text("Alpha Mode: %s", alphaModeName(params.alphaMode));
            ImGui::Text("Alpha Cutoff: %.3f", params.alphaCutoff);
            ImGui::Text("Double Sided: %s",
                        params.doubleSided ? "true" : "false");
            ImGui::Text("Transmission: %.3f", params.transmissionFactor);
            ImGui::Text("Emissive Strength: %.3f",
                        params.emissiveStrength);
            ImGui::Text("Metallic Factor: %.3f", params.metallicFactor);
            ImGui::Text("Roughness Factor: %.3f", params.roughnessFactor);
            ImGui::Text("Normal Scale: %.3f", params.normalScale);
            ImGui::Text("Occlusion Strength: %.3f",
                        params.occlusionStrength);
            ImGui::Text("Occlusion UV: %u", params.occlusionTexCoord);
            ImGui::Text("Base Color Factor: %.3f %.3f %.3f %.3f",
                        params.baseColorFactor.r, params.baseColorFactor.g,
                        params.baseColorFactor.b, params.baseColorFactor.a);
            ImGui::Text("Emissive Factor: %.3f %.3f %.3f",
                        params.emissiveFactor.r, params.emissiveFactor.g,
                        params.emissiveFactor.b);
            ImGui::Separator();
            ImGui::Text("Render Queue: %s",
                        isTransparentMaterial(params) ? "Transparent"
                                                      : "Opaque");
            ImGui::Text("Cull: %s", params.doubleSided ? "None" : "Back");
            ImGui::Separator();
            const auto &textures = material->textures();
            for (size_t slotIndex = 0; slotIndex < kMaterialTextureSlotCount;
                 ++slotIndex) {
                const auto slot =
                    static_cast<MaterialTextureSlot>(slotIndex);
                ImGui::Text("%s: %s", slotName(slot),
                            textures[slotIndex] ? "Bound" : "Missing");
            }
            ImGui::TreePop();
        }
    }
    ImGui::End();

    ImGui::Begin("Camera");
    const auto cameraPos = camera_.position();
    ImGui::Text("Position: (%.2f, %.2f, %.2f)", cameraPos.x, cameraPos.y,
                cameraPos.z);
    float nearPlane = camera_.nearPlane();
    float farPlane = camera_.farPlane();
    bool  clipChanged = false;
    clipChanged |= ImGui::DragFloat("Near Plane", &nearPlane, 0.001f, 0.001f,
                                    100.0f, "%.4f");
    clipChanged |= ImGui::DragFloat("Far Plane", &farPlane, 0.1f, 1.0f,
                                    100000.0f, "%.2f");
    if (clipChanged)
        camera_.setClipPlanes(nearPlane, farPlane);

    if (currentScene_ && currentScene_->bounds().valid) {
        const Bounds &bounds = currentScene_->bounds();
        ImGui::Separator();
        ImGui::Text("Bounds Center: (%.2f, %.2f, %.2f)", bounds.center.x,
                    bounds.center.y, bounds.center.z);
        ImGui::Text("Bounds Radius: %.2f", bounds.radius);
    } else {
        ImGui::Separator();
        ImGui::Text("Bounds: unavailable");
    }
    ImGui::End();

    ImGui::Begin("Stats");
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    const auto p = camera_.position();
    ImGui::Text("Camera: (%.2f, %.2f, %.2f)", p.x, p.y, p.z);
    ImGui::Text("Mode:   %s", mode_ == InputMode::UI ? "UI" : "CameraDrag");
    if (currentScene_)
        ImGui::Text("Objects: %zu", currentScene_->objects().size());
    if (lastSceneLoadStats_ &&
        ImGui::CollapsingHeader("Last Scene Load",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        const SceneLoadStats &stats = *lastSceneLoadStats_;
        const ResourceLoadStats &resources = stats.resources;
        const int64_t allocationDelta =
            memoryDelta(stats.allocatorAfter.allocationBytes,
                        stats.allocatorBefore.allocationBytes);
        const int64_t blockDelta =
            memoryDelta(stats.allocatorAfter.blockBytes,
                        stats.allocatorBefore.blockBytes);

        ImGui::Text("Scene: %s", stats.sceneName.c_str());
        ImGui::Text("Status: %s", stats.success ? "Success" : "Failed");
        if (stats.taskId != 0) {
            ImGui::Text("Task: %llu  Generation: %llu",
                        static_cast<unsigned long long>(stats.taskId),
                        static_cast<unsigned long long>(stats.generation));
        }
        ImGui::Text("Texture Limit: %s",
                    textureLimitLabel(stats.maxTextureSize));
        ImGui::Separator();
        ImGui::Text("Total: %.2f ms", stats.totalMs);
        ImGui::Text("Device Idle: %.2f ms (%llu calls)",
                    stats.deviceIdleMs,
                    static_cast<unsigned long long>(
                        stats.deviceWaitIdleCalls));
        ImGui::Text("Teardown: %.2f ms", stats.teardownMs);
        ImGui::Text("Scene Factory: %.2f ms", stats.sceneFactoryMs);
        ImGui::Text("glTF Parse: %.2f ms", stats.gltfParseMs);
        ImGui::Text("Image Read: %.2f ms", stats.textureFileReadMs);
        ImGui::Text("Image Decode: %.2f ms", stats.textureDecodeMs);
        ImGui::Text("Texture Resize: %.2f ms", resources.textureResizeMs);
        ImGui::Text("KTX Read: %.2f ms  Transcode: %.2f ms",
                    resources.derivedTextureReadMs,
                    resources.derivedTextureTranscodeMs);
        ImGui::Text("Texture Upload: %.2f ms", resources.textureUploadMs);
        ImGui::Text("Material Setup: %.2f ms", stats.materialSetupMs);
        ImGui::Text("Mesh CPU: %.2f ms", stats.meshCpuMs);
        ImGui::Text("Mesh Upload: %.2f ms", resources.meshUploadMs);
        ImGui::Text("Batch Submit/Wait: %.2f ms",
                    resources.batchSubmitWaitMs);
        ImGui::Text("Hierarchy: %.2f ms", stats.hierarchyMs);
        ImGui::Text("Worker Queue: %.2f ms", stats.workerQueueWaitMs);
        ImGui::Text("CPU Prepare: %.2f ms", stats.cpuPrepareMs);
        ImGui::Text("GPU Build: %.2f ms", stats.gpuBuildMs);
        ImGui::Text("Max Upload Pump: %.2f ms",
                    resources.maxUploadPumpMs);
        ImGui::Separator();
        ImGui::Text("Textures: %llu decoded, %llu GPU, %llu resized",
                    static_cast<unsigned long long>(
                        resources.textureDecodeCount),
                    static_cast<unsigned long long>(resources.gpuTextureCount),
                    static_cast<unsigned long long>(
                        resources.resizedTextureCount));
        ImGui::Text("Derived cache: %llu/%llu hits, %llu miss, %llu invalid",
                    static_cast<unsigned long long>(
                        resources.derivedTextureHits),
                    static_cast<unsigned long long>(
                        resources.derivedTextureLookups),
                    static_cast<unsigned long long>(
                        resources.derivedTextureMisses),
                    static_cast<unsigned long long>(
                        resources.derivedTextureInvalid));
        ImGui::Text("BC7: %llu  RGBA fallback: %llu  Prebuilt mip: %llu",
                    static_cast<unsigned long long>(resources.bc7TextureCount),
                    static_cast<unsigned long long>(
                        resources.rgbaTranscodeFallbackCount),
                    static_cast<unsigned long long>(
                        resources.prebuiltMipTextureCount));
        ImGui::Text("Meshes: %llu  Vertices: %llu  Indices: %llu",
                    static_cast<unsigned long long>(resources.gpuMeshCount),
                    static_cast<unsigned long long>(resources.vertexCount),
                    static_cast<unsigned long long>(resources.indexCount));
        ImGui::Text("Materials: %llu  Objects: %llu",
                    static_cast<unsigned long long>(stats.materialCount),
                    static_cast<unsigned long long>(stats.objectCount));
        ImGui::Text("Texture bytes: encoded %.2f, decoded %.2f MiB",
                    bytesToMiB(resources.encodedSourceBytes),
                    bytesToMiB(resources.decodedRgbaBytes));
        ImGui::Text("Texture upload: %.2f MiB  GPU estimate: %.2f MiB",
                    bytesToMiB(resources.textureUploadBytes),
                    bytesToMiB(resources.textureGpuBytesEstimated));
        ImGui::Text("Mesh upload: %.2f MiB",
                    bytesToMiB(resources.vertexUploadBytes +
                               resources.indexUploadBytes));
        ImGui::Text("Legacy submits: %llu  Queue waits: %llu",
                    static_cast<unsigned long long>(
                        resources.singleTimeSubmits),
                    static_cast<unsigned long long>(
                        resources.queueWaitIdleCalls));
        ImGui::Text("Batch submits: %llu  Fence waits: %llu",
                    static_cast<unsigned long long>(resources.batchSubmits),
                    static_cast<unsigned long long>(
                        resources.fenceWaitCalls));
        ImGui::Text("Completed batches: %llu  Fence polls: %llu",
                    static_cast<unsigned long long>(
                        resources.completedBatchSubmits),
                    static_cast<unsigned long long>(
                        resources.fencePollCalls));
        ImGui::Text("Peak in-flight batches: %llu",
                    static_cast<unsigned long long>(
                        resources.peakInFlightBatches));
        ImGui::Text("Peak staging: %.2f MiB",
                    bytesToMiB(resources.peakStagingBytes));
        ImGui::Text("Prepared CPU: %.2f MiB",
                    bytesToMiB(stats.preparedCpuBytes));
        ImGui::Separator();
        ImGui::Text("VMA allocations: %llu -> %llu",
                    static_cast<unsigned long long>(
                        stats.allocatorBefore.allocationCount),
                    static_cast<unsigned long long>(
                        stats.allocatorAfter.allocationCount));
        ImGui::Text("VMA allocation bytes: %.2f -> %.2f MiB (%+.2f)",
                    bytesToMiB(stats.allocatorBefore.allocationBytes),
                    bytesToMiB(stats.allocatorAfter.allocationBytes),
                    signedBytesToMiB(allocationDelta));
        ImGui::Text("VMA block bytes: %.2f -> %.2f MiB (%+.2f)",
                    bytesToMiB(stats.allocatorBefore.blockBytes),
                    bytesToMiB(stats.allocatorAfter.blockBytes),
                    signedBytesToMiB(blockDelta));
    }
    ImGui::Text("(Hold RMB in viewport to fly, WASD/Q/E to move)");
    ImGui::End();
}

void Application::handleSwapChainRecreate() {
    renderer_->recreateSwapChain();
    pipelineCache_->clear();
    frameSync_->onSwapChainRecreated();
    gui_->onSwapChainRecreated(swapChain_->imageCount());
    camera_.setAspect(static_cast<float>(swapChain_->extent().width) /
                      static_cast<float>(swapChain_->extent().height));
}

const ShaderVariant &Application::currentShaderVariant() const {
    if (shaderVariants_.empty())
        return defaultShaderVariant();
    const int index = std::clamp(currentShaderVariantIndex_, 0,
                                 static_cast<int>(shaderVariants_.size()) - 1);
    return shaderVariants_[index];
}

void Application::mainLoop() {
    auto startTime = std::chrono::high_resolution_clock::now();
    auto lastTime = startTime;

    while (!window_->shouldClose()) {
        window_->pollEvents();
        input_->update();

        updateAssetImports();
        processRuntimeCommand();
        if (pendingQuitCommand_ &&
            pendingQuitCommand_->responseDelivered.load()) {
            window_->setShouldClose(true);
            pendingQuitCommand_.reset();
            break;
        }

        // 1. 帧外：场景切�?
        if (pendingSceneIndex_ != -1) {
            switchScene(pendingSceneIndex_);
            pendingSceneIndex_ = -1;
        }
        updateSceneLoading();

        // 2. 时间
        auto  now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        // 3. ImGui 新帧
        gui_->beginFrame();

        // 4. 模式切换 + 输入
        updateInputMode();
        if (mode_ == InputMode::CameraDrag)
            processCameraInput(dt);
        if (input_->isKeyDown(Key::Escape))
            window_->setShouldClose(true);

        // 5. 场景 tick
        float t = std::chrono::duration<float>(now - startTime).count();
        if (currentScene_)
            currentScene_->update(dt, t);

        // 6. UI
        drawGui();

        // 7. 渲染
        auto ctx = frameSync_->beginFrame();
        if (!ctx) {
            if (frameSync_->swapChainNeedsRecreation())
                handleSwapChainRecreate();
            ImGui::EndFrame();
            input_->endFrame();
            continue;
        }

        updateUniforms(ctx->frameIndex);
        renderQueue_.clear();
        if (currentScene_)
            currentScene_->collectRenderCommands(renderQueue_);
        renderQueue_.sortOpaque();
        renderQueue_.sortTransparent(camera_.position());

        renderer_->renderFrame(*ctx, renderQueue_, *pipelineCache_, *gui_,
                               currentShaderVariant());
        frameSync_->endFrame(*ctx);

        if (frameSync_->swapChainNeedsRecreation())
            handleSwapChainRecreate();

        // 8. 帧末：丢弃本帧鼠标增量
        input_->endFrame();
    }

    vkDeviceWaitIdle(device_->logicalDevice());
}

} // namespace vkr
