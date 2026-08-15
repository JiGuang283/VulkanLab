#include "Application.h"
#include "RenderSettingsController.h"
#include "SceneRuntimeCoordinator.h"
#include "SceneWorkflowController.h"

#include <BuildFeatures.h>

#include "assets/DerivedAssetPaths.h"
#include "assets/ContentHash.h"
#include "assets/ArtifactStatus.h"
#include "assets/EnvironmentLoadManager.h"
#include "assets/ModelImportService.h"
#include "assets/SceneCatalogEditor.h"
#include "assets/SceneCatalogStore.h"
#if VKL_ENABLE_RUNTIME_CONTROL
#include "control/NamedPipeServerWin32.h"
#include "control/RuntimeCommand.h"
#include "control/RuntimeControlProtocol.h"
#endif
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/GpuDebugUtils.h"
#include "core/Log.h"
#include "core/SwapChain.h"
#include "core/VulkanContext.h"
#include "diagnostics/BuildInfo.h"
#include "diagnostics/CaptureService.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/SceneLoadStats.h"
#include "diagnostics/TracyProfiler.h"
#if VKL_ENABLE_EDITOR_UI
#include "editor/EditorController.h"
#endif
#include "render/GuiSystem.h"
#include "render/DirectionalShadow.h"
#include "render/PunctualShadow.h"
#include "render/MaterialInstance.h"
#include "render/MaterialSystem.h"
#include "render/MaterialTextureSlot.h"
#include "render/PipelineCache.h"
#include "render/RenderView.h"
#include "render/Renderer.h"
#include "render/TemporalAA.h"
#include "render/RayTracingScene.h"
#include "render/pass/DdgiPass.h"
#include "scene/AssetRepository.h"
#include "scene/EnvironmentAssetRepository.h"
#include "scene/ModelAsset.h"
#include "scene/ModelInstance.h"
#include "scene/ModelSourceResolver.h"
#include "scene/SceneEntry.h"
#include "scene/SceneLight.h"
#include "scene/SceneLoadManager.h"
#include "scene/SceneLoadTask.h"
#include "scene/SceneRegistryBuilder.h"
#include "scene/RuntimeWorld.h"
#include "scene_data/PrimitiveModelDefinitions.h"
#include "window/InputManager.h"
#include "window/Window.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace vkr {

namespace {

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

#if VKL_ENABLE_RUNTIME_CONTROL
ControlJson materialBindingStatusToJson(
    const MaterialBindingStatus &status) {
    return {
        {"requested", materialBindingModeName(status.requested)},
        {"active", materialBindingModeName(status.active)},
        {"deviceSupported", status.deviceSupported},
        {"shaderManifestSupported", status.shaderManifestSupported},
        {"textureCapacity", status.textureCapacity},
        {"materialCapacity", status.materialCapacity},
        {"fallbackReason", status.fallbackReason.empty()
                               ? ControlJson(nullptr)
                               : ControlJson(status.fallbackReason)},
        {"activeTextures", status.activeTextures},
        {"activeMaterials", status.activeMaterials},
        {"retiringTextures", status.retiringTextures},
        {"retiringMaterials", status.retiringMaterials},
        {"textureHighWaterMark", status.textureHighWaterMark},
        {"materialHighWaterMark", status.materialHighWaterMark},
        {"descriptorWrites", status.descriptorWrites},
        {"textureSlotReuses", status.textureSlotReuses},
        {"materialSlotReuses", status.materialSlotReuses},
        {"textureCapacityFailures", status.textureCapacityFailures},
        {"materialCapacityFailures", status.materialCapacityFailures}};
}

ControlJson assetImportTaskToJson(const AssetTaskSnapshot &task) {
    ControlJson result = {
        {"taskId", task.id},
        {"assetKind", task.kind},
        {"assetId", task.assetId},
        {"sceneId", task.assetId},
        {"profileId", task.profileId},
        {"source", task.sourcePath.u8string()},
        {"state", task.state},
        {"phase", task.phase},
        {"terminal", task.terminal},
        {"progress",
         {{"completed", task.completed},
          {"total", task.total},
          {"encoded", task.encoded},
          {"reused", task.reused},
          {"failed", task.failed},
          {"workers", task.workers},
          {"activeImage", task.activeImage},
          {"estimatedMemoryBytes", task.estimatedMemoryBytes}}},
        {"logPath", task.logPath.u8string()},
        {"exitCode", task.processExitCode}};
    if (!task.error.empty())
        result["error"] = task.error;
    if (!task.manifestPath.empty())
        result["manifest"] = task.manifestPath.u8string();
    if (task.kind == "SceneValidation") {
        result["validation"] = {
            {"state", task.validationState},
            {"errors", task.validationErrors},
            {"warnings", task.validationWarnings},
            {"reportKey", task.validationReportKey},
            {"inputFingerprint", task.validationInputFingerprint},
            {"failureReason", task.validationFailureReason}};
    }
    return result;
}

ControlJson validationQueryToJson(const AssetValidationQuery &query,
                                  size_t issueLimit) {
    const auto bounded = [](const std::string &value, size_t limit) {
        return value.size() <= limit ? value : value.substr(0, limit);
    };
    ControlJson result = {
        {"state", assetValidationStateName(query.state)},
        {"reason", query.reason},
        {"reportPath", query.reportPath.u8string()}};
    if (!query.report)
        return result;

    const AssetValidationReport &report = *query.report;
    result["validator"] = {{"name", report.validatorName},
                           {"version", report.validatorVersion}};
    result["reportKey"] = report.reportKey;
    result["inputFingerprint"] = report.inputFingerprint;
    result["counts"] = {{"errors", report.errorCount},
                         {"warnings", report.warningCount},
                         {"infos", report.infoCount},
                         {"hints", report.hintCount}};
    result["truncated"] = report.truncated;
    result["failureReason"] = report.failureReason;

    ControlJson extensions = ControlJson::array();
    for (const GltfExtensionDiagnostic &extension : report.extensions) {
        extensions.push_back(
            {{"name", extension.name},
             {"support", gltfExtensionSupportName(extension.support)},
             {"required", extension.required},
             {"note", extension.note}});
    }
    result["extensions"] = std::move(extensions);

    ControlJson issues = ControlJson::array();
    const size_t count = std::min(issueLimit, report.issues.size());
    for (size_t i = 0; i < count; ++i) {
        const AssetValidationIssue &issue = report.issues[i];
        issues.push_back({{"code", bounded(issue.code, 128)},
                          {"message", bounded(issue.message, 1024)},
                          {"pointer", bounded(issue.pointer, 512)},
                          {"severity", issue.severity}});
    }
    result["issues"] = std::move(issues);
    result["issueCount"] = report.issues.size();
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
        {"modelGeneration", stats.modelGeneration},
        {"finalState", stats.finalState},
        {"textureLimit", stats.maxTextureSize},
        {"success", stats.success},
        {"repositoryHit", stats.repositoryHit},
        {"coalescedRequest", stats.coalescedRequest},
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
          {"nativeTextureRead", r.nativeTextureReadMs},
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
          {"primitives", stats.primitiveCount},
          {"gltfLightDefinitions", stats.gltfLightDefinitionCount},
          {"lightInstances", stats.lightInstanceCount},
          {"directionalLights", stats.directionalLightCount},
          {"pointLights", stats.pointLightCount},
          {"spotLights", stats.spotLightCount},
          {"textureDecodes", r.textureDecodeCount},
          {"gpuTextures", r.gpuTextureCount},
          {"resizedTextures", r.resizedTextureCount},
          {"derivedTextureLookups", r.derivedTextureLookups},
          {"derivedTextureHits", r.derivedTextureHits},
          {"derivedTextureMisses", r.derivedTextureMisses},
          {"derivedTextureInvalid", r.derivedTextureInvalid},
          {"nativeBc7CacheHits", r.nativeBc7CacheHits},
          {"basisUastcCacheHits", r.basisUastcCacheHits},
          {"basisTranscodeCount", r.basisTranscodeCount},
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
          {"nativeTextureRead", r.nativeTextureReadBytes},
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
         {{"batchSubmits", r.batchSubmits},
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
        {"kind", sceneLoadKindName(task->kind)},
        {"phase", sceneLoadPhaseName(task->phase.load())},
        {"scene", task->sceneName},
        {"sceneIndex", task->sceneIndex},
        {"modelId", task->modelId},
        {"profileId", task->profileId},
        {"modelGeneration", task->modelGeneration},
        {"repositoryHit", task->repositoryHit},
        {"coalescedRequest", task->coalescedRequest},
        {"uniqueModels", task->uniqueModelCount},
        {"readyModels", task->readyModelCount},
        {"failedModelId",
         task->failedModelId.empty() ? ControlJson(nullptr)
                                     : ControlJson(task->failedModelId)},
        {"environmentId",
         task->targetEnvironmentId.empty()
             ? ControlJson(nullptr)
             : ControlJson(task->targetEnvironmentId)},
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

ControlJson environmentLoadTaskToJson(
    const std::shared_ptr<EnvironmentLoadTask> &task) {
    if (!task)
        return nullptr;
    const EnvironmentLoadState state = task->state.load();
    ControlJson result = {
        {"taskId", task->id},
        {"generation", task->generation},
        {"environmentId", task->environmentId},
        {"environment", task->displayName},
        {"profileId", task->profileId},
        {"state", environmentLoadStateName(state)},
        {"phase",
         state == EnvironmentLoadState::PreparingCpu
             ? "preparing"
             : (state == EnvironmentLoadState::Uploading ||
                        state == EnvironmentLoadState::WaitingForGpu ||
                        state == EnvironmentLoadState::ReadyToPublish
                    ? "uploading"
                    : (isTerminalEnvironmentLoadState(state) ? "complete"
                                                              : "queued"))},
        {"terminal", isTerminalEnvironmentLoadState(state)},
        {"progress",
         {{"imagesUploaded", task->uploadedImages.load()},
          {"imagesTotal", task->totalImages}}}};
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        if (!task->error.empty())
            result["error"] = task->error;
    }
    return result;
}

ControlJson captureTaskToJson(const CaptureTaskSnapshot &task) {
    ControlJson result = nullptr;
    if (task.state == CaptureTaskState::Completed) {
        result = {
            {"width", task.result.width},
            {"height", task.result.height},
            {"format", describeCaptureFormat(task.result.format).name},
            {"source", captureSourceKindName(task.result.source)},
            {"frameSerial", task.result.frameSerial},
            {"outputPath", task.result.outputPath.u8string()},
            {"sha256", task.result.sha256},
            {"timingsMs",
             {{"recording", task.result.timings.recordingMs},
              {"gpuWait", task.result.timings.gpuWaitMs},
              {"cpuCopy", task.result.timings.cpuCopyMs},
              {"encode", task.result.timings.encodeMs},
              {"total", task.result.timings.totalMs}}}};
    }
    return {
        {"taskId", task.request.taskId},
        {"state", captureTaskStateName(task.state)},
        {"terminal", isTerminalCaptureTaskState(task.state)},
        {"request",
         {{"path", task.request.relativeOutputPath.u8string()},
          {"includeGui", task.request.includeGui}}},
        {"result", std::move(result)},
        {"error", task.result.error.empty()
                      ? ControlJson(nullptr)
                      : ControlJson(task.result.error)}};
}
#endif

} // namespace

namespace {

#if VKL_ENABLE_RUNTIME_CONTROL
ControlJson loadOperationToJson(
    const AssetTaskSnapshot &importTask,
    const std::shared_ptr<SceneLoadTask> &loadTask) {
    ControlJson result = assetImportTaskToJson(importTask);
    if (!loadTask)
        return result;

    const uint64_t operationId = importTask.id;
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
    return {{"assetKind", "model"},
            {"modelId", entry.id},
            {"sceneId", entry.id},
            {"scene", entry.name},
            {"source", entry.sourcePath},
            {"profileId", profileId},
            {"state", artifactStateName(status.state)},
            {"ready", status.ready()},
            {"reason", status.reason},
            {"manifest", status.manifestPath.u8string()},
            {"artifactCount", status.entryCount},
            {"blobBytes", status.blobBytes},
            {"textureEncoder", status.textureEncoder},
            {"payloadKind", status.payloadKind}};
}
#endif

} // namespace

Application::Application(const Config &config, ProjectContext projectContext,
                         SceneCatalog catalog)
    : config_(config), projectContext_(std::move(projectContext)),
      sceneWorkflow_(std::make_unique<SceneWorkflowController>(
          projectContext_, std::move(catalog))),
      catalog_(sceneWorkflow_->catalog()),
      sceneRegistry_(sceneWorkflow_->entries()) {
    if (config_.derivedTextureCachePath.empty())
        config_.derivedTextureCachePath = projectContext_.cacheRoot.u8string();
    renderSettingsController_ = std::make_unique<RenderSettingsController>(
        projectContext_.resolveRuntimePath("shader/manifest.json"));
    const ShaderRegistry &shaderRegistry =
        renderSettingsController_->shaderRegistry();
    VKR_LOG_INFO("ShaderRegistry", "Loaded {} programs and {} variants; default={}",
                 shaderRegistry.programs().size(),
                 shaderRegistry.variants().size(),
                 renderSettingsController_->currentShaderVariant().id);
#if VKL_ENABLE_RUNTIME_CONTROL
    runtimeControlPipeName_ =
        control::makeRuntimeControlEndpoint(
            config_.diagnostics.runtimePipeSuffix)
            .nameUtf8;
#endif
}

Application::~Application() {
#if VKL_ENABLE_RUNTIME_CONTROL
    if (runtimeControlServer_)
        runtimeControlServer_->stop();
#endif
#if VKL_ENABLE_EDITOR_UI
    editorController_.reset();
#endif
    if (sceneWorkflow_)
        sceneWorkflow_->shutdown();
    if (sceneRuntime_)
        sceneRuntime_->shutdown();
    if (device_) {
        vkDeviceWaitIdle(device_->logicalDevice());
        if (frameSync_)
            frameSync_->markAllSubmissionsCompleted();
        if (captureService_ && frameSync_) {
            captureService_->shutdown(
                frameSync_->completedSubmissionSerial());
        }
    }
    sceneRuntime_.reset();
}

void Application::run() {
    profileSetThreadName("Main");
    {
        VKL_PROFILE_ZONE("Application Init");
        init();
    }
#if VKL_ENABLE_RUNTIME_CONTROL
    if (config_.enableRuntimeControl) {
        runtimeCommandQueue_ = std::make_unique<RuntimeCommandQueue>();
        runtimeControlServer_ = std::make_unique<NamedPipeServerWin32>(
            *runtimeCommandQueue_, control::makeRuntimeControlEndpoint(
                                       config_.diagnostics.runtimePipeSuffix));
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
#else
    mainLoop();
#endif
}

void Application::init() {
    window_ = std::make_unique<Window>(
        config_.windowWidth, config_.windowHeight, config_.windowTitle,
        config_.diagnostics.windowResizable());
    input_ = std::make_unique<InputManager>(*window_);

    auto extensions = Window::getRequiredVulkanExtensions();
    VulkanContextOptions contextOptions;
    contextOptions.validationProfile = config_.validationProfile;
    contextOptions.validationAllowed =
        build::kValidation && !projectContext_.cookedPackage;
    contextOptions.debugUtilsRequested =
        build::kGpuDebugUtils && !projectContext_.cookedPackage;
    context_ = std::make_unique<VulkanContext>(
        [this](VkInstance inst) { return window_->createSurface(inst); },
        std::move(extensions), contextOptions);
    device_ = std::make_unique<Device>(*context_,
                                       config_.materialBindingMode);
    descriptorAllocator_ = std::make_unique<DescriptorAllocator>(*device_);
    materialSystem_ = std::make_unique<MaterialSystem>(
        *device_, *descriptorAllocator_, config_.materialBindingMode,
        renderSettingsController_->shaderRegistry().supportsBindlessMaterials());
    VKR_LOG_INFO(
        "Material",
        "Material binding: requested={} active={} textures={} materials={}{}",
        materialBindingModeName(config_.materialBindingMode),
        materialBindingModeName(materialSystem_->activeMode()),
        materialSystem_->status().textureCapacity,
        materialSystem_->status().materialCapacity,
        materialSystem_->status().fallbackReason.empty()
            ? std::string{}
            : " fallback=" + materialSystem_->status().fallbackReason);
    swapChain_ =
        std::make_unique<SwapChain>(*device_, context_->surface(), [this]() {
            return window_->framebufferExtent();
        });
    frameSync_ = std::make_unique<FrameSync>(*device_, *swapChain_);
    renderer_ = std::make_unique<Renderer>(
        *device_, *swapChain_, *frameSync_, *descriptorAllocator_,
        *materialSystem_, renderSettingsController_->shaderRegistry(),
        materialSystem_->activeMode());
    RenderSettingsCallbacks renderSettingsCallbacks;
    renderSettingsCallbacks.reconfigureCacao = [this](CacaoResolution resolution) {
        frameSync_->waitForAllFrames();
        std::string error;
        if (!renderer_->reconfigureCacao(resolution, error)) {
            throw RenderSettingsError(
                "cacao_reconfigure_failed",
                error.empty() ? "Failed to reconfigure FidelityFX CACAO."
                              : error);
        }
    };
    renderSettingsController_->configure(
        renderer_->featureSupport(), std::move(renderSettingsCallbacks));
#if VKL_ENABLE_CAPTURE
    if (!projectContext_.cookedPackage) {
        captureService_ = std::make_unique<CaptureService>(
            *device_, projectContext_.captureRoot);
    }
#endif

    window_->setResizeCallback(
        [this](int, int) { frameSync_->notifyResize(); });

    camera_.setAspect(static_cast<float>(swapChain_->extent().width) /
                      static_cast<float>(swapChain_->extent().height));

    if (sceneRegistry_.empty())
        throw std::runtime_error(
            "No model previews or native scenes are registered in the "
            "scene catalog.");

    int start = std::clamp(config_.defaultSceneIndex, 0,
                           static_cast<int>(sceneRegistry_.size()) - 1);
    if (projectContext_.nativeScenePackage) {
        const auto found = std::find_if(
            sceneRegistry_.begin(), sceneRegistry_.end(),
            [this](const SceneEntry &entry) {
                return entry.isNativeScene() &&
                       entry.id == projectContext_.startupSceneId;
            });
        if (found == sceneRegistry_.end()) {
            throw std::runtime_error(
                "Cooked package startup scene is not registered: " +
                projectContext_.startupSceneId);
        }
        start = static_cast<int>(found - sceneRegistry_.begin());
    }
    pipelineCache_ = std::make_unique<PipelineCache>(*device_);
    sceneLoadContext_.maxTextureSize = config_.gltfMaxTextureSize;
    sceneLoadContext_.derivedTextureCachePath =
        config_.derivedTextureCachePath;
    sceneLoadContext_.projectId = catalog_.projectId;
    sceneLoadContext_.textureTranscodeTarget =
        device_->textureTranscodeTarget();
    if (projectContext_.cookedPackage &&
        projectContext_.requiredTextureEncoder == "bc7" &&
        sceneLoadContext_.textureTranscodeTarget !=
            TextureTranscodeTarget::Bc7) {
        throw std::runtime_error(
            "bc7_required: this cooked package requires native BC7 texture "
            "support");
    }
    sceneLoadContext_.requireDerivedTextures =
        config_.assetImportMode == AssetImportMode::CookedOnly;
    SceneRuntimeCallbacks runtimeCallbacks;
    runtimeCallbacks.publicationBlockReason = [this]()
        -> std::optional<std::string> {
        return hasUnsavedSceneChanges()
                   ? std::optional<std::string>(
                         "Scene changed while another world was loading")
                   : std::nullopt;
    };
    runtimeCallbacks.worldPublished =
        [this](const SceneRuntimePublication &publication) {
            shadowSystem_.reset();
#if VKL_ENABLE_EDITOR_UI
            if (editorController_)
                editorController_->onWorldPublished(publication);
#endif
            if (publication.document &&
                publication.document->document.environment) {
                const SceneEnvironmentDocument &environment =
                    *publication.document->document.environment;
                RenderSettingsPatch patch;
                patch.environmentIntensity = environment.intensity;
                patch.environmentRotationRadians =
                    environment.rotationRadians;
                applyRenderSettings(patch);
            }
        };
    runtimeCallbacks.environmentPublished =
        [this](const EnvironmentAssetKey &key) {
            sceneWorkflow_->recordEnvironmentUse(key.environmentId,
                                                 key.profileId);
        };
    runtimeCallbacks.loadFinalized =
        [this](const std::shared_ptr<SceneLoadTask> &task,
               bool success) {
            if (!success || !task ||
                task->sceneIndex < 0 ||
                task->sceneIndex >=
                    static_cast<int>(sceneRegistry_.size())) {
                return;
            }
            const SceneEntry &entry = sceneRegistry_[task->sceneIndex];
            if (!entry.isModelPreview())
                return;
            const std::string &profileId = task->profileId;
            sceneWorkflow_->recordModelUse(entry.id, profileId);
        };
    sceneRuntime_ = std::make_unique<SceneRuntimeCoordinator>(
        *device_, *descriptorAllocator_, *materialSystem_, *renderer_,
        *frameSync_, camera_, projectContext_, catalog_, sceneRegistry_,
        sceneLoadContext_, std::move(runtimeCallbacks));

    SceneWorkflowCallbacks workflowCallbacks;
    workflowCallbacks.requestSceneLoad =
        [this](int index, bool sourceFallback, bool reloadAsset) {
            return requestSceneLoad(index, sourceFallback, reloadAsset);
        };
    workflowCallbacks.hasUnsavedChanges =
        [this] { return hasUnsavedSceneChanges(); };
    workflowCallbacks.invalidateModel =
        [this](const ModelAssetId &modelId,
               const std::optional<std::string> &profileId) {
            sceneRuntime_->invalidateModel(
                modelId, profileId ? &*profileId : nullptr);
        };
    workflowCallbacks.invalidateEnvironment =
        [this](const std::string &environmentId,
               const std::optional<std::string> &profileId) {
            sceneRuntime_->invalidateEnvironment(
                environmentId, profileId ? &*profileId : nullptr);
        };
    workflowCallbacks.catalogRefreshed =
        [this] { sceneRuntime_->remapCurrentSceneIndex(); };
    workflowCallbacks.environmentArtifactsReady =
        [this](const std::string &environmentId) {
            if (sceneRuntime_->selectedEnvironmentId() != environmentId)
                return;
            try {
                setEnvironment(environmentId);
                sceneWorkflow_->clearEnvironmentUiError();
            } catch (const std::exception &error) {
                sceneWorkflow_->setEnvironmentUiError(error.what());
            }
        };
    workflowCallbacks.environmentWillBeRemoved =
        [this](const std::string &environmentId) {
            if (sceneRuntime_->selectedEnvironmentId() == environmentId)
                setEnvironment("None");
        };
    workflowCallbacks.textureLimitChanged = [this](uint32_t value) {
        sceneLoadContext_.maxTextureSize = value;
        config_.gltfMaxTextureSize = value;
    };
    sceneWorkflow_->initialize(
        SceneWorkflowConfig{
            config_.assetImportMode,
            std::filesystem::u8path(config_.derivedTextureCachePath),
            std::filesystem::u8path(config_.assetToolPath),
            std::filesystem::u8path(config_.gltfValidatorPath),
            config_.assetImportWorkers,
            config_.assetImportMemoryBudgetMiB,
            build::kAssetAuthoring},
        std::move(workflowCallbacks));
    sceneWorkflow_->setTextureLimit(sceneLoadContext_.maxTextureSize);
    if (catalog_.defaultEnvironment &&
        catalog_.findEnvironment(*catalog_.defaultEnvironment)) {
        try {
            setEnvironment(*catalog_.defaultEnvironment);
        } catch (const std::exception &error) {
            VKR_LOG_WARN("Environment",
                         "Could not queue default environment '{}': {}",
                         *catalog_.defaultEnvironment, error.what());
        }
    }
    sceneWorkflow_->selectEntry(start);
    requestSceneOperation(start);

#if VKL_ENABLE_EDITOR_UI
    if (config_.diagnostics.guiVisible) {
        gui_ = std::make_unique<GuiSystem>(
            context_->instance(), *device_, swapChain_->imageFormat(),
            window_->handle(), swapChain_->imageCount(),
            swapChain_->imageCount());
        EditorControllerActions editorActions;
        editorActions.requestSceneOperation =
            [this](int index, bool sourceFallback, bool loadAfter,
                   SceneWorkflowRequestReason reason, bool forceReimport,
                   bool reloadAsset) {
                return requestSceneOperation(index, sourceFallback, loadAfter,
                                             reason, forceReimport,
                                             reloadAsset);
            };
        editorActions.setTextureLimit =
            [this](uint32_t limit) { return setTextureLimit(limit); };
        editorActions.setEnvironment =
            [this](const std::string &id) { return setEnvironment(id); };
        editorController_ = std::make_unique<EditorController>(
            EditorControllerServices{
                config_, projectContext_, catalog_, sceneRegistry_,
                sceneLoadContext_, *window_, *device_, *frameSync_,
                *swapChain_, *renderer_, *gui_, *materialSystem_,
                *sceneWorkflow_, *sceneRuntime_, *renderSettingsController_,
                captureService_.get(), camera_, ambientColor_,
                ambientIntensity_, defaultSunDirection_, defaultSunColor_,
                defaultSunIntensity_, visibilityFrame_, shadowSystem_,
                lastLightStats_,
                [this]() { return mode_ == InputMode::CameraDrag; },
                std::move(editorActions)});
    }
#endif
}

uint64_t Application::reloadCurrentScene() {
    const int index = sceneRuntime_->currentSceneIndex();
    if (index < 0 || index >= static_cast<int>(sceneRegistry_.size()))
        return 0;
    const uint64_t taskId = requestSceneOperation(
        index, false, true, SceneWorkflowRequestReason::SceneLoad, false, true);
    VKR_LOG_INFO("Scene", "Requested reload of {} with glTF texture limit {}",
                 sceneRegistry_[index].name,
                 sceneLoadContext_.maxTextureSize == 0
                     ? std::string("Full")
                     : std::to_string(sceneLoadContext_.maxTextureSize));
    return taskId;
}

void Application::switchScene(int index) {
    if (index < 0 || index >= static_cast<int>(sceneRegistry_.size()))
        return;
    const auto &latest = sceneRuntime_->latestSceneLoadTask();
    if (index == sceneRuntime_->currentSceneIndex() &&
        (!latest || isTerminalSceneLoadState(latest->state.load())))
        return;
    const uint64_t taskId = requestSceneOperation(index);
    VKR_LOG_INFO("Scene", "{} {}",
                 taskId != 0 ? "Requested switch to" : "Switched to",
                 sceneRegistry_[index].name);
}

uint64_t Application::setTextureLimit(uint32_t limit) {
    if (limit != 0 && limit != 512 && limit != 1024 && limit != 2048)
        throw RuntimeCommandError("invalid_texture_limit",
                                  "Texture limit must be 0, 512, 1024, or 2048.");
    if (sceneLoadContext_.maxTextureSize == limit)
        return 0;
    if (config_.assetImportMode == AssetImportMode::CookedOnly) {
        throw RuntimeCommandError(
            "texture_limit_locked",
            "Texture limit is fixed by the cooked package profile.");
    }
    sceneLoadContext_.maxTextureSize = limit;
    sceneWorkflow_->setTextureLimit(limit);
    VKR_LOG_INFO("Renderer", "glTF texture limit set to {}",
                 textureLimitLabel(limit));
    const int currentIndex = sceneRuntime_->currentSceneIndex();
    if (currentIndex >= 0 &&
        currentIndex < static_cast<int>(sceneRegistry_.size()) &&
        sceneRegistry_[currentIndex].isNativeScene())
        return 0;
    const auto &latest = sceneRuntime_->latestSceneLoadTask();
    return latest && !isTerminalSceneLoadState(latest->state.load())
               ? requestSceneOperation(latest->sceneIndex)
               : reloadCurrentScene();
}

uint64_t Application::requestSceneLoad(int index, bool sourceFallback,
                                       bool reloadAsset) {
    try {
        return sceneRuntime_->requestSceneLoad(index, sourceFallback,
                                               reloadAsset);
    } catch (const SceneRuntimeError &error) {
        throw RuntimeCommandError(error.code(), error.what());
    }
}

bool Application::cancelSceneLoad(uint64_t taskId) {
    return sceneRuntime_ && sceneRuntime_->cancelSceneLoad(taskId);
}

bool Application::cancelEnvironmentLoad(uint64_t taskId) {
    return sceneRuntime_ && sceneRuntime_->cancelEnvironmentLoad(taskId);
}

uint64_t Application::setEnvironment(const std::string &id) {
    if (asciiEqualsIgnoreCase(id, "None") || id.empty()) {
        sceneRuntime_->clearEnvironment();
        return 0;
    }
    if (!device_->environmentIblSupported()) {
        throw RuntimeCommandError(
            "environment_unsupported",
            "The selected Vulkan device does not support the required "
            "RGBA16F cubemap and RG16F filtering features.");
    }
    const CatalogEnvironment *environment = findEnvironmentByName(id);
    if (!environment) {
        throw RuntimeCommandError("unknown_environment",
                                  "Unknown environment '" + id + "'.");
    }
    return queueEnvironmentLoad(*environment);
}

uint64_t Application::queueEnvironmentLoad(
    const CatalogEnvironment &environment, bool reload) {
    try {
        return sceneRuntime_->queueEnvironment(environment, reload);
    } catch (const SceneRuntimeError &error) {
        throw RuntimeCommandError(error.code(), error.what());
    }
}

EnvironmentAssetHandle Application::requestEnvironmentAsset(
    const CatalogEnvironment &environment, bool reload,
    bool *repositoryHit, bool *coalesced) {
    try {
        return sceneRuntime_->requestEnvironmentAsset(
            environment, reload, repositoryHit, coalesced);
    } catch (const SceneRuntimeError &error) {
        throw RuntimeCommandError(error.code(), error.what());
    }
}

uint64_t Application::reloadCurrentEnvironment() {
    try {
        return sceneRuntime_->reloadEnvironment();
    } catch (const SceneRuntimeError &error) {
        throw RuntimeCommandError(error.code(), error.what());
    }
}

void Application::setShaderVariant(const std::string &id) {
    try {
        const std::string previous = currentShaderVariant().id;
        renderSettingsController_->setShaderVariant(id);
        if (previous != id) {
            VKR_LOG_INFO("Renderer", "Shader variant switched to {}",
                         currentShaderVariant().displayName);
        }
    } catch (const RenderSettingsError &error) {
        throw RuntimeCommandError(error.code(), error.what());
    }
}

void Application::applyRenderSettings(const RenderSettingsPatch &patch) {
    try {
        renderSettingsController_->apply(patch);
    } catch (const RenderSettingsError &error) {
        throw RuntimeCommandError(error.code(), error.what());
    }
}

const RenderSettings &Application::renderSettings() const {
    return renderSettingsController_->settings();
}

int Application::findSceneIndexByName(const std::string &name) const {
    return sceneWorkflow_->findEntryByName(name);
}

const CatalogEnvironment *
Application::findEnvironmentByName(const std::string &name) const {
    for (const CatalogEnvironment &environment : catalog_.environments) {
        if (asciiEqualsIgnoreCase(environment.id, name) ||
            asciiEqualsIgnoreCase(environment.displayName, name)) {
            return &environment;
        }
    }
    return nullptr;
}

std::string
Application::profileIdForTextureLimit(const SceneEntry &entry) const {
    return sceneWorkflow_->profileIdForEntry(entry);
}

uint64_t Application::requestSceneOperation(int index, bool sourceFallback,
                                            bool loadAfter,
                                            SceneWorkflowRequestReason reason,
                                            bool forceReimport,
                                            bool reloadAsset) {
    try {
        return sceneWorkflow_->requestEntry(
            index, sourceFallback, loadAfter, reason, forceReimport,
            reloadAsset);
    } catch (const SceneWorkflowError &error) {
        throw RuntimeCommandError(error.code(), error.what());
    }
}

bool Application::hasUnsavedSceneChanges() const {
#if VKL_ENABLE_EDITOR_UI
    return editorController_ && editorController_->hasUnsavedChanges();
#else
    return false;
#endif
}

bool Application::cancelLoadOperation(uint64_t taskId) {
    if ((taskId & EnvironmentLoadManager::kTaskIdMask) != 0 &&
        !sceneWorkflow_->isAssetTaskId(taskId)) {
        return cancelEnvironmentLoad(taskId);
    }
    if (!sceneWorkflow_->isAssetTaskId(taskId))
        return cancelSceneLoad(taskId);
    const uint64_t linked = sceneWorkflow_->linkedSceneLoadTask(taskId);
    if (linked != 0)
        return cancelSceneLoad(linked);
    return sceneWorkflow_->cancelAssetTask(taskId);
}

#if VKL_ENABLE_RUNTIME_CONTROL
void Application::processRuntimeCommand() {
    VKL_PROFILE_ZONE("Runtime Command Dispatch");
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
    const VkPhysicalDeviceProperties gpu = device_->physicalDeviceProperties();
    ControlJson fixedDelta = nullptr;
    if (config_.diagnostics.fixedDeltaSeconds)
        fixedDelta = *config_.diagnostics.fixedDeltaSeconds;
    ControlJson capabilities = {"async_scene_load", "load_status",
                                "load_cancel", "asset_catalog",
                                "camera_control", "render_status",
                                "render_settings", "environment"};
    if (sceneWorkflow_->assetAuthoringAvailable()) {
        capabilities.push_back("asset_import");
        capabilities.push_back("asset_cancel");
    }
    if (captureService_ && swapChain_->captureSupported())
        capabilities.push_back("capture");
    if (config_.diagnostics.automationMode)
        capabilities.push_back("window_resize");
    const ShaderVariant *shader = &currentShaderVariant();
    const MaterialBindingStatus &materialBinding = materialSystem_->status();
    const ValidationStatus validation = context_->validationStatus();
    const ControlJson validationFallback =
        validation.fallbackReason.empty()
            ? ControlJson(nullptr)
            : ControlJson(validation.fallbackReason);
    return {
        {"application", "VulkanLab"},
        {"protocolVersion", control::kProtocolVersion},
        {"capabilities", std::move(capabilities)},
        {"pipe", runtimeControlPipeName_},
        {"scene", sceneRuntime_->currentWorld() && sceneRuntime_->currentSceneIndex() >= 0
                      ? ControlJson(sceneRegistry_[sceneRuntime_->currentSceneIndex()].name)
                      : ControlJson(nullptr)},
        {"sceneId", sceneRuntime_->currentWorld() && sceneRuntime_->currentSceneIndex() >= 0
                        ? ControlJson(sceneRegistry_[sceneRuntime_->currentSceneIndex()].id)
                        : ControlJson(nullptr)},
        {"modelId", sceneRuntime_->currentWorld() && sceneRuntime_->currentSceneIndex() >= 0
                        && sceneRegistry_[sceneRuntime_->currentSceneIndex()].isModelPreview()
                        ? ControlJson(sceneRegistry_[sceneRuntime_->currentSceneIndex()].id)
                        : ControlJson(nullptr)},
        {"projectId", catalog_.projectId},
        {"build",
         {{"revision", build.revision},
          {"dirty", build.dirty},
          {"configuration", build.configuration},
          {"compiler", build.compiler},
          {"vulkanSdk", build.vulkanSdk},
          {"glslc", build.glslc},
          {"features",
           {{"editorUi", build.features.editorUi},
            {"runtimeControl", build.features.runtimeControl},
            {"capture", build.features.capture},
            {"assetAuthoring", build.features.assetAuthoring},
            {"validation", build.features.validation},
            {"gpuDebugUtils", build.features.gpuDebugUtils},
            {"gpuProfiling", build.features.gpuProfiling},
            {"tracy", build.features.tracy},
            {"cacao", build.features.cacao},
            {"assetTool", build.features.assetTool},
            {"controlTool", build.features.controlTool},
            {"renderTest", build.features.renderTest}}}}},
        {"gpu",
         {{"name", gpu.deviceName},
          {"vendorId", gpu.vendorID},
          {"deviceId", gpu.deviceID},
          {"deviceType", static_cast<uint32_t>(gpu.deviceType)},
          {"driverVersion", gpu.driverVersion},
          {"apiVersion", gpu.apiVersion}}},
        {"diagnostics",
         {{"automation", config_.diagnostics.automationMode},
          {"fixedDelta", fixedDelta},
          {"windowSize", {config_.windowWidth, config_.windowHeight}},
          {"windowResizable", config_.diagnostics.windowResizable()},
          {"guiVisible", config_.diagnostics.guiVisible},
          {"runtimePipeSuffix", config_.diagnostics.runtimePipeSuffix},
          {"captureRoot", projectContext_.captureRoot.u8string()},
          {"validation",
           {{"requested", validationProfileName(validation.requested)},
            {"actual", validationProfileName(validation.actual)},
            {"layerAvailable", validation.layerAvailable},
            {"validationFeaturesAvailable",
             validation.validationFeaturesAvailable},
            {"debugUtilsAvailable", validation.debugUtilsAvailable},
            {"debugUtilsEnabled", validation.debugUtilsEnabled},
            {"fallbackReason", validationFallback},
            {"warningCount", validation.warningCount},
            {"errorCount", validation.errorCount}}},
           {"tracy",
           {{"compiled", device_->tracyProfiler().compiled()},
            {"version",
             std::string(device_->tracyProfiler().version())},
            {"connected", device_->tracyProfiler().connected()},
            {"gpuAvailable",
             device_->tracyProfiler().gpuAvailable()},
            {"connectionMode",
             device_->tracyProfiler().compiled()
                 ? "on-demand-localhost"
                  : "disabled"}}},
           {"materialBinding",
            materialBindingStatusToJson(materialBinding)}}},
        {"projectRoot", projectContext_.projectRoot.u8string()},
        {"runtimeRoot", projectContext_.runtimeRoot.u8string()},
        {"assetMode", assetImportModeName(config_.assetImportMode)},
        {"cookedPackage", projectContext_.cookedPackage},
        {"package",
         {{"schemaVersion", projectContext_.packageSchemaVersion},
          {"nativeScenePackage", projectContext_.nativeScenePackage},
          {"startupSceneId",
           projectContext_.startupSceneId.empty()
               ? ControlJson(nullptr)
               : ControlJson(projectContext_.startupSceneId)}}},
        {"cacheRoot", sceneLoadContext_.derivedTextureCachePath},
        {"captureRoot", projectContext_.captureRoot.u8string()},
        {"textureLimit", sceneLoadContext_.maxTextureSize},
        {"shader", shader->displayName},
        {"shaderInfo",
         {{"id", shader->id},
          {"name", shader->displayName},
          {"category", shader->category},
          {"toneMapping",
           shaderToneMappingPolicyName(shader->toneMapping)},
          {"vertexSha256", sha256File(shader->vertSpvPath)},
          {"fragmentSha256",
           sha256File(shader->fragmentSpvPath(
               materialSystem_->activeMode()))}}},
        {"loadTask", sceneLoadTaskToJson(sceneRuntime_->latestSceneLoadTask())}};
}

ControlJson Application::runtimeSceneList() {
    ControlJson scenes = ControlJson::array();
    ControlJson entries = ControlJson::array();
    for (const auto &entry : sceneRegistry_)
        scenes.push_back(entry.name);
    for (const auto &entry : sceneRegistry_) {
        ControlJson item = {{"id", entry.id},
                            {"sceneId", entry.id},
                            {"assetKind", entry.isNativeScene()
                                              ? "scene"
                                              : "model"},
                            {"kind", sceneEntryKindName(entry.kind)},
                            {"name", entry.name},
                            {"available", entry.available},
                            {"source", entry.sourcePath}};
        if (entry.isModelPreview()) {
            item["modelId"] = entry.id;
            item["profileId"] = entry.profileId;
            item["textureLimit"] =
                catalog_.profile(entry.profileId).textureLimit;
        } else {
            item["modelId"] = nullptr;
            item["profileId"] = nullptr;
            item["textureLimit"] = nullptr;
        }
        entries.push_back(std::move(item));
    }
    return {{"scenes", std::move(scenes)},
            {"entries", std::move(entries)}};
}

ControlJson Application::runtimeSceneCurrent() {
    const ControlJson name =
        sceneRuntime_->currentWorld() && sceneRuntime_->currentSceneIndex() >= 0
            ? ControlJson(sceneRegistry_[sceneRuntime_->currentSceneIndex()].name)
            : ControlJson(nullptr);
    const ControlJson id =
        sceneRuntime_->currentWorld() && sceneRuntime_->currentSceneIndex() >= 0
            ? ControlJson(sceneRegistry_[sceneRuntime_->currentSceneIndex()].id)
            : ControlJson(nullptr);
    const SceneEntry *entry =
        sceneRuntime_->currentWorld() && sceneRuntime_->currentSceneIndex() >= 0
            ? &sceneRegistry_[sceneRuntime_->currentSceneIndex()]
            : nullptr;
    ControlJson result = {{"name", name},
                          {"id", id},
                          {"sceneId", id},
                          {"modelId", entry && entry->isModelPreview()
                                          ? id
                                          : ControlJson(nullptr)},
                          {"assetKind", entry && entry->isNativeScene()
                                            ? "scene"
                                            : "model"},
                          {"kind", entry ? sceneEntryKindName(entry->kind)
                                         : "none"}};
    if (const auto *world =
            dynamic_cast<const RuntimeWorld *>(sceneRuntime_->currentWorld().get())) {
        result["sceneDocumentId"] = world->id().value();
        result["entityCount"] = world->entityCount();
        result["modelInstanceCount"] = world->modelInstanceCount();
        result["lightCount"] = world->lights().size();
        result["activeCamera"] =
            world->activeCameraId()
                ? ControlJson(world->activeCameraId()->toString())
                : ControlJson(nullptr);
    }
    return result;
}

ControlJson Application::runtimeSceneOperationResult(int index,
                                                     uint64_t taskId) {
    ControlJson result;
    if (sceneWorkflow_->isAssetTaskId(taskId)) {
        const auto task = sceneWorkflow_->assetTask(taskId);
        result = task ? loadOperationToJson(*task, nullptr)
                      : ControlJson(nullptr);
    } else if (taskId != 0) {
        result = sceneLoadTaskToJson(sceneRuntime_->latestSceneLoadTask());
    } else {
        result = {{"scene", sceneRegistry_[index].name}, {"completed", true}};
    }
    if (taskId == 0 && sceneRuntime_->lastSceneLoadStats())
        result["loadStats"] = sceneLoadStatsToJson(*sceneRuntime_->lastSceneLoadStats());
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
    return runtimeSceneOperationResult(index, requestSceneOperation(index));
}

ControlJson Application::runtimeSceneReload() {
    if (sceneRuntime_->currentSceneIndex() < 0)
        throw RuntimeCommandError("no_current_scene",
                                  "No scene is currently loaded.");
    const int index = sceneRuntime_->currentSceneIndex();
    return runtimeSceneOperationResult(
        index, requestSceneOperation(
                   index, false, true, SceneWorkflowRequestReason::SceneLoad,
                   false, true));
}

ControlJson
Application::runtimeLoadStatus(std::optional<uint64_t> requestedTaskId) {
    uint64_t taskId = requestedTaskId.value_or(0);
    if (taskId == 0) {
        const auto activeImport = sceneWorkflow_->activeAssetTask();
        const bool environmentActive =
            sceneRuntime_->latestEnvironmentLoadTask() &&
            !isTerminalEnvironmentLoadState(
                sceneRuntime_->latestEnvironmentLoadTask()->state.load());
        taskId =
            activeImport
                ? activeImport->id
                : (environmentActive
                       ? sceneRuntime_->latestEnvironmentLoadTask()->id
                       : (sceneRuntime_->latestSceneLoadTask()
                              ? sceneRuntime_->latestSceneLoadTask()->id
                              : (sceneRuntime_->latestEnvironmentLoadTask()
                                     ? sceneRuntime_->latestEnvironmentLoadTask()->id
                                     : 0)));
    }
    if ((taskId & EnvironmentLoadManager::kTaskIdMask) != 0 &&
        !sceneWorkflow_->isAssetTaskId(taskId)) {
        const auto task = sceneRuntime_->environmentLoadTask(taskId);
        if (!task)
            throw RuntimeCommandError("load_not_found",
                                      "Load task was not found.");
        return environmentLoadTaskToJson(task);
    }
    if (sceneWorkflow_->isAssetTaskId(taskId)) {
        const auto importTask = sceneWorkflow_->assetTask(taskId);
        if (!importTask)
            throw RuntimeCommandError("load_not_found",
                                      "Load task was not found.");
        std::shared_ptr<SceneLoadTask> loadTask;
        const uint64_t linked = sceneWorkflow_->linkedSceneLoadTask(taskId);
        if (linked != 0)
            loadTask = sceneRuntime_->sceneLoadTask(linked);
        return loadOperationToJson(*importTask, loadTask);
    }

    std::shared_ptr<SceneLoadTask> task =
        sceneRuntime_->sceneLoadTask(taskId);
    if (!task)
        throw RuntimeCommandError("load_not_found",
                                  "Load task was not found.");
    const bool superseded =
        !sceneRuntime_->latestSceneLoadTask() || sceneRuntime_->latestSceneLoadTask()->id != task->id;
    return sceneLoadTaskToJson(task, superseded);
}

ControlJson
Application::runtimeLoadCancel(std::optional<uint64_t> requestedTaskId) {
    const auto activeImport = sceneWorkflow_->activeAssetTask();
    const uint64_t taskId = requestedTaskId.value_or(
        activeImport
            ? activeImport->id
            : (sceneRuntime_->latestEnvironmentLoadTask() &&
                       !isTerminalEnvironmentLoadState(
                           sceneRuntime_->latestEnvironmentLoadTask()->state.load())
                   ? sceneRuntime_->latestEnvironmentLoadTask()->id
                   : (sceneRuntime_->latestSceneLoadTask() ? sceneRuntime_->latestSceneLoadTask()->id : 0)));
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
    if (sceneWorkflow_->isAssetTaskId(taskId)) {
        const auto task = sceneWorkflow_->assetTask(taskId);
        result["loadTask"] =
            task ? loadOperationToJson(*task, nullptr) : ControlJson(nullptr);
        result["taskId"] = taskId;
    } else if (sceneRuntime_->latestSceneLoadTask() &&
               !isTerminalSceneLoadState(sceneRuntime_->latestSceneLoadTask()->state.load())) {
        result["loadTask"] = sceneLoadTaskToJson(sceneRuntime_->latestSceneLoadTask());
    } else if (sceneRuntime_->lastSceneLoadStats()) {
        result["loadStats"] = sceneLoadStatsToJson(*sceneRuntime_->lastSceneLoadStats());
    }
    return result;
}

ControlJson Application::runtimeIndexedArtifactStatus(int index) const {
    const SceneEntry &entry = sceneRegistry_[index];
    const std::string profileId = profileIdForTextureLimit(entry);
    const auto status = sceneWorkflow_->artifactStatus(index);
    if (!status)
        throw RuntimeCommandError("artifact_status_unavailable",
                                  "Artifact status is unavailable.");
    ControlJson json = artifactStatusToJson(entry, profileId, *status);
    const auto record = sceneWorkflow_->artifactRecord(index);
    if (!record)
        return json;
    json["lastSuccessfulImportUnixMs"] = record->lastSuccessfulImportUnixMs;
    json["lastSuccessfulImportTaskId"] = record->lastSuccessfulImportTaskId;
    json["lastAccessUnixMs"] = record->lastAccessUnixMs;
    if (!record->failureCode.empty()) {
        json["lastFailure"] = {{"code", record->failureCode},
                               {"message", record->failureMessage},
                               {"log", record->failureLogPath},
                               {"unixMs", record->lastFailureUnixMs}};
    }
    return json;
}

int Application::runtimeAssetSceneIndex(const std::string &name) const {
    int index = findSceneIndexByName(name);
    if (index >= 0 && sceneRegistry_[index].isModelPreview())
        return index;
    index = sceneWorkflow_->findEntryById(name);
    return index >= 0 && sceneRegistry_[index].isModelPreview() ? index : -1;
}

ControlJson Application::runtimeAssetCatalog() {
    sceneWorkflow_->refreshAllArtifactStatuses();
    ControlJson entries = ControlJson::array();
    for (int i = 0; i < static_cast<int>(sceneRegistry_.size()); ++i) {
        const SceneEntry &entry = sceneRegistry_[i];
        if (!entry.isModelPreview())
            continue;
        const std::string profileId = profileIdForTextureLimit(entry);
        if (const auto status = sceneWorkflow_->artifactStatus(i))
            entries.push_back(runtimeIndexedArtifactStatus(i));
    }
    return {{"projectId", catalog_.projectId},
            {"catalog", projectContext_.catalogPath.u8string()},
            {"mode", assetImportModeName(config_.assetImportMode)},
            {"entries", std::move(entries)}};
}

ControlJson Application::runtimeAssetStatus(
    const std::optional<std::string> &name) {
    const int index = name ? runtimeAssetSceneIndex(*name)
                           : sceneWorkflow_->selectedEntryIndex();
    if (index < 0 || index >= static_cast<int>(sceneRegistry_.size()))
        throw RuntimeCommandError("scene_not_found",
                                  "Asset scene was not found.");
    sceneWorkflow_->refreshArtifactStatus(index);
    const SceneEntry &entry = sceneRegistry_[index];
    const std::string profileId = profileIdForTextureLimit(entry);
    const auto status = sceneWorkflow_->artifactStatus(index);
    if (!status)
        throw RuntimeCommandError("artifact_status_unavailable",
                                  "Artifact status is unavailable.");
    return runtimeIndexedArtifactStatus(index);
}

ControlJson Application::runtimeAssetValidation(const std::string &name) {
    const int index = runtimeAssetSceneIndex(name);
    if (index < 0)
        throw RuntimeCommandError("scene_not_found",
                                  "Catalog scene was not found.");
    const SceneEntry &entry = sceneRegistry_[index];
    const CatalogModel *catalogModel = catalog_.findModel(entry.id);
    if (!catalogModel)
        throw RuntimeCommandError(
            "scene_not_catalog",
            "Validation queries accept Catalog scene IDs only.");

    sceneWorkflow_->refreshValidationStatus(index);
    const auto validation = sceneWorkflow_->validationStatus(index);
    if (!validation)
        throw RuntimeCommandError("validation_unavailable",
                                  "Validation status is unavailable.");
    ControlJson result = validationQueryToJson(*validation, 32);
    result["sceneId"] = entry.id;
    result["modelId"] = entry.id;
    result["assetKind"] = "model";
    result["scene"] = entry.name;
    return result;
}

ControlJson Application::runtimeAssetImport(const std::string &name,
                                            bool force, bool loadAfter) {
#if !VKL_ENABLE_ASSET_AUTHORING
    (void)name;
    (void)force;
    (void)loadAfter;
    throw RuntimeCommandError(
        "feature_not_compiled",
        "Asset authoring support was not compiled into this build.");
#else
    if (config_.assetImportMode != AssetImportMode::OnDemand)
        throw RuntimeCommandError(
            "asset_import_disabled",
            "Asset import is only available in OnDemand mode.");
    const int index = runtimeAssetSceneIndex(name);
    if (index < 0)
        throw RuntimeCommandError("scene_not_found",
                                  "Asset scene was not found.");
    const uint64_t taskId = requestSceneOperation(
        index, false, loadAfter,
        SceneWorkflowRequestReason::ManualReimport, force);
    if (taskId != 0)
        return runtimeSceneOperationResult(index, taskId);

    sceneWorkflow_->refreshArtifactStatus(index);
    const SceneEntry &entry = sceneRegistry_[index];
    const std::string profileId = profileIdForTextureLimit(entry);
    const auto status = sceneWorkflow_->artifactStatus(index);
    if (!status)
        throw RuntimeCommandError("artifact_status_unavailable",
                                  "Artifact status is unavailable.");
    ControlJson result = runtimeIndexedArtifactStatus(index);
    result["terminal"] = true;
    return result;
#endif
}

ControlJson
Application::runtimeAssetCancel(std::optional<uint64_t> requestedTaskId) {
#if !VKL_ENABLE_ASSET_AUTHORING
    (void)requestedTaskId;
    throw RuntimeCommandError(
        "feature_not_compiled",
        "Asset authoring support was not compiled into this build.");
#else
    uint64_t taskId = requestedTaskId.value_or(0);
    if (taskId == 0) {
        if (const auto active = sceneWorkflow_->activeAssetTask())
            taskId = active->id;
    }
    if (taskId == 0 || !sceneWorkflow_->isAssetTaskId(taskId) ||
        !sceneWorkflow_->cancelAssetTask(taskId)) {
        throw RuntimeCommandError("asset_not_cancellable",
                                  "Asset import cannot be cancelled.");
    }
    return {{"taskId", taskId}, {"cancelRequested", true}};
#endif
}

ControlJson Application::runtimeAssetCacheInfo() {
    const AssetWorkflowSnapshot assets = sceneWorkflow_->assetSnapshot();
    return {{"root", assets.cachePath},
            {"index", sceneWorkflow_->artifactIndexPath().u8string()},
            {"indexSchema", assets.indexSchema},
            {"records", assets.indexRecords},
            {"readyRecords", assets.readyRecords},
            {"referencedBlobs", assets.referencedBlobs},
            {"referencedBlobBytes", assets.referencedBlobBytes},
            {"blobFiles", assets.cacheBlobFiles},
            {"blobBytes", assets.cacheBlobBytes},
            {"files", assets.cacheBlobFiles},
            {"bytes", assets.cacheBlobBytes},
            {"unreferencedBlobFiles", assets.unreferencedBlobFiles},
            {"unreferencedBlobBytes", assets.unreferencedBlobBytes},
            {"mode", assets.mode}};
}

ControlJson Application::runtimeShaderList() {
    ControlJson shaders = ControlJson::array();
    ControlJson entries = ControlJson::array();
    for (const auto &variant : renderSettingsController_->shaderRegistry().variants()) {
        shaders.push_back(variant.displayName);
        entries.push_back(
            {{"id", variant.id},
             {"name", variant.displayName},
             {"category", variant.category},
             {"toneMapping",
              shaderToneMappingPolicyName(variant.toneMapping)},
             {"bloom", variant.supportsBloom},
             {"default", variant.isDefault}});
    }
    return {{"shaders", std::move(shaders)},
            {"entries", std::move(entries)}};
}

ControlJson Application::runtimeShaderCurrent() {
    const ShaderVariant &variant = currentShaderVariant();
    return {{"id", variant.id},
            {"name", variant.displayName},
            {"bloom", variant.supportsBloom}};
}

ControlJson Application::runtimeShaderSet(const std::string &name) {
    const ShaderVariant *variant = renderSettingsController_->shaderRegistry().findVariant(name);
    if (!variant) {
        ControlJson candidates = ControlJson::array();
        for (const auto &candidate : renderSettingsController_->shaderRegistry().variants()) {
            candidates.push_back(
                {{"id", candidate.id}, {"name", candidate.displayName}});
        }
        throw RuntimeCommandError(
            "shader_not_found",
            "Unknown shader '" + name + "'. Available shaders: " +
                candidates.dump());
    }
    setShaderVariant(variant->id);
    return {{"id", variant->id}, {"shader", variant->displayName}};
}

ControlJson Application::runtimeCameraGet() {
    const glm::vec3 position = camera_.position();
    return {{"position", {position.x, position.y, position.z}},
            {"yaw", camera_.yaw()},
            {"pitch", camera_.pitch()},
            {"nearPlane", camera_.nearPlane()},
            {"farPlane", camera_.farPlane()}};
}

ControlJson
Application::runtimeCameraSet(const RuntimeCameraPose &pose) {
    camera_.setPosition(
        {pose.position[0], pose.position[1], pose.position[2]});
    camera_.setYawPitch(pose.yaw, pose.pitch);
    return runtimeCameraGet();
}

ControlJson Application::runtimeWindowResize(uint32_t width,
                                             uint32_t height) {
    if (!config_.diagnostics.automationMode) {
        throw RuntimeCommandError(
            "automation_required",
            "window.resize is available only in automation mode.");
    }
    window_->resize(width, height);
    return {{"width", width}, {"height", height}};
}

ControlJson Application::runtimeRenderStatus() {
    const VkExtent2D framebufferExtent = window_->framebufferExtent();
    const bool minimized = framebufferExtent.width == 0 ||
                           framebufferExtent.height == 0;
    const bool recreatePending = frameSync_->swapChainNeedsRecreation();

    uint64_t pendingTextures = 0;
    uint64_t pendingMeshes = 0;
    uint64_t pendingUploads = 0;
    uint32_t inFlightUploadBatches = 0;
    pendingTextures = sceneRuntime_->pendingTextureCount();
    pendingMeshes = sceneRuntime_->pendingMeshCount();
    pendingUploads = sceneRuntime_->pendingUploadCount();
    inFlightUploadBatches = sceneRuntime_->inFlightUploadBatches();
    if (sceneRuntime_->latestEnvironmentLoadTask() &&
        !isTerminalEnvironmentLoadState(
            sceneRuntime_->latestEnvironmentLoadTask()->state.load())) {
        const auto &task = sceneRuntime_->latestEnvironmentLoadTask();
        const uint64_t uploaded = task->uploadedImages.load();
        pendingUploads +=
            task->totalImages > uploaded
                ? task->totalImages - uploaded
                : 0;
    }

    ControlJson captureQueue = {{"total", 0},
                                {"active", 0},
                                {"queued", 0},
                                {"recording", 0},
                                {"waitingForGpu", 0},
                                {"encoding", 0},
                                {"cancelling", 0}};
    if (captureService_) {
        const std::vector<CaptureTaskSnapshot> tasks =
            captureService_->tasks();
        captureQueue["total"] = tasks.size();
        for (const CaptureTaskSnapshot &task : tasks) {
            if (!isTerminalCaptureTaskState(task.state)) {
                captureQueue["active"] =
                    captureQueue["active"].get<uint64_t>() + 1;
            }
            const char *field = nullptr;
            switch (task.state) {
            case CaptureTaskState::Queued:
                field = "queued";
                break;
            case CaptureTaskState::Recording:
                field = "recording";
                break;
            case CaptureTaskState::WaitingForGpu:
                field = "waitingForGpu";
                break;
            case CaptureTaskState::Encoding:
                field = "encoding";
                break;
            case CaptureTaskState::Cancelling:
                field = "cancelling";
                break;
            case CaptureTaskState::Completed:
            case CaptureTaskState::Failed:
            case CaptureTaskState::Cancelled:
                break;
            }
            if (field) {
                captureQueue[field] =
                    captureQueue[field].get<uint64_t>() + 1;
            }
        }
    }

    ControlJson scene = nullptr;
    if (sceneRuntime_->currentWorld() && sceneRuntime_->currentSceneIndex() >= 0) {
        scene = {{"id", sceneRegistry_[sceneRuntime_->currentSceneIndex()].id},
                 {"name", sceneRegistry_[sceneRuntime_->currentSceneIndex()].name},
                 {"kind", sceneEntryKindName(
                              sceneRegistry_[sceneRuntime_->currentSceneIndex()].kind)}};
    }

    ControlJson loadTask = sceneLoadTaskToJson(sceneRuntime_->latestSceneLoadTask());
    if (const auto activeImport = sceneWorkflow_->activeAssetTask()) {
        std::shared_ptr<SceneLoadTask> linkedLoad;
        const uint64_t linked =
            sceneWorkflow_->linkedSceneLoadTask(activeImport->id);
        if (linked != 0)
            linkedLoad = sceneRuntime_->sceneLoadTask(linked);
        loadTask = loadOperationToJson(*activeImport, linkedLoad);
    }

    const bool captureEnabled = captureService_ != nullptr;
    const bool workspaceCaptureSupported =
        captureEnabled && swapChain_->captureSupported();
    const bool viewportCaptureSupported =
        captureEnabled &&
        describeCaptureFormat(renderer_->viewportOutput().format).supported;
    const GpuPassTimings &gpuTimings = renderer_->gpuPassTimings();
    ControlJson gpuPasses = ControlJson::object();
    for (const GpuPassTiming &pass : gpuTimings.passes)
        gpuPasses[pass.name] = pass.milliseconds;
    const RenderGraphDiagnostics &graph =
        renderer_->renderGraphDiagnostics();
    ControlJson graphExecutionOrder = ControlJson::array();
    for (const std::string &name : graph.executionOrder)
        graphExecutionOrder.push_back(name);
    ControlJson graphCulledPasses = ControlJson::array();
    for (const std::string &name : graph.culledNames)
        graphCulledPasses.push_back(name);
    ControlJson graphResources = ControlJson::array();
    for (const auto &resource : graph.resources) {
        graphResources.push_back(
            {{"index", resource.index},
             {"name", resource.name},
             {"lifetime", resource.lifetime},
             {"versions", resource.versions},
             {"residentBytes", resource.residentBytes},
             {"initialLayout",
              static_cast<int32_t>(resource.initialLayout)},
             {"finalLayout", static_cast<int32_t>(resource.finalLayout)},
             {"producers", resource.producers},
             {"consumers", resource.consumers}});
    }
    ControlJson graphBuffers = ControlJson::array();
    for (const auto &buffer : graph.buffers) {
        graphBuffers.push_back(
            {{"index", buffer.index},
             {"nativeHandle", buffer.nativeHandle},
             {"name", buffer.name},
             {"lifetime", buffer.lifetime},
             {"versions", buffer.versions},
             {"declaredRangeBytes", buffer.declaredRangeBytes},
             {"producers", buffer.producers},
             {"consumers", buffer.consumers}});
    }
    uint64_t directionalSceneLights = 0;
    uint64_t pointSceneLights = 0;
    uint64_t spotSceneLights = 0;
    uint64_t activeSceneLights = 0;
    if (sceneRuntime_->currentWorld()) {
        for (const SceneLight &light : sceneRuntime_->currentWorld()->lights()) {
            if (isEffectiveSceneLight(light))
                ++activeSceneLights;
            switch (light.type) {
            case LightType::Directional:
                ++directionalSceneLights;
                break;
            case LightType::Point:
                ++pointSceneLights;
                break;
            case LightType::Spot:
                ++spotSceneLights;
                break;
            }
        }
    }
    const SceneLightBufferStatus lightBufferStatus =
        renderer_->sceneLightBufferStatus();
    ControlJson lightCapacities = ControlJson::array();
    for (const uint32_t capacity : lightBufferStatus.frameCapacities)
        lightCapacities.push_back(capacity);
    ControlJson ignoredLightEntities = ControlJson::array();
    const size_t reportedIgnoredEntities =
        std::min<size_t>(lastLightStats_.ignoredEntityIds.size(), 32);
    for (size_t index = 0; index < reportedIgnoredEntities; ++index) {
        ignoredLightEntities.push_back(
            lastLightStats_.ignoredEntityIds[index].toString());
    }
    ControlJson shadowCaster = nullptr;
    if (!lastLightStats_.shadowCasterKey.empty()) {
        shadowCaster = {
            {"key", lastLightStats_.shadowCasterKey},
            {"name", lastLightStats_.shadowCasterName},
            {"entityId",
             lastLightStats_.shadowCasterEntity
                 ? ControlJson(
                       lastLightStats_.shadowCasterEntity->toString())
                 : ControlJson(nullptr)},
            {"bufferIndex", lastLightStats_.shadowCasterBufferIndex},
            {"active", lastLightStats_.shadowCasterActive}};
    }
    ControlJson pointShadowSelections = ControlJson::array();
    for (const PunctualShadowSelection &selection :
         lastLightStats_.pointShadowSelections) {
        ControlJson faceDraws = ControlJson::array();
        uint32_t casterDraws = 0;
        for (uint32_t face = 0; face < kPointShadowFaceCount; ++face) {
            const uint32_t drawCount =
                visibilityFrame_.cpuStats.pointShadowDraws[
                    selection.slot * kPointShadowFaceCount + face];
            faceDraws.push_back(drawCount);
            casterDraws += drawCount;
        }
        pointShadowSelections.push_back(
            {{"slot", selection.slot},
             {"key", selection.stableKey},
             {"name", selection.name},
             {"entityId",
              selection.entity
                  ? ControlJson(selection.entity->toString())
                  : ControlJson(nullptr)},
             {"farPlane", selection.farPlane},
             {"policy", shadowCastingPolicyName(selection.policy)},
             {"score", selection.score},
             {"slotAge", selection.age},
             {"retained", selection.retained},
             {"focused", selection.focused},
             {"casterDraws", casterDraws},
             {"faceDraws", std::move(faceDraws)}});
    }
    ControlJson spotShadowSelections = ControlJson::array();
    for (const PunctualShadowSelection &selection :
         lastLightStats_.spotShadowSelections) {
        const uint32_t casterDraws =
            visibilityFrame_.cpuStats.spotShadowDraws[selection.slot];
        spotShadowSelections.push_back(
            {{"slot", selection.slot},
             {"key", selection.stableKey},
             {"name", selection.name},
             {"entityId",
              selection.entity
                  ? ControlJson(selection.entity->toString())
                  : ControlJson(nullptr)},
             {"farPlane", selection.farPlane},
             {"policy", shadowCastingPolicyName(selection.policy)},
             {"score", selection.score},
             {"slotAge", selection.age},
             {"retained", selection.retained},
             {"focused", selection.focused},
             {"casterDraws", casterDraws}});
    }
    ControlJson shadowEvictions = ControlJson::array();
    for (const ShadowEviction &eviction : lastLightStats_.shadowEvictions) {
        shadowEvictions.push_back({{"key", eviction.stableKey},
                                   {"reason", eviction.reason}});
    }
    constexpr uint64_t shadowTexelBytes = 4;
    uint64_t shadowMapEstimatedBytes = 0;
    if (lastLightStats_.shadowCasterActive) {
        shadowMapEstimatedBytes +=
            shadowTexelBytes * uint64_t{kCsmCascadeCount} *
            kDirectionalShadowMapSize * kDirectionalShadowMapSize;
    }
    if (!lastLightStats_.pointShadowSelections.empty()) {
        shadowMapEstimatedBytes +=
            shadowTexelBytes * uint64_t{kPointShadowLayers} *
            kPointShadowMapSize * kPointShadowMapSize;
    }
    if (!lastLightStats_.spotShadowSelections.empty()) {
        shadowMapEstimatedBytes +=
            shadowTexelBytes * uint64_t{kMaxSpotShadowLights} *
            kSpotShadowMapSize * kSpotShadowMapSize;
    }
    const AtmosphereRuntimeStatus atmosphereStatus =
        renderer_->atmosphereStatus();
    const OcclusionCullingStatus cullingStatus =
        renderer_->occlusionCullingStatus();
    const SurfaceDataStatus surfaceStatus = renderer_->surfaceDataStatus();
    const ScreenSpaceEffectsStatus screenSpaceStatus =
        renderer_->screenSpaceEffectsStatus();
    const ReflectionProbeRuntimeStatus reflectionProbeStatus =
        renderer_->reflectionProbeStatus();
    const DdgiRuntimeStatus ddgiStatus = renderer_->ddgiStatus();
    const RayTracingSceneStatus rayTracingStatus =
        renderer_->rayTracingSceneStatus();
    const EnvironmentAssetRepositorySnapshot environmentRepository =
        sceneRuntime_->environmentRepositorySnapshot();
    ControlJson ignoredProbeEntities = ControlJson::array();
    for (size_t index = 0;
         index < std::min<size_t>(
                     reflectionProbeStatus.ignoredEntityIds.size(), 32);
         ++index) {
        ignoredProbeEntities.push_back(
            reflectionProbeStatus.ignoredEntityIds[index].toString());
    }
    ControlJson environmentRepositoryRecords = ControlJson::array();
    for (const EnvironmentAssetRecordSnapshot &record :
         environmentRepository.records) {
        environmentRepositoryRecords.push_back(
            {{"environmentId", record.key.environmentId},
             {"profileId", record.key.profileId},
             {"generation", record.generation},
             {"state", environmentAssetStateName(record.state)},
             {"consumers", record.consumerCount},
             {"uploadedImages", record.uploadedImages},
             {"error", record.error}});
    }
    ControlJson indirectCapacities = ControlJson::array();
    for (const uint32_t capacity : cullingStatus.indirectCapacities)
        indirectCapacities.push_back(capacity);
    ControlJson surfaceHistoryCapacities = ControlJson::array();
    for (const uint32_t capacity : surfaceStatus.historyCapacities)
        surfaceHistoryCapacities.push_back(capacity);
    ControlJson atmosphereComponent = nullptr;
    if (atmosphereStatus.componentPresent) {
        atmosphereComponent = atmosphereStatus.componentEntity.toString();
    }
    ControlJson atmosphereSun = nullptr;
    if (atmosphereStatus.sunEntity)
        atmosphereSun = atmosphereStatus.sunEntity->toString();
    const VkExtent2D viewportRenderExtent = renderer_->viewportExtent();
    uint32_t viewportDisplayWidth = swapChain_->extent().width;
    uint32_t viewportDisplayHeight = swapChain_->extent().height;
    bool viewportVisible = true;
    bool viewportHovered = false;
    bool viewportResizePending = false;
#if VKL_ENABLE_EDITOR_UI
    if (editorController_) {
        const EditorViewportDiagnostics editorViewport =
            editorController_->viewportDiagnostics();
        viewportDisplayWidth = editorViewport.displayWidth;
        viewportDisplayHeight = editorViewport.displayHeight;
        viewportVisible = editorViewport.visible;
        viewportHovered = editorViewport.hovered;
        viewportResizePending = editorViewport.resizePending;
    }
#endif
    const AssetRepositorySnapshot repository =
        sceneRuntime_->modelRepositorySnapshot();
    const MaterialBindingStatus &materialBinding = materialSystem_->status();
    ControlJson repositoryRecords = ControlJson::array();
    for (const ModelAssetRecordSnapshot &record : repository.records) {
        repositoryRecords.push_back(
            {{"modelId", record.key.modelId.value()},
             {"profileId", record.key.profileId},
             {"generation", record.generation},
             {"state", modelAssetStateName(record.state)},
             {"consumers", record.consumerCount},
             {"textures", record.textureCount},
             {"meshes", record.meshCount},
             {"materials", record.materialCount},
             {"primitives", record.primitiveCount},
             {"error", record.error}});
    }
    ControlJson runtimeWorld = nullptr;
    if (const auto *world =
            dynamic_cast<const RuntimeWorld *>(sceneRuntime_->currentWorld().get())) {
        runtimeWorld = {
            {"sceneDocumentId", world->id().value()},
            {"entities", world->entityCount()},
            {"modelInstances", world->modelInstanceCount()},
            {"explicitLights", world->explicitLightCount()},
            {"reflectionProbes", world->reflectionProbes().size()},
            {"renderables", world->renderableCount()},
            {"activeCamera",
             world->activeCameraId()
                 ? ControlJson(world->activeCameraId()->toString())
                 : ControlJson(nullptr)},
            {"environment",
             world->worldEnvironment()
                 ? ControlJson(world->worldEnvironment()->id)
                 : ControlJson(nullptr)}};
    }
    return {
        {"scene", std::move(scene)},
        {"runtimeWorld", std::move(runtimeWorld)},
        {"package",
         {{"cooked", projectContext_.cookedPackage},
          {"schemaVersion", projectContext_.packageSchemaVersion},
          {"nativeScenePackage", projectContext_.nativeScenePackage},
          {"startupSceneId",
           projectContext_.startupSceneId.empty()
               ? ControlJson(nullptr)
               : ControlJson(projectContext_.startupSceneId)}}},
        {"sceneGeneration", sceneRuntime_->sceneGeneration()},
        {"loadTask", std::move(loadTask)},
        {"environment",
         {{"selectedId",
           sceneRuntime_->selectedEnvironmentId().empty()
               ? ControlJson(nullptr)
               : ControlJson(sceneRuntime_->selectedEnvironmentId())},
          {"publishedId",
           renderer_->environmentReady()
               ? ControlJson(renderer_->currentEnvironmentId())
               : ControlJson(nullptr)},
          {"ready", renderer_->environmentReady()},
          {"loadTask",
           environmentLoadTaskToJson(sceneRuntime_->latestEnvironmentLoadTask())},
          {"repository",
           {{"records", environmentRepository.recordCount},
            {"ready", environmentRepository.readyCount},
            {"loading", environmentRepository.loadingCount},
            {"failed", environmentRepository.failedCount},
            {"retiring", environmentRepository.retiringCount},
            {"readyHits", environmentRepository.readyHits},
            {"coalescedRequests",
             environmentRepository.coalescedRequests},
            {"entries", std::move(environmentRepositoryRecords)}}}}},
        {"reflectionProbes",
         {{"sourceCount", reflectionProbeStatus.sourceCount},
          {"activeCount", reflectionProbeStatus.activeCount},
          {"ignoredCount", reflectionProbeStatus.ignoredCount},
          {"limit", reflectionProbeStatus.limit},
          {"descriptorGeneration",
           reflectionProbeStatus.descriptorGeneration},
          {"bufferBytes", reflectionProbeStatus.allocatedBytes},
          {"ignoredEntityIds", std::move(ignoredProbeEntities)}}},
        {"ddgi",
         {{"supported", ddgiStatus.supported},
          {"componentPresent", ddgiStatus.componentPresent},
          {"active", ddgiStatus.active},
          {"componentEntity",
           ddgiStatus.componentPresent
               ? ControlJson(ddgiStatus.componentEntity.toString())
               : ControlJson(nullptr)},
          {"probeCount", ddgiStatus.probeCount},
          {"raysPerProbe", ddgiStatus.raysPerProbe},
          {"probesUpdatedPerFrame", ddgiStatus.probesUpdatedPerFrame},
          {"updateCursor", ddgiStatus.updateCursor},
          {"tracedInstanceCount", ddgiStatus.tracedInstanceCount},
          {"generation", ddgiStatus.generation},
          {"resetCount", ddgiStatus.resetCount},
          {"allocatedBytes", ddgiStatus.allocatedBytes},
          {"unavailableReason", ddgiStatus.unavailableReason},
          {"rayTracing",
           {{"supported", rayTracingStatus.supported},
            {"active", rayTracingStatus.active},
            {"instanceCount", rayTracingStatus.instanceCount},
            {"allocatedBytes", rayTracingStatus.allocatedBytes},
            {"unavailableReason", rayTracingStatus.unavailableReason}}}}},
        {"atmosphere",
         {{"supported", atmosphereStatus.supported},
          {"componentPresent", atmosphereStatus.componentPresent},
          {"active", atmosphereStatus.active},
          {"componentEntity", std::move(atmosphereComponent)},
          {"sunEntity", std::move(atmosphereSun)},
          {"sunBufferIndex", atmosphereStatus.sunBufferIndex},
          {"cameraAltitudeKm", atmosphereStatus.cameraAltitudeKm},
          {"staticLutReady", atmosphereStatus.staticLutReady},
          {"staticLutDirty", atmosphereStatus.staticLutDirty},
          {"lutGeneration", atmosphereStatus.lutGeneration},
          {"lastUpdateMs", atmosphereStatus.lastUpdateMs},
          {"unavailableReason", atmosphereStatus.unavailableReason}}},
        {"lighting",
         {{"sceneLights",
           sceneRuntime_->currentWorld() ? sceneRuntime_->currentWorld()->lights().size() : 0},
          {"sceneDirectional", directionalSceneLights},
          {"scenePoint", pointSceneLights},
          {"sceneSpot", spotSceneLights},
          {"activeSceneLights", activeSceneLights},
          {"fallbackSunActive",
           activeSceneLights == 0 && sceneRuntime_->currentWorld() &&
               sceneRuntime_->currentWorld()->allowsFallbackSun()},
          {"effective", lastLightStats_.effectiveLights},
          {"uploadedDirectional", lastLightStats_.directionalLights},
          {"uploadedPoint", lastLightStats_.pointLights},
          {"uploadedSpot", lastLightStats_.spotLights},
          {"uploadedPunctual", lastLightStats_.punctualLights},
          {"uploadedTotal", lastLightStats_.totalLights},
          {"pointShadowLights", lastLightStats_.pointShadowLights},
          {"spotShadowLights", lastLightStats_.spotShadowLights},
          {"maxPointShadowLights", renderSettings().maxPointShadowLights},
          {"maxSpotShadowLights", renderSettings().maxSpotShadowLights},
          {"pointShadowDistance", renderSettings().pointShadowDistance},
          {"spotShadowDistance", renderSettings().spotShadowDistance},
          {"pointShadowReceiverBiasWorld",
           renderSettings().pointShadowReceiverBiasWorld},
          {"shadowSystemRevision",
           lastLightStats_.shadowContentRevision},
          {"shadowTemporalReactive",
           lastLightStats_.shadowTemporalReactive},
          {"shadowReactiveFramesRemaining",
           lastLightStats_.shadowReactiveFramesRemaining},
           {"pointShadowSelections", std::move(pointShadowSelections)},
           {"spotShadowSelections", std::move(spotShadowSelections)},
           {"shadowEvictions", std::move(shadowEvictions)},
           {"shadowMapEstimatedBytes", shadowMapEstimatedBytes},
          {"limit", lightBufferStatus.limit},
          {"frameCapacities", std::move(lightCapacities)},
          {"bufferBytes", lightBufferStatus.allocatedBytes},
          {"ignored", lastLightStats_.ignoredLights},
          {"ignoredEntityIds", std::move(ignoredLightEntities)},
          {"shadowCaster", std::move(shadowCaster)}}},
        {"culling",
         {{"supported", cullingStatus.supported},
          {"active", cullingStatus.active},
          {"unavailableReason", cullingStatus.unavailableReason},
          {"sourceDraws", visibilityFrame_.cpuStats.sourceDraws},
          {"invalidBounds", visibilityFrame_.cpuStats.invalidBounds},
          {"cameraVisible", visibilityFrame_.cpuStats.cameraVisible},
          {"cameraOpaque", visibilityFrame_.cpuStats.cameraOpaque},
          {"cameraTransparent", visibilityFrame_.cpuStats.cameraTransparent},
          {"frustumCulled", visibilityFrame_.cpuStats.frustumCulled},
          {"distanceCulled", visibilityFrame_.cpuStats.distanceCulled},
          {"smallObjectCulled",
           visibilityFrame_.cpuStats.smallObjectCulled},
          {"shadowCandidates", visibilityFrame_.cpuStats.shadowCandidates},
          {"shadowCulled", visibilityFrame_.cpuStats.shadowCulled},
          {"shadowVisible", visibilityFrame_.cpuStats.shadowVisible},
          {"directionalShadowDraws",
           visibilityFrame_.cpuStats.directionalShadowDraws},
          {"pointShadowDraws",
           visibilityFrame_.cpuStats.pointShadowDraws},
          {"spotShadowDraws",
           visibilityFrame_.cpuStats.spotShadowDraws},
          {"depthPrepassDraws", visibilityFrame_.cpuStats.depthPrepassDraws},
          {"occlusionMinCandidates",
           renderSettings().culling.occlusionMinCandidates},
          {"occlusionCandidates",
           cullingStatus.latestCandidates},
          {"gpuUncullable", cullingStatus.latestUncullable},
          {"gpuVisible", cullingStatus.completed.visible},
          {"gpuOccluded", cullingStatus.completed.occluded},
          {"gpuStatsFrameSerial", cullingStatus.completed.frameSerial},
          {"visibilityGeneration", visibilityFrame_.generation},
          {"hiZMipLevels", cullingStatus.hiZMipLevels},
          {"indirectCapacities", std::move(indirectCapacities)},
          {"allocatedBytes", cullingStatus.allocatedBytes}}},
        {"surfaceData",
         {{"supported", surfaceStatus.supported},
          {"active", surfaceStatus.active},
          {"unavailableReason", surfaceStatus.unavailableReason},
          {"debugView", surfaceDebugViewName(
                            renderSettings().surfaceDebugView)},
          {"motionDebugScale", renderSettings().surfaceMotionDebugScale},
          {"depthFormat", static_cast<int32_t>(surfaceStatus.depthFormat)},
          {"normalRoughnessFormat",
           static_cast<int32_t>(surfaceStatus.normalRoughnessFormat)},
          {"motionFormat", static_cast<int32_t>(surfaceStatus.motionFormat)},
          {"historyGeneration", visibilityFrame_.history.historyGeneration},
          {"historyValidItems", visibilityFrame_.history.historyValidItems},
          {"itemCount", visibilityFrame_.items.size()},
          {"globalHistoryValid", visibilityFrame_.history.globalValid},
          {"invalidationReason",
           visibilityFrame_.history.invalidationReason},
          {"historyCapacities", std::move(surfaceHistoryCapacities)},
           {"allocatedBytes", surfaceStatus.allocatedBytes}}},
        {"screenSpace",
         {{"depthPyramidSupported",
           screenSpaceStatus.depthPyramidSupported},
          {"colorPyramidSupported",
           screenSpaceStatus.colorPyramidSupported},
          {"ssaoSupported", screenSpaceStatus.ssaoSupported},
          {"requestedMode", ambientOcclusionModeName(
                                screenSpaceStatus.requestedMode)},
          {"activeMode", ambientOcclusionModeName(
                             screenSpaceStatus.activeMode)},
          {"requestedGiMode", globalIlluminationModeName(
                                  screenSpaceStatus.requestedGiMode)},
          {"activeGiMode", globalIlluminationModeName(
                               screenSpaceStatus.activeGiMode)},
          {"debugView", screenSpaceDebugViewName(
                            renderSettings().screenSpaceDebugView)},
          {"debugMip", renderSettings().screenSpaceDebugMip},
          {"depthMipLevels", screenSpaceStatus.depthMipLevels},
          {"colorMipLevels", screenSpaceStatus.colorMipLevels},
          {"depthExtent",
           {{"width", screenSpaceStatus.depthExtent.width},
            {"height", screenSpaceStatus.depthExtent.height}}},
          {"colorExtent",
           {{"width", screenSpaceStatus.colorExtent.width},
            {"height", screenSpaceStatus.colorExtent.height}}},
           {"ssaoExtent",
            {{"width", screenSpaceStatus.ssaoExtent.width},
             {"height", screenSpaceStatus.ssaoExtent.height}}},
           {"cacaoCompiled", screenSpaceStatus.cacaoCompiled},
           {"cacaoSupported", screenSpaceStatus.cacaoSupported},
           {"cacaoInitialized", screenSpaceStatus.cacaoInitialized},
           {"cacaoFp32", screenSpaceStatus.cacaoFp32},
           {"cacaoInternalMemoryTracked",
            screenSpaceStatus.cacaoInternalMemoryTracked},
           {"cacaoResolution",
            cacaoResolutionName(screenSpaceStatus.cacaoResolution)},
           {"cacaoGeneration", screenSpaceStatus.cacaoGeneration},
           {"cacaoOutputExtent",
            {{"width", screenSpaceStatus.cacaoOutputExtent.width},
              {"height", screenSpaceStatus.cacaoOutputExtent.height}}},
           {"gtaoSupported", screenSpaceStatus.gtaoSupported},
           {"gtaoActive", screenSpaceStatus.gtaoActive},
           {"gtaoHistoryValid", screenSpaceStatus.gtaoHistoryValid},
           {"gtaoHistoryGeneration",
            screenSpaceStatus.gtaoHistoryGeneration},
           {"gtaoLastFrameSerial", screenSpaceStatus.gtaoLastFrameSerial},
           {"gtaoExtent",
            {{"width", screenSpaceStatus.gtaoExtent.width},
             {"height", screenSpaceStatus.gtaoExtent.height}}},
           {"gtaoLastResetReason", screenSpaceStatus.gtaoLastResetReason},
          {"ssrSupported", screenSpaceStatus.ssrSupported},
          {"ssrActive", screenSpaceStatus.ssrActive},
          {"ssrHistoryValid", screenSpaceStatus.ssrHistoryValid},
          {"ssrHistoryGeneration", screenSpaceStatus.ssrHistoryGeneration},
          {"ssrLastFrameSerial", screenSpaceStatus.ssrLastFrameSerial},
          {"ssrExtent",
           {{"width", screenSpaceStatus.ssrExtent.width},
            {"height", screenSpaceStatus.ssrExtent.height}}},
          {"ssrLastResetReason", screenSpaceStatus.ssrLastResetReason},
          {"ssgiSupported", screenSpaceStatus.ssgiSupported},
          {"ssgiActive", screenSpaceStatus.ssgiActive},
          {"ssgiHistoryValid", screenSpaceStatus.ssgiHistoryValid},
          {"ssgiHistoryGeneration", screenSpaceStatus.ssgiHistoryGeneration},
          {"ssgiLastFrameSerial", screenSpaceStatus.ssgiLastFrameSerial},
          {"ssgiExtent",
           {{"width", screenSpaceStatus.ssgiExtent.width},
            {"height", screenSpaceStatus.ssgiExtent.height}}},
          {"ssgiLastResetReason", screenSpaceStatus.ssgiLastResetReason},
          {"taaSupported", screenSpaceStatus.taaSupported},
          {"taaActive", screenSpaceStatus.taaActive},
          {"taaHistoryValid", screenSpaceStatus.taaHistoryValid},
          {"taaHistoryGeneration",
           screenSpaceStatus.taaHistoryGeneration},
          {"taaLastFrameSerial", screenSpaceStatus.taaLastFrameSerial},
          {"taaExtent",
           {{"width", screenSpaceStatus.taaExtent.width},
            {"height", screenSpaceStatus.taaExtent.height}}},
          {"taaJitterPixels",
           {screenSpaceStatus.taaJitterPixels.x,
            screenSpaceStatus.taaJitterPixels.y}},
          {"taaLastResetReason", screenSpaceStatus.taaLastResetReason},
          {"estimatedMemoryBytes",
           screenSpaceStatus.estimatedMemoryBytes},
          {"depthPyramidUnavailableReason",
           screenSpaceStatus.depthPyramidUnavailableReason},
          {"colorPyramidUnavailableReason",
           screenSpaceStatus.colorPyramidUnavailableReason},
           {"ssaoUnavailableReason",
            screenSpaceStatus.ssaoUnavailableReason},
           {"cacaoUnavailableReason",
            screenSpaceStatus.cacaoUnavailableReason},
           {"gtaoUnavailableReason",
            screenSpaceStatus.gtaoUnavailableReason},
           {"taaUnavailableReason",
            screenSpaceStatus.taaUnavailableReason},
           {"ssrUnavailableReason",
            screenSpaceStatus.ssrUnavailableReason},
           {"ssgiUnavailableReason",
            screenSpaceStatus.ssgiUnavailableReason}}},
        {"frameSerial", frameSync_->lastSubmittedSerial()},
        {"completedSubmissionSerial",
         frameSync_->completedSubmissionSerial()},
        {"presentedFrames", presentedFrameCount_},
        {"gpuTimings",
         {{"compiled", build::kGpuProfiling},
          {"available", build::kGpuProfiling && gpuTimings.available},
          {"frameSerial", gpuTimings.frameSerial},
          {"passes", std::move(gpuPasses)},
           {"totalMs", gpuTimings.totalMs}}},
        {"renderGraph",
         {{"topologyHash", fmt::format("{:016x}", graph.topologyHash)},
          {"activePasses", graph.activePasses},
          {"culledPasses", graph.culledPasses},
          {"dependencyEdges", graph.dependencyEdges},
          {"automaticBarriers", graph.automaticBarriers},
          {"layoutBarriers", graph.layoutBarriers},
          {"hazardBarriers", graph.hazardBarriers},
          {"activeImageBytes", graph.activeImageBytes},
          {"logicalImageBytes", graph.logicalImageBytes},
          {"residentImageBytes", graph.residentImageBytes},
          {"retiringImageBytes", graph.retiringImageBytes},
          {"executionOrder", std::move(graphExecutionOrder)},
          {"culled", std::move(graphCulledPasses)},
          {"resources", std::move(graphResources)},
          {"buffers", std::move(graphBuffers)}}},
        {"pendingUpload", pendingUploads},
        {"pendingUploadDetail",
         {{"textures", pendingTextures},
          {"meshes", pendingMeshes},
          {"inFlightBatches", inFlightUploadBatches}}},
        {"modelAssetRepository",
         {{"records", repository.recordCount},
          {"ready", repository.readyCount},
          {"loading", repository.loadingCount},
          {"failed", repository.failedCount},
          {"retiring", repository.retiringCount},
          {"cpuPrepareStarts", repository.cpuPrepareStarts},
          {"gpuBuildStarts", repository.gpuBuildStarts},
          {"readyHits", repository.readyHits},
          {"coalescedRequests", repository.coalescedRequests},
          {"entries", std::move(repositoryRecords)}}},
        {"materials", materialBindingStatusToJson(materialBinding)},
        {"captureQueue", std::move(captureQueue)},
        {"capture",
         {{"enabled", captureEnabled},
          {"supported",
           viewportCaptureSupported || workspaceCaptureSupported},
          {"viewportSupported", viewportCaptureSupported},
          {"workspaceSupported", workspaceCaptureSupported},
          {"accepting",
           captureEnabled && captureService_->acceptingRequests()},
          {"reason", captureEnabled && !workspaceCaptureSupported
                         ? swapChain_->captureUnsupportedReason()
                         : std::string{}}}},
        {"viewport",
         {{"mode", gui_ ? "editor" : "fullscreen"},
          {"visible", viewportVisible},
          {"hovered", viewportHovered},
          {"displayExtent",
           {{"width", viewportDisplayWidth},
            {"height", viewportDisplayHeight}}},
          {"renderExtent",
           {{"width", viewportRenderExtent.width},
            {"height", viewportRenderExtent.height}}},
          {"resizePending", viewportResizePending}}},
        {"guiVisible", config_.diagnostics.guiVisible},
        {"minimized", minimized},
        {"swapchainRecreatePending", recreatePending},
        {"rendering", sceneRuntime_->currentWorld() && !minimized && !recreatePending}};
}

ControlJson Application::runtimeRenderSettingsGet() {
    const RenderSettingsSnapshot snapshot =
        renderSettingsController_->snapshot();
    const RenderSettings &settings = snapshot.requested;
    const ScreenSpaceEffectsStatus screenSpaceStatus =
        renderer_->screenSpaceEffectsStatus();
    return {{"shadowsEnabled", settings.shadowsEnabled},
            {"shadowMapSize", kDirectionalShadowMapSize},
            {"shadowReceiverBias", settings.shadowReceiverBias},
            {"pointShadowReceiverBiasWorld",
             settings.pointShadowReceiverBiasWorld},
            {"shadowConstantBias", settings.shadowConstantBias},
            {"shadowSlopeBias", settings.shadowSlopeBias},
            {"maxPointShadowLights",
             settings.maxPointShadowLights},
            {"maxSpotShadowLights",
             settings.maxSpotShadowLights},
            {"pointShadowDistance",
             settings.pointShadowDistance},
            {"spotShadowDistance",
             settings.spotShadowDistance},
            {"exposureEv", settings.exposureEv},
            {"toneMapper", toneMapperName(settings.toneMapper)},
            {"bloomEnabled", settings.bloomEnabled},
            {"bloomThreshold", settings.bloomThreshold},
            {"bloomSoftKnee", settings.bloomSoftKnee},
            {"bloomIntensity", settings.bloomIntensity},
            {"bloomAvailable", snapshot.support.bloom.supported},
            {"bloomActive", snapshot.runtime.bloomActive},
            {"bloomUnavailableReason",
             snapshot.support.bloom.unavailableReason},
            {"iblEnabled", settings.iblEnabled},
            {"skyboxEnabled", settings.skyboxEnabled},
            {"environmentIntensity",
             settings.environmentIntensity},
            {"environmentRotationRadians",
             settings.environmentRotationRadians},
            {"surfaceDebugView",
             surfaceDebugViewName(settings.surfaceDebugView)},
            {"surfaceMotionDebugScale",
             settings.surfaceMotionDebugScale},
            {"surfaceDataAvailable", snapshot.support.surfaceData.supported},
            {"surfaceDataActive", snapshot.runtime.surfaceDataActive},
            {"surfaceDataUnavailableReason",
             snapshot.support.surfaceData.unavailableReason},
            {"ambientOcclusionMode",
             ambientOcclusionModeName(
                 settings.ambientOcclusionMode)},
            {"ssaoQuality",
             ssaoQualityName(settings.ssaoQuality)},
            {"ssaoRadius", settings.ssaoRadius},
            {"ssaoBias", settings.ssaoBias},
            {"ssaoIntensity", settings.ssaoIntensity},
            {"ssaoPower", settings.ssaoPower},
            {"ssaoAvailable", snapshot.support.ssao.supported},
            {"ssaoActive",
             snapshot.runtime.activeAmbientOcclusion ==
                 AmbientOcclusionMode::Ssao},
            {"ssaoUnavailableReason",
             snapshot.support.ssao.unavailableReason},
            {"cacaoQuality", cacaoQualityName(settings.cacao.quality)},
            {"cacaoResolution",
             cacaoResolutionName(settings.cacao.resolution)},
            {"cacaoRadius", settings.cacao.radius},
            {"cacaoIntensity", settings.cacao.intensity},
            {"cacaoPower", settings.cacao.power},
            {"cacaoCompiled", screenSpaceStatus.cacaoCompiled},
            {"cacaoAvailable", snapshot.support.cacao.supported},
            {"cacaoActive",
             snapshot.runtime.activeAmbientOcclusion ==
                 AmbientOcclusionMode::Cacao},
            {"cacaoFp32", screenSpaceStatus.cacaoFp32},
            {"cacaoGeneration", screenSpaceStatus.cacaoGeneration},
            {"cacaoUnavailableReason",
             snapshot.support.cacao.unavailableReason},
            {"gtaoQuality", gtaoQualityName(settings.gtao.quality)},
            {"gtaoRadius", settings.gtao.radius},
            {"gtaoFalloff", settings.gtao.falloff},
            {"gtaoIntensity", settings.gtao.intensity},
            {"gtaoPower", settings.gtao.power},
            {"gtaoTemporalWeight", settings.gtao.temporalWeight},
            {"gtaoAvailable", snapshot.support.gtao.supported},
            {"gtaoActive",
             snapshot.runtime.activeAmbientOcclusion ==
                 AmbientOcclusionMode::Gtao},
            {"gtaoHistoryValid", screenSpaceStatus.gtaoHistoryValid},
            {"gtaoHistoryGeneration",
             screenSpaceStatus.gtaoHistoryGeneration},
            {"gtaoLastResetReason",
             screenSpaceStatus.gtaoLastResetReason},
            {"gtaoUnavailableReason",
             snapshot.support.gtao.unavailableReason},
            {"temporalAntiAliasingMode",
             temporalAntiAliasingModeName(
                 settings.temporalAntiAliasingMode)},
            {"taaHistoryWeight", settings.taaHistoryWeight},
            {"taaSharpness", settings.taaSharpness},
            {"taaAvailable", snapshot.support.taa.supported},
            {"taaActive", snapshot.runtime.taaActive},
            {"taaHistoryValid", screenSpaceStatus.taaHistoryValid},
            {"taaHistoryGeneration",
             screenSpaceStatus.taaHistoryGeneration},
            {"taaLastResetReason",
             screenSpaceStatus.taaLastResetReason},
            {"taaUnavailableReason",
             snapshot.support.taa.unavailableReason},
            {"reflectionMode",
             reflectionModeName(settings.reflectionMode)},
            {"ssrQuality", ssrQualityName(settings.ssrQuality)},
            {"ssrMaxDistance", settings.ssrMaxDistance},
            {"ssrThickness", settings.ssrThickness},
            {"ssrMaxRoughness", settings.ssrMaxRoughness},
            {"ssrIntensity", settings.ssrIntensity},
            {"ssrHistoryWeight", settings.ssrHistoryWeight},
            {"ssrAvailable", snapshot.support.ssr.supported},
            {"ssrActive", snapshot.runtime.ssrActive},
            {"ssrHistoryValid", screenSpaceStatus.ssrHistoryValid},
            {"ssrHistoryGeneration",
             screenSpaceStatus.ssrHistoryGeneration},
            {"ssrLastResetReason",
             screenSpaceStatus.ssrLastResetReason},
            {"ssrUnavailableReason",
             snapshot.support.ssr.unavailableReason},
            {"globalIlluminationMode",
             globalIlluminationModeName(
                 settings.globalIlluminationMode)},
            {"ssgiQuality", ssgiQualityName(settings.ssgiQuality)},
            {"ssgiMaxDistance", settings.ssgiMaxDistance},
            {"ssgiThickness", settings.ssgiThickness},
            {"ssgiIntensity", settings.ssgiIntensity},
            {"ssgiRadianceClamp", settings.ssgiRadianceClamp},
            {"ssgiHistoryWeight", settings.ssgiHistoryWeight},
            {"ssgiAvailable", snapshot.support.ssgi.supported},
            {"ssgiActive", snapshot.runtime.ssgiActive},
            {"ssgiHistoryValid", screenSpaceStatus.ssgiHistoryValid},
            {"ssgiHistoryGeneration",
             screenSpaceStatus.ssgiHistoryGeneration},
            {"ssgiLastResetReason",
             screenSpaceStatus.ssgiLastResetReason},
            {"ssgiUnavailableReason",
             snapshot.support.ssgi.unavailableReason},
            {"ddgiRadianceClamp", settings.ddgi.radianceClamp},
            {"ddgiDebugView",
             ddgiDebugViewName(settings.ddgi.debugView)},
            {"ddgiSupported", snapshot.support.ddgi.supported},
            {"ddgiComponentPresent",
             renderer_->ddgiStatus().componentPresent},
            {"ddgiActive", snapshot.runtime.ddgiActive},
            {"ddgiUnavailableReason",
             snapshot.support.ddgi.unavailableReason},
            {"screenSpaceDebugView",
             screenSpaceDebugViewName(
                 settings.screenSpaceDebugView)},
            {"screenSpaceDebugMip",
             settings.screenSpaceDebugMip},
            {"frustumCullingEnabled",
             settings.culling.frustumEnabled},
            {"shadowCullingEnabled",
             settings.culling.shadowCullingEnabled},
            {"shadowDistance", settings.culling.shadowDistance},
            {"distanceCullingEnabled",
             settings.culling.distanceEnabled},
            {"maxDrawDistance",
             settings.culling.maxDrawDistance},
            {"smallObjectCullingEnabled",
             settings.culling.smallObjectEnabled},
            {"minProjectedSizePixels",
             settings.culling.minProjectedSizePixels},
            {"occlusionCullingEnabled",
             settings.culling.occlusionEnabled},
            {"occlusionMinCandidates",
             settings.culling.occlusionMinCandidates},
            {"occlusionDepthBias",
             settings.culling.occlusionDepthBias},
            {"occlusionAvailable",
             snapshot.support.occlusionCulling.supported},
            {"occlusionActive",
             snapshot.runtime.occlusionCullingActive},
            {"occlusionUnavailableReason",
             snapshot.support.occlusionCulling.unavailableReason},
            {"toneMappingPolicy", "pbr_only"},
            {"bloomPolicy", "pbr_only"}};
}

ControlJson Application::runtimeRenderSettingsSet(
    const RenderSettingsPatch &patch) {
    applyRenderSettings(patch);
    return runtimeRenderSettingsGet();
}

ControlJson Application::runtimeEnvironmentList() {
    ControlJson entries = ControlJson::array();
    entries.push_back({{"id", nullptr},
                       {"name", "None"},
                       {"profileId", nullptr},
                       {"ready", true}});
    for (const CatalogEnvironment &environment : catalog_.environments) {
        ArtifactStatus status;
        if (projectContext_.cookedPackage) {
            status.state = ArtifactState::Ready;
            status.reason = "packaged";
        } else {
            status = inspectEnvironmentArtifacts(
                {std::filesystem::u8path(
                     config_.derivedTextureCachePath),
                 projectContext_.resolveProjectPath(environment.source),
                 catalog_.projectId, environment.id,
                 environment.environmentProfile});
        }
        entries.push_back(
            {{"id", environment.id},
             {"name", environment.displayName},
             {"profileId", environment.environmentProfile},
             {"ready", status.ready()},
             {"artifactState", artifactStateName(status.state)},
             {"reason", status.reason}});
    }
    return {{"supported", device_->environmentIblSupported()},
            {"entries", std::move(entries)}};
}

ControlJson Application::runtimeEnvironmentCurrent() {
    return {
        {"selectedId",
         sceneRuntime_->selectedEnvironmentId().empty()
             ? ControlJson(nullptr)
             : ControlJson(sceneRuntime_->selectedEnvironmentId())},
        {"publishedId",
         renderer_->environmentReady()
             ? ControlJson(renderer_->currentEnvironmentId())
             : ControlJson(nullptr)},
        {"ready", renderer_->environmentReady()},
        {"task", environmentLoadTaskToJson(
                     sceneRuntime_->latestEnvironmentLoadTask())}};
}

ControlJson
Application::runtimeEnvironmentSet(const std::string &name) {
    const uint64_t taskId = setEnvironment(name);
    ControlJson result = runtimeEnvironmentCurrent();
    result["taskId"] =
        taskId == 0 ? ControlJson(nullptr) : ControlJson(taskId);
    return result;
}

ControlJson Application::runtimeEnvironmentReload() {
    if (sceneRuntime_->selectedEnvironmentId().empty()) {
        throw RuntimeCommandError(
            "no_current_environment",
            "No environment is currently selected.");
    }
    const uint64_t taskId = reloadCurrentEnvironment();
    ControlJson result = runtimeEnvironmentCurrent();
    result["taskId"] = taskId;
    return result;
}

ControlJson Application::runtimeCaptureScreenshot(const std::string &path,
                                                   bool includeGui) {
#if !VKL_ENABLE_CAPTURE
    (void)path;
    (void)includeGui;
    throw RuntimeCommandError(
        "feature_not_compiled",
        "Capture support was not compiled into this build.");
#else
    if (!captureService_) {
        throw RuntimeCommandError(
            "capture_disabled",
            "Capture is disabled for this runtime configuration.");
    }
    if (!swapChain_->captureSupported()) {
        throw RuntimeCommandError("capture_unsupported",
                                  swapChain_->captureUnsupportedReason());
    }
    if (!captureService_->acceptingRequests()) {
        throw RuntimeCommandError("capture_disabled",
                                  "Capture service is shutting down.");
    }

    try {
        const uint64_t taskId = captureService_->request(
            std::filesystem::u8path(path), includeGui);
        const auto task = captureService_->task(taskId);
        if (!task)
            throw std::logic_error("queued capture task was not retained");
        return captureTaskToJson(*task);
    } catch (const std::invalid_argument &error) {
        throw RuntimeCommandError("invalid_capture_path", error.what());
    } catch (const std::length_error &error) {
        throw RuntimeCommandError("capture_queue_full", error.what());
    } catch (const RuntimeCommandError &) {
        throw;
    } catch (const std::exception &error) {
        throw RuntimeCommandError("capture_failed", error.what());
    }
#endif
}

ControlJson Application::runtimeCaptureStatus(uint64_t taskId) {
#if !VKL_ENABLE_CAPTURE
    (void)taskId;
    throw RuntimeCommandError(
        "feature_not_compiled",
        "Capture support was not compiled into this build.");
#else
    if (!captureService_) {
        throw RuntimeCommandError(
            "capture_disabled",
            "Capture is disabled for this runtime configuration.");
    }
    const auto task = captureService_->task(taskId);
    if (!task) {
        throw RuntimeCommandError("capture_not_found",
                                  "Capture task was not found.");
    }
    return captureTaskToJson(*task);
#endif
}

ControlJson Application::runtimeCaptureCancel(uint64_t taskId) {
#if !VKL_ENABLE_CAPTURE
    (void)taskId;
    throw RuntimeCommandError(
        "feature_not_compiled",
        "Capture support was not compiled into this build.");
#else
    if (!captureService_) {
        throw RuntimeCommandError(
            "capture_disabled",
            "Capture is disabled for this runtime configuration.");
    }
    const auto task = captureService_->task(taskId);
    if (!task) {
        throw RuntimeCommandError("capture_not_found",
                                  "Capture task was not found.");
    }
    if (isTerminalCaptureTaskState(task->state) ||
        !captureService_->cancel(taskId)) {
        throw RuntimeCommandError("capture_not_cancellable",
                                  "Capture task cannot be cancelled.");
    }
    const auto updated = captureService_->task(taskId);
    if (!updated)
        throw std::logic_error("cancelled capture task was not retained");
    return captureTaskToJson(*updated);
#endif
}

ControlJson Application::runtimeLastLoadStats() {
    if (!sceneRuntime_->lastSceneLoadStats())
        throw RuntimeCommandError("no_load_stats",
                                  "No scene load statistics exist.");
    return sceneLoadStatsToJson(*sceneRuntime_->lastSceneLoadStats());
}

ControlJson Application::runtimeQuit() {
    if (hasUnsavedSceneChanges()) {
        throw RuntimeCommandError(
            "unsaved_changes",
            "The active native scene has unsaved changes.");
    }
    return {{"quitting", true}};
}
#endif
void Application::updateInputMode() {
#if VKL_ENABLE_EDITOR_UI
    if (editorController_ && editorController_->activeSceneCamera()) {
        if (mode_ == InputMode::CameraDrag) {
            input_->setCursorCaptured(false);
            input_->setCursorPos(savedCursor_);
            editorController_->setCameraDragActive(false);
            mode_ = InputMode::UI;
        }
        return;
    }
#endif

    if (mode_ == InputMode::UI) {
        const bool pressed = input_->isMousePressed(MouseButton::Right);
        bool overUI = gui_ && gui_->wantCaptureMouse();
#if VKL_ENABLE_EDITOR_UI
        const bool overSceneArea =
            !editorController_ || editorController_->viewportHovered();
        if (overSceneArea)
            overUI = false;
        overUI = overUI ||
                 (editorController_ && editorController_->anyItemActive());
        overUI = overUI ||
                 (editorController_ && editorController_->blocksViewportInput());
#else
        constexpr bool overSceneArea = true;
#endif
        if (pressed && overSceneArea && !overUI) {
            savedCursor_ = input_->cursorPos();
            input_->setCursorCaptured(true);
#if VKL_ENABLE_EDITOR_UI
            if (editorController_)
                editorController_->setCameraDragActive(true);
#endif
            mode_ = InputMode::CameraDrag;
        }
    } else { // CameraDrag
        if (input_->isMouseReleased(MouseButton::Right)) {
            input_->setCursorCaptured(false);
            input_->setCursorPos(savedCursor_);
#if VKL_ENABLE_EDITOR_UI
            if (editorController_)
                editorController_->setCameraDragActive(false);
#endif
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

void Application::handleSwapChainRecreate() {
    VKL_PROFILE_ZONE("Swapchain Recreate");
    const VkExtent2D framebufferExtent = window_->framebufferExtent();
    if (framebufferExtent.width == 0 || framebufferExtent.height == 0)
        return;

    renderer_->recreateSwapChain();
    frameSync_->markAllSubmissionsCompleted();
    if (captureService_) {
        captureService_->update(frameSync_->completedSubmissionSerial());
        captureService_->onSwapChainRecreated(
            frameSync_->completedSubmissionSerial());
    }
    pipelineCache_->clear();
    frameSync_->onSwapChainRecreated();
    if (gui_)
        gui_->onSwapChainRecreated(swapChain_->imageCount());
    if (!gui_) {
        renderer_->resizeViewport(swapChain_->extent());
        camera_.setAspect(static_cast<float>(swapChain_->extent().width) /
                          static_cast<float>(swapChain_->extent().height));
    }
}

const ShaderVariant &Application::currentShaderVariant() const {
    return renderSettingsController_->currentShaderVariant();
}

void Application::mainLoop() {
    auto startTime = std::chrono::high_resolution_clock::now();
    auto lastTime = startTime;
    auto lastProfilerMemorySample = std::chrono::steady_clock::now() -
                                    std::chrono::seconds(1);
    AllocatorMemorySnapshot profilerMemory{};
    profileConfigureMemoryPlot("SceneLoad/StagingBytes");
    profileConfigureMemoryPlot("SceneLoad/ProcessedBytes");
    profileConfigureMemoryPlot("VMA/AllocationBytes");
    profileConfigureMemoryPlot("VMA/BlockBytes");
    float simulationTime = 0.0f;

    while (true) {
        VKL_PROFILE_ZONE("Application Frame");
        window_->pollEvents();
        input_->update();

#if VKL_ENABLE_EDITOR_UI
        if (window_->shouldClose() && editorController_ &&
            editorController_->interceptCloseRequest()) {
            window_->setShouldClose(false);
        }
#endif
        if (window_->shouldClose())
            break;

        if (captureService_) {
            captureService_->update(
                frameSync_->completedSubmissionSerial());
        }

        sceneWorkflow_->pump();
#if VKL_ENABLE_RUNTIME_CONTROL
        processRuntimeCommand();
        if (pendingQuitCommand_ &&
            pendingQuitCommand_->responseDelivered.load()) {
            window_->setShouldClose(true);
            pendingQuitCommand_.reset();
            break;
        }
#endif

        // 1. Pump scene work outside the frame command buffer.
        sceneRuntime_->pump();
#if VKL_ENABLE_EDITOR_UI
        if (editorController_)
            editorController_->update();
#endif

        // 2. Advance frame time.
        const auto now = std::chrono::high_resolution_clock::now();
        const float dt = config_.diagnostics.fixedDeltaSeconds
                             ? *config_.diagnostics.fixedDeltaSeconds
                             : std::chrono::duration<float>(now - lastTime)
                                   .count();
        lastTime = now;

#if VKL_ENABLE_EDITOR_UI
        if (editorController_)
            editorController_->applyPendingViewportResize();
#endif

        // 3. Begin the editor frame.
        if (gui_)
            gui_->beginFrame();
#if VKL_ENABLE_EDITOR_UI
        if (editorController_)
            editorController_->beginFrame();
#endif

        // 4. Update input mode and camera controls.
        if (!config_.diagnostics.automationMode) {
            updateInputMode();
            if (mode_ == InputMode::CameraDrag)
                processCameraInput(dt);
        }
        if (input_->isKeyPressed(Key::Escape)) {
#if VKL_ENABLE_EDITOR_UI
            if (!editorController_ || !editorController_->handleEscape())
                window_->setShouldClose(true);
#else
            window_->setShouldClose(true);
#endif
        }
#if VKL_ENABLE_CAPTURE
        if (input_->isKeyPressed(Key::F12)) {
#if VKL_ENABLE_EDITOR_UI
            if (editorController_)
                editorController_->requestManualCapture();
#endif
        }
#endif

        // 5. Tick the active world.
        simulationTime = config_.diagnostics.fixedDeltaSeconds
                             ? simulationTime + dt
                             : std::chrono::duration<float>(now - startTime)
                                   .count();
        if (sceneRuntime_->currentWorld())
            sceneRuntime_->currentWorld()->update(dt, simulationTime);

        // 6. Build editor UI.
#if VKL_ENABLE_EDITOR_UI
        if (editorController_)
            editorController_->draw();
#endif

        // 7. Render the frame.
        std::optional<FrameSync::FrameContext> ctx;
        {
            VKL_PROFILE_ZONE("Begin Render Frame");
            ctx = frameSync_->beginFrame();
        }
        sceneRuntime_->collectRetired();
        if (!ctx) {
            if (frameSync_->swapChainNeedsRecreation())
                handleSwapChainRecreate();
            if (gui_)
                gui_->discardFrame();
            input_->endFrame();
            const VkExtent2D framebufferExtent =
                window_->framebufferExtent();
            if (framebufferExtent.width == 0 ||
                framebufferExtent.height == 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(16));
            }
            continue;
        }

        std::optional<CaptureFrameSelection> captureSelection;
        if (captureService_) {
            captureService_->update(
                frameSync_->completedSubmissionSerial());
            const RendererViewportOutput viewportOutput =
                renderer_->viewportOutput();
            CaptureImageSource viewportSource{};
            viewportSource.kind = CaptureSourceKind::Viewport;
            viewportSource.image = viewportOutput.images[ctx->frameIndex];
            viewportSource.extent = viewportOutput.extent;
            viewportSource.format = viewportOutput.format;
            viewportSource.layout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            viewportSource.sourceStage =
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            viewportSource.sourceAccess =
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                VK_ACCESS_SHADER_READ_BIT;
            viewportSource.restoreStage =
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            viewportSource.restoreAccess = VK_ACCESS_SHADER_READ_BIT;
            viewportSource.supported =
                describeCaptureFormat(viewportSource.format).supported;
            if (!viewportSource.supported) {
                viewportSource.unsupportedReason =
                    "viewport format is not supported for PNG capture";
            }

            CaptureImageSource workspaceSource{};
            workspaceSource.kind = CaptureSourceKind::Workspace;
            workspaceSource.image = swapChain_->image(ctx->imageIndex);
            workspaceSource.extent = swapChain_->extent();
            workspaceSource.format = swapChain_->imageFormat();
            workspaceSource.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            workspaceSource.sourceStage =
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            workspaceSource.sourceAccess =
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            workspaceSource.restoreStage =
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            workspaceSource.restoreAccess = 0;
            workspaceSource.supported = swapChain_->captureSupported();
            workspaceSource.unsupportedReason =
                swapChain_->captureUnsupportedReason();

            const RendererHdrOutput hdrOutput = renderer_->hdrOutput();
            CaptureImageSource hdrSource{};
            hdrSource.kind = CaptureSourceKind::Hdr;
            hdrSource.image = hdrOutput.images[ctx->frameIndex];
            hdrSource.extent = hdrOutput.extent;
            hdrSource.format = hdrOutput.format;
            hdrSource.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            hdrSource.sourceStage =
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            hdrSource.sourceAccess =
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            hdrSource.restoreStage =
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            hdrSource.restoreAccess = VK_ACCESS_SHADER_READ_BIT;
            const CaptureFormatDescription hdrFormat =
                describeCaptureFormat(hdrSource.format);
            hdrSource.supported = hdrFormat.supported &&
                                  hdrFormat.encoding !=
                                      CapturePixelEncoding::Unorm8;
            if (!hdrSource.supported)
                hdrSource.unsupportedReason =
                    "renderer HDR format is not supported for HDR capture";
            captureSelection = captureService_->prepareFrame(
                viewportSource, workspaceSource, hdrSource);
        }

        RenderWorldFrameSnapshot worldSnapshot{};
        const bool hasWorldSnapshot = sceneRuntime_->currentWorld() != nullptr;
        if (sceneRuntime_->currentWorld()) {
            VKL_PROFILE_ZONE("RenderWorld Snapshot");
            worldSnapshot = sceneRuntime_->currentWorld()->buildRenderSnapshot();
        }

        RenderViewInput viewInput{};
        viewInput.view = camera_.viewMatrix();
        viewInput.projection = camera_.projectionMatrix();
        viewInput.cameraPosition = camera_.position();
        viewInput.cameraNearPlane = camera_.nearPlane();
        viewInput.cameraFarPlane = camera_.farPlane();
        viewInput.viewportExtent = renderer_->viewportExtent();
        viewInput.atmosphereSupported =
            device_->atmosphereSupport().available;
        viewInput.ddgiSupported = device_->ddgiSupport().available;
        viewInput.ambientColor = ambientColor_;
        viewInput.ambientIntensity = ambientIntensity_;
        viewInput.defaultSun = {defaultSunDirection_, defaultSunColor_,
                                defaultSunIntensity_};
        viewInput.settings = renderSettingsController_->snapshot().active;
        viewInput.environmentReady = renderer_->environmentReady();
        viewInput.maxSpecularLod =
            renderer_->currentEnvironmentMaxSpecularLod();
        std::string cameraHistoryIdentity = "editor";
        if (sceneRuntime_->currentWorld()) {
            viewInput.sceneBounds = worldSnapshot.bounds;
            viewInput.sceneLights = &worldSnapshot.lights;
            viewInput.reflectionProbes = &worldSnapshot.reflectionProbes;
            viewInput.fallbackSunEnabled =
                worldSnapshot.fallbackSunEnabled;
            viewInput.atmosphere = worldSnapshot.atmosphere;
            viewInput.ddgiProbeVolume = worldSnapshot.ddgiProbeVolume;
            if (const auto &ambient = worldSnapshot.ambient) {
                viewInput.ambientColor = ambient->color;
                viewInput.ambientIntensity = ambient->intensity;
            }
            if (const auto &environment = worldSnapshot.environment) {
                viewInput.settings.environmentIntensity =
                    environment->intensity;
                viewInput.settings.environmentRotationRadians =
                    environment->rotationRadians;
            }
            bool useActiveSceneCamera = !gui_;
#if VKL_ENABLE_EDITOR_UI
            useActiveSceneCamera = useActiveSceneCamera ||
                                   (editorController_ &&
                                    editorController_->activeSceneCamera());
#endif
            if (useActiveSceneCamera) {
                const VkExtent2D extent = renderer_->viewportExtent();
                const float aspect =
                    extent.height == 0
                        ? 1.0f
                        : static_cast<float>(extent.width) /
                              static_cast<float>(extent.height);
                if (const auto activeCamera =
                        sceneRuntime_->currentWorld()->activeCamera(aspect)) {
                    viewInput.view = activeCamera->view;
                    viewInput.projection = activeCamera->projection;
                    viewInput.cameraPosition = activeCamera->position;
                    viewInput.cameraNearPlane = activeCamera->nearPlane;
                    viewInput.cameraFarPlane = activeCamera->farPlane;
                    cameraHistoryIdentity = activeCamera->entityId.empty()
                                                ? "active-camera"
                                                : "active:" +
                                                      activeCamera->entityId
                                                          .toString();
                }
            }
        }
#if VKL_ENABLE_EDITOR_UI
        if (editorController_)
            editorController_->applyReflectionProbeCaptureView(
                viewInput, cameraHistoryIdentity);
#endif
        const bool taaJitterEnabled =
            device_->screenSpaceEffectsSupport().taaAvailable &&
            taaPassRequested(viewInput.settings);
        const TemporalJitter jitter = temporalJitter(
            presentedFrameCount_, viewInput.viewportExtent,
            taaJitterEnabled);
        viewInput.projectionJitterNdc = jitter.ndc;
        viewInput.projectionJitterPixels = jitter.pixels;
        ShadowBuildInput shadowInput{};
        shadowInput.sceneLights = &worldSnapshot.lights;
        shadowInput.sceneBounds = viewInput.sceneBounds;
        shadowInput.cameraView = viewInput.view;
        shadowInput.cameraProjection = viewInput.projection;
        shadowInput.cameraPosition = viewInput.cameraPosition;
        shadowInput.cameraNearPlane = viewInput.cameraNearPlane;
        shadowInput.cameraFarPlane = viewInput.cameraFarPlane;
        shadowInput.fallbackSunDirection = defaultSunDirection_;
        shadowInput.fallbackSunColor = defaultSunColor_;
        shadowInput.fallbackSunIntensity = defaultSunIntensity_;
        shadowInput.fallbackSunEnabled =
            hasWorldSnapshot && viewInput.fallbackSunEnabled;
        shadowInput.settings = viewInput.settings;
#if VKL_ENABLE_EDITOR_UI
        if (editorController_)
            shadowInput.focusedLightEntity =
                editorController_->focusedLightEntity();
#endif
        const ShadowFramePlan shadowPlan =
            shadowSystem_.build(shadowInput);
        const RenderView renderView =
            buildRenderView(viewInput, shadowPlan);
        if (renderView.lightStats.ignoredLights !=
            lastLightStats_.ignoredLights) {
            if (renderView.lightStats.ignoredLights > 0) {
                VKR_LOG_WARN(
                    "Lighting",
                    "Ignored {} scene lights beyond the shared limit of {}.",
                    renderView.lightStats.ignoredLights, kMaxSceneLights);
            }
        }
        lastLightStats_ = renderView.lightStats;
        {
            VKL_PROFILE_ZONE("RenderItem Collect");
            renderItems_ = std::move(worldSnapshot.renderItems);
        }
        {
            VKL_PROFILE_ZONE("Visibility Build");
            VisibilityBuildInput visibilityInput{};
            visibilityInput.sceneGeneration = sceneRuntime_->sceneGeneration();
            visibilityInput.cameraIdentity =
                std::move(cameraHistoryIdentity);
            visibilityInput.shaderIdentity =
                currentShaderVariant().id;
            visibilityInput.sceneBounds = viewInput.sceneBounds;
            visibilityFrame_ = visibilitySystem_.build(
                std::move(renderItems_), renderView,
                renderer_->viewportExtent(),
                std::move(visibilityInput));
        }

        if constexpr (build::kTracy) {
            profilePlotNumber(
                "Frame/DrawCount",
                static_cast<int64_t>(visibilityFrame_.cameraDrawCount()));
            profilePlotNumber(
                "Frame/OpaqueDrawCount",
                static_cast<int64_t>(
                    visibilityFrame_.cameraOpaque.size()));
            profilePlotNumber(
                "Frame/TransparentDrawCount",
                static_cast<int64_t>(
                    visibilityFrame_.cameraTransparent.size()));
            profilePlotNumber(
                "Visibility/Source",
                static_cast<int64_t>(visibilityFrame_.cpuStats.sourceDraws));
            profilePlotNumber(
                "Visibility/CameraVisible",
                static_cast<int64_t>(visibilityFrame_.cpuStats.cameraVisible));
            profilePlotNumber(
                "Visibility/FrustumCulled",
                static_cast<int64_t>(visibilityFrame_.cpuStats.frustumCulled));
            profilePlotNumber(
                "Visibility/DistanceCulled",
                static_cast<int64_t>(visibilityFrame_.cpuStats.distanceCulled));
            profilePlotNumber(
                "Visibility/SmallObjectCulled",
                static_cast<int64_t>(
                    visibilityFrame_.cpuStats.smallObjectCulled));
            profilePlotNumber(
                "Visibility/ShadowVisible",
                static_cast<int64_t>(visibilityFrame_.cpuStats.shadowVisible));
            profilePlotNumber(
                "Frame/GraphicsPipelines",
                static_cast<int64_t>(pipelineCache_->graphicsPipelineCount()));
            profilePlotNumber(
                "Lighting/Active",
                static_cast<int64_t>(lastLightStats_.effectiveLights));
            profilePlotNumber(
                "Lighting/Uploaded",
                static_cast<int64_t>(lastLightStats_.totalLights));
            profilePlotNumber(
                "Lighting/Ignored",
                static_cast<int64_t>(lastLightStats_.ignoredLights));
            profilePlotNumber(
                "Frame/ComputePipelines",
                static_cast<int64_t>(pipelineCache_->computePipelineCount()));
            profilePlotMemory(
                "SceneLoad/StagingBytes",
                static_cast<int64_t>(sceneRuntime_->stagingBytesInUse()));
            if (sceneRuntime_->latestSceneLoadTask()) {
                profilePlotMemory(
                    "SceneLoad/ProcessedBytes",
                    static_cast<int64_t>(
                        sceneRuntime_->latestSceneLoadTask()->progress.processedBytes.load()));
            }
            const auto profilerNow = std::chrono::steady_clock::now();
            if (profileConnected() &&
                profilerNow - lastProfilerMemorySample >=
                    std::chrono::seconds(1)) {
                profilerMemory = device_->allocatorMemorySnapshot();
                lastProfilerMemorySample = profilerNow;
            }
            profilePlotMemory(
                "VMA/AllocationBytes",
                static_cast<int64_t>(profilerMemory.allocationBytes));
            profilePlotMemory(
                "VMA/BlockBytes",
                static_cast<int64_t>(profilerMemory.blockBytes));
        }

        GuiSystem *frameGui = gui_.get();
        {
            ScopedGpuLabel frameLabel(
                device_->debugUtils(), ctx->cmd,
                "Frame " + std::to_string(presentedFrameCount_ + 1));
            renderer_->renderFrame(*ctx, visibilityFrame_, *pipelineCache_,
                                   frameGui, currentShaderVariant(),
                                   renderView,
                                   captureSelection
                                       ? std::optional<FrameCaptureSource>{
                                             captureSelection->source ==
                                                     CaptureSourceKind::Workspace
                                                 ? FrameCaptureSource::Workspace
                                                 : captureSelection->source ==
                                                           CaptureSourceKind::Hdr
                                                       ? FrameCaptureSource::Hdr
                                                       : FrameCaptureSource::Viewport}
                                       : std::nullopt,
                                   captureSelection
                                       ? std::function<void(VkCommandBuffer)>{
                                             [this](VkCommandBuffer cmd) {
                                                 captureService_->recordCopy(cmd);
                                             }}
                                        : std::function<void(VkCommandBuffer)>{});
            renderSettingsController_->updateRuntimeState(
                renderer_->featureRuntimeState());
            if constexpr (build::kTracy) {
                profilePlotNumber(
                    "Visibility/GpuOccluded",
                    static_cast<int64_t>(renderer_
                                             ->occlusionCullingStatus()
                                             .completed.occluded));
            }
        }
        device_->tracyProfiler().collect(ctx->cmd);
        const uint64_t submissionSerial = frameSync_->endFrame(*ctx);
        visibilitySystem_.commit(visibilityFrame_);
        ++presentedFrameCount_;
        profileFrameMark();
        if (captureSelection)
            captureService_->frameSubmitted(submissionSerial);

        if (frameSync_->swapChainNeedsRecreation())
            handleSwapChainRecreate();

        // 8. 甯ф湯锛氫涪寮冩湰甯ч紶鏍囧閲?
        input_->endFrame();
    }

    vkDeviceWaitIdle(device_->logicalDevice());
    frameSync_->markAllSubmissionsCompleted();
    sceneRuntime_->collectRetired();
    if (captureService_)
        captureService_->shutdown(frameSync_->completedSubmissionSerial());
}

} // namespace vkr
