#include "Application.h"
#include "SceneRuntimeCoordinator.h"
#include "SceneWorkflowController.h"

#include <BuildFeatures.h>

#include "assets/DerivedAssetPaths.h"
#include "assets/ContentHash.h"
#include "assets/ArtifactIndex.h"
#include "assets/ArtifactStatus.h"
#include "assets/AssetLoadCoordinator.h"
#include "assets/AssetImportManager.h"
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
#include "editor/EditorDockWorkspace.h"
#include "editor/ReflectionProbeCapture.h"
#include "editor/SceneEditorSession.h"
#include "editor/SceneViewportController.h"
#include "editor/EditorWidgets.h"
#include "editor/panels/AssetsPanel.h"
#include "editor/panels/InspectorPanel.h"
#include "editor/panels/OutlinerPanel.h"
#include "editor/panels/ScenesPanel.h"
#endif
#include "editor/EditorUiState.h"
#include "render/GuiSystem.h"
#include "render/DirectionalShadow.h"
#include "render/PunctualShadow.h"
#include "render/MaterialInstance.h"
#include "render/MaterialSystem.h"
#include "render/MaterialTextureSlot.h"
#include "render/PipelineCache.h"
#include "render/RenderView.h"
#include "render/Renderer.h"
#include "render/RendererShaderPaths.h"
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
#if VKL_ENABLE_EDITOR_UI && VKL_ENABLE_ASSET_AUTHORING
#include "platform/FileDialogWin32.h"
#endif

#if VKL_ENABLE_EDITOR_UI
#include <imgui.h>
#endif

#if VKL_ENABLE_EDITOR_UI
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#endif

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

glm::vec3 normalizedSunDirectionOrDefault(const glm::vec3 &direction) {
    const float lengthSquared = glm::dot(direction, direction);
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-8f)
        return glm::normalize(glm::vec3(0.3f, 0.8f, 0.5f));
    return direction / std::sqrt(lengthSquared);
}

void sunAnglesFromDirection(const glm::vec3 &direction, float &azimuth,
                            float &elevation) {
    const glm::vec3 normalized = normalizedSunDirectionOrDefault(direction);
    azimuth = std::atan2(normalized.y, normalized.x);
    elevation = std::asin(std::clamp(normalized.z, -1.0f, 1.0f));
}

glm::vec3 sunDirectionFromAngles(float azimuth, float elevation) {
    const float horizontalLength = std::cos(elevation);
    return {horizontalLength * std::cos(azimuth),
            horizontalLength * std::sin(azimuth), std::sin(elevation)};
}

glm::quat rotationLookingAlong(const glm::vec3 &forward,
                               const glm::vec3 &upHint) {
    const glm::vec3 normalizedForward = glm::normalize(forward);
    glm::vec3 right = glm::cross(normalizedForward, upHint);
    if (glm::dot(right, right) <= 1.0e-8f)
        right = glm::cross(normalizedForward, glm::vec3(0.0f, 1.0f, 0.0f));
    right = glm::normalize(right);
    const glm::vec3 up = glm::normalize(glm::cross(right, normalizedForward));
    return glm::normalize(
        glm::quat_cast(glm::mat3(right, up, -normalizedForward)));
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

const char *lightTypeName(LightType type) {
    switch (type) {
    case LightType::Directional:
        return "Directional";
    case LightType::Point:
        return "Point";
    case LightType::Spot:
        return "Spot";
    }
    return "Unknown";
}

const char *lightIntensityUnit(LightType type) {
    return type == LightType::Directional ? "lux" : "candela";
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

bool asciiContainsIgnoreCase(const std::string &text,
                             const std::string &query) {
    if (query.empty())
        return true;

    auto fold = [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    };
    std::string foldedText(text.size(), '\0');
    std::string foldedQuery(query.size(), '\0');
    std::transform(text.begin(), text.end(), foldedText.begin(), fold);
    std::transform(query.begin(), query.end(), foldedQuery.begin(), fold);
    return foldedText.find(foldedQuery) != std::string::npos;
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

ControlJson assetImportTaskToJson(
    const std::shared_ptr<AssetImportTask> &task) {
    if (!task)
        return nullptr;
    const AssetImportState state = task->state.load();
    ControlJson result = {
        {"taskId", task->id},
        {"assetKind", assetImportKindName(task->kind)},
        {"assetId", task->sceneId},
        {"sceneId", task->sceneId},
        {"profileId", task->profileId},
        {"source", task->sourcePath.u8string()},
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
        {"logPath", task->logPath.u8string()},
        {"exitCode", task->processExitCode}};
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        if (!task->error.empty())
            result["error"] = task->error;
        if (!task->manifestPath.empty())
            result["manifest"] = task->manifestPath;
        if (task->kind == AssetImportKind::SceneValidation) {
            result["validation"] = {
                {"state",
                 assetValidationStateName(task->validationState.load())},
                {"errors", task->validationErrors.load()},
                {"warnings", task->validationWarnings.load()},
                {"reportKey", task->validationReportKey},
                {"inputFingerprint", task->validationInputFingerprint},
                {"failureReason", task->validationFailureReason}};
        }
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

#if VKL_ENABLE_EDITOR_UI
std::string reflectionProbeEnvironmentId(const SceneDocumentId &sceneId,
                                         const PersistentEntityId &entityId) {
    std::string compactEntity = entityId.toString();
    compactEntity.erase(
        std::remove(compactEntity.begin(), compactEntity.end(), '-'),
        compactEntity.end());
    if (compactEntity.size() > 12)
        compactEntity.resize(12);
    return ModelImportService::suggestModelId(
        "probe-" + sceneId.value() + "-" + compactEntity);
}

void publishProbeSource(const std::filesystem::path &temporary,
                        const std::filesystem::path &destination) {
    std::filesystem::create_directories(destination.parent_path());
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING |
                         MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error(
            "Could not publish reflection probe HDR (Win32 error " +
            std::to_string(GetLastError()) + ")");
    }
}
#endif

} // namespace

namespace {

#if VKL_ENABLE_RUNTIME_CONTROL
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
    shaderRegistry_ = ShaderRegistry::load(
        projectContext_.resolveRuntimePath("shader/manifest.json"));
    currentShaderVariantId_ = shaderRegistry_.defaultVariant().id;
    VKR_LOG_INFO("ShaderRegistry", "Loaded {} programs and {} variants; default={}",
                 shaderRegistry_.programs().size(),
                 shaderRegistry_.variants().size(), currentShaderVariantId_);
    modelImportUi_ = &sceneWorkflow_->modelImport();
    sceneAssetOperations_ = &sceneWorkflow_->assetOperations();
    editorUi_ = std::make_unique<EditorUiState>();
#if VKL_ENABLE_EDITOR_UI
    editorDockWorkspace_ = std::make_unique<EditorDockWorkspace>();
    assetsPanel_ = std::make_unique<AssetsPanel>();
    scenesPanel_ = std::make_unique<ScenesPanel>();
    outlinerPanel_ = std::make_unique<OutlinerPanel>();
    inspectorPanel_ = std::make_unique<InspectorPanel>();
    sceneEditorSession_ = std::make_unique<SceneEditorSession>();
    sceneViewportController_ =
        std::make_unique<SceneViewportController>();
#endif
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
    if (modelImportUi_) {
        if (modelImportUi_->worker)
            modelImportUi_->worker->cancel = true;
        if (modelImportUi_->validationTask && assetImportManager_)
            assetImportManager_->cancel(
                modelImportUi_->validationTask->id);
        if (modelImportUi_->importFuture.valid())
            modelImportUi_->importFuture.wait();
    }
    if (assetImportManager_)
        assetImportManager_->shutdown();
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
        shaderRegistry_.supportsBindlessMaterials());
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
    const ShaderProgram &shadowOpaque =
        shaderRegistry_.program("shadow.opaque");
    const ShaderProgram &shadowMask = shaderRegistry_.program("shadow.mask");
    const ShaderProgram &shadowPointOpaque =
        shaderRegistry_.program("shadow.point-opaque");
    const ShaderProgram &shadowPointMask =
        shaderRegistry_.program("shadow.point-mask");
    const ShaderProgram &shadowSpotOpaque =
        shaderRegistry_.program("shadow.spot-opaque");
    const ShaderProgram &shadowSpotMask =
        shaderRegistry_.program("shadow.spot-mask");
    const ShaderProgram &surfacePrepassOpaque =
        shaderRegistry_.program("surface.prepass-opaque");
    const ShaderProgram &surfacePrepassMask =
        shaderRegistry_.program("surface.prepass-mask");
    const ShaderProgram &visibilityHiZInit =
        shaderRegistry_.program("visibility.hiz-init");
    const ShaderProgram &visibilityHiZReduce =
        shaderRegistry_.program("visibility.hiz-reduce");
    const ShaderProgram &visibilityOcclusion =
        shaderRegistry_.program("visibility.occlusion-cull");
    const ShaderProgram &screenDepthInit =
        shaderRegistry_.program("screenspace.depth-init");
    const ShaderProgram &screenDepthReduce =
        shaderRegistry_.program("screenspace.depth-reduce");
    const ShaderProgram &screenColorInit =
        shaderRegistry_.program("screenspace.color-init");
    const ShaderProgram &screenColorReduce =
        shaderRegistry_.program("screenspace.color-reduce");
    const ShaderProgram &ssaoTrace =
        shaderRegistry_.program("screenspace.ssao-trace");
    const ShaderProgram &ssaoBlur =
        shaderRegistry_.program("screenspace.ssao-blur");
    const ShaderProgram &cacaoNormalAdapter =
        shaderRegistry_.program("screenspace.cacao-normal-adapter");
    const ShaderProgram &gtaoTrace =
        shaderRegistry_.program("screenspace.gtao-trace");
    const ShaderProgram &gtaoTemporal =
        shaderRegistry_.program("screenspace.gtao-temporal");
    const ShaderProgram &ssrTrace =
        shaderRegistry_.program("screenspace.ssr-trace");
    const ShaderProgram &ssrTemporal =
        shaderRegistry_.program("screenspace.ssr-temporal");
    const ShaderProgram &ssrBlur =
        shaderRegistry_.program("screenspace.ssr-blur");
    const ShaderProgram &ssgiTrace =
        shaderRegistry_.program("screenspace.ssgi-trace");
    const ShaderProgram &ssgiTemporal =
        shaderRegistry_.program("screenspace.ssgi-temporal");
    const ShaderProgram &ssgiFilter =
        shaderRegistry_.program("screenspace.ssgi-filter");
    const ShaderProgram &reflectionComposite =
        shaderRegistry_.program("screenspace.reflection-composite");
    const ShaderProgram &taaResolve =
        shaderRegistry_.program("postprocess.taa-resolve");
    const ShaderProgram &toneMap =
        shaderRegistry_.program("postprocess.tonemap");
    const ShaderProgram &present =
        shaderRegistry_.program("postprocess.present");
    const ShaderProgram &bloomDownsample =
        shaderRegistry_.program("postprocess.bloom-downsample");
    const ShaderProgram &bloomUpsample =
        shaderRegistry_.program("postprocess.bloom-upsample");
    const ShaderProgram &skybox = shaderRegistry_.program("skybox");
    const ShaderProgram &atmosphereTransmittance =
        shaderRegistry_.program("atmosphere.transmittance");
    const ShaderProgram &atmosphereMultipleScattering =
        shaderRegistry_.program("atmosphere.multiple-scattering");
    const ShaderProgram &atmosphereSkyView =
        shaderRegistry_.program("atmosphere.sky-view");
    const ShaderProgram &atmosphereAerialPerspective =
        shaderRegistry_.program("atmosphere.aerial-perspective");
    const ShaderProgram &atmosphereSky =
        shaderRegistry_.program("atmosphere.sky");
    const ShaderProgram &ddgiTrace =
        shaderRegistry_.program("gi.ddgi-trace");
    const ShaderProgram &ddgiUpdate =
        shaderRegistry_.program("gi.ddgi-update");
    const auto materialFragment = [this](const ShaderProgram &program)
        -> const std::string & {
        return program.fragmentSpvPath(materialSystem_->activeMode());
    };
    RendererShaderPaths shaderPaths;
    shaderPaths.shadowVert = shadowOpaque.vertSpvPath;
    shaderPaths.shadowMaskFrag = materialFragment(shadowMask);
    shaderPaths.shadowPunctualVert =
        shadowPointOpaque.vertSpvPath;
    shaderPaths.shadowPointFrag = shadowPointOpaque.fragSpvPath;
    shaderPaths.shadowPointMaskFrag = materialFragment(shadowPointMask);
    if (shadowSpotOpaque.vertSpvPath !=
        shaderPaths.shadowPunctualVert) {
        throw std::runtime_error(
            "point and spot shadow programs must share the punctual vertex shader");
    }
    shaderPaths.shadowSpotMaskFrag = materialFragment(shadowSpotMask);
    shaderPaths.surfacePrepassVert = surfacePrepassOpaque.vertSpvPath;
    for (uint32_t attachmentCount = 0; attachmentCount <= 3;
         ++attachmentCount) {
        shaderPaths.surfacePrepassOpaqueFrags[attachmentCount] =
            surfacePrepassOpaque.fragmentSpvPath(
                materialSystem_->activeMode(), attachmentCount);
        shaderPaths.surfacePrepassMaskFrags[attachmentCount] =
            surfacePrepassMask.fragmentSpvPath(
                materialSystem_->activeMode(), attachmentCount);
    }
    shaderPaths.visibilityHiZInitComp = visibilityHiZInit.computeSpvPath;
    shaderPaths.visibilityHiZReduceComp = visibilityHiZReduce.computeSpvPath;
    shaderPaths.visibilityOcclusionComp = visibilityOcclusion.computeSpvPath;
    shaderPaths.screenDepthInitComp = screenDepthInit.computeSpvPath;
    shaderPaths.screenDepthReduceComp = screenDepthReduce.computeSpvPath;
    shaderPaths.screenColorInitComp = screenColorInit.computeSpvPath;
    shaderPaths.screenColorReduceComp = screenColorReduce.computeSpvPath;
    shaderPaths.ssaoTraceComp = ssaoTrace.computeSpvPath;
    shaderPaths.ssaoBlurComp = ssaoBlur.computeSpvPath;
    shaderPaths.cacaoNormalAdapterComp = cacaoNormalAdapter.computeSpvPath;
    shaderPaths.gtaoTraceComp = gtaoTrace.computeSpvPath;
    shaderPaths.gtaoTemporalComp = gtaoTemporal.computeSpvPath;
    shaderPaths.ssrTraceComp = ssrTrace.computeSpvPath;
    shaderPaths.ssrTemporalComp = ssrTemporal.computeSpvPath;
    shaderPaths.ssrBlurComp = ssrBlur.computeSpvPath;
    shaderPaths.ssgiTraceComp = ssgiTrace.computeSpvPath;
    shaderPaths.ssgiTemporalComp = ssgiTemporal.computeSpvPath;
    shaderPaths.ssgiFilterComp = ssgiFilter.computeSpvPath;
    shaderPaths.reflectionCompositeComp =
        reflectionComposite.computeSpvPath;
    shaderPaths.ddgiTraceComp = ddgiTrace.computeSpvPath;
    shaderPaths.ddgiUpdateComp = ddgiUpdate.computeSpvPath;
    shaderPaths.taaResolveComp = taaResolve.computeSpvPath;
    shaderPaths.fullscreenVert = toneMap.vertSpvPath;
    shaderPaths.toneMapFrag = toneMap.fragSpvPath;
    shaderPaths.presentFrag = present.fragSpvPath;
    shaderPaths.skyboxFrag = skybox.fragSpvPath;
    shaderPaths.bloomDownsampleComp = bloomDownsample.computeSpvPath;
    shaderPaths.bloomUpsampleComp = bloomUpsample.computeSpvPath;
    shaderPaths.atmosphereTransmittanceComp =
        atmosphereTransmittance.computeSpvPath;
    shaderPaths.atmosphereMultipleScatteringComp =
        atmosphereMultipleScattering.computeSpvPath;
    shaderPaths.atmosphereSkyViewComp = atmosphereSkyView.computeSpvPath;
    shaderPaths.atmosphereAerialPerspectiveComp =
        atmosphereAerialPerspective.computeSpvPath;
    shaderPaths.atmosphereSkyFrag = atmosphereSky.fragSpvPath;
    renderer_ = std::make_unique<Renderer>(
        *device_, *swapChain_, *frameSync_, *descriptorAllocator_,
        *materialSystem_,
        std::move(shaderPaths));
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
#if VKL_ENABLE_ASSET_AUTHORING
    if (config_.assetImportMode == AssetImportMode::OnDemand) {
        assetImportManager_ = std::make_unique<AssetImportManager>(
            AssetImportManagerOptions{
                projectContext_.projectRoot,
                std::filesystem::u8path(
                    config_.derivedTextureCachePath),
                std::filesystem::u8path(config_.assetToolPath),
                config_.assetImportWorkers,
                config_.assetImportMemoryBudgetMiB,
                std::filesystem::u8path(config_.gltfValidatorPath)});
    }
#endif
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
            if (sceneEditorSession_) {
                if (publication.kind == SceneLoadKind::NativeScene &&
                    publication.document && gui_) {
                    const LoadedSceneDocument &loaded =
                        *publication.document;
                    const std::shared_ptr<RuntimeWorld> runtimeWorld =
                        std::dynamic_pointer_cast<RuntimeWorld>(
                            publication.world);
                    if (!runtimeWorld) {
                        throw std::logic_error(
                            "Native scene publication did not contain a "
                            "RuntimeWorld");
                    }
                    sceneEditorSession_->attach(
                        runtimeWorld, loaded.path, loaded.sourceStamp);
                    sceneEditorSession_->setWorldChangedCallback(
                        [this]() {
                            updateEditorModelBindings();
                            updateEditorReflectionProbeBindings();
                        });
                } else {
                    sceneEditorSession_->detach();
                }
            }
#endif
            if (publication.document &&
                publication.document->document.environment) {
                const SceneEnvironmentDocument &environment =
                    *publication.document->document.environment;
                renderSettings_.environmentIntensity =
                    environment.intensity;
                renderSettings_.environmentRotationRadians =
                    environment.rotationRadians;
            }
        };
    runtimeCallbacks.environmentPublished =
        [this](const EnvironmentAssetKey &key) {
            if (!artifactIndex_)
                return;
            artifactIndex_->touchEnvironment(key.environmentId,
                                             key.profileId);
            persistArtifactIndex();
        };
    runtimeCallbacks.loadFinalized =
        [this](const std::shared_ptr<SceneLoadTask> &task,
               bool success) {
            if (!success || !artifactIndex_ || !task ||
                task->sceneIndex < 0 ||
                task->sceneIndex >=
                    static_cast<int>(sceneRegistry_.size())) {
                return;
            }
            const SceneEntry &entry = sceneRegistry_[task->sceneIndex];
            if (!entry.isModelPreview())
                return;
            const std::string &profileId = task->profileId;
            const auto profile = catalog_.importProfiles.find(profileId);
            if (profile != catalog_.importProfiles.end() &&
                profile->second.textureLimit == task->textureLimit) {
                artifactIndex_->touch(entry.id, profileId);
                persistArtifactIndex();
            }
        };
    sceneRuntime_ = std::make_unique<SceneRuntimeCoordinator>(
        *device_, *descriptorAllocator_, *materialSystem_, *renderer_,
        *frameSync_, camera_, projectContext_, catalog_, sceneRegistry_,
        sceneLoadContext_, std::move(runtimeCallbacks));
    reloadArtifactIndex();
    refreshAllArtifactStatuses();
    refreshAllValidationStatuses();
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
    sceneAssetOperations_->selectedSceneIndex = start;
    requestSceneOperation(start);

#if VKL_ENABLE_EDITOR_UI
    if (config_.diagnostics.guiVisible) {
        gui_ = std::make_unique<GuiSystem>(
            context_->instance(), *device_, swapChain_->imageFormat(),
            window_->handle(), swapChain_->imageCount(),
            swapChain_->imageCount());
        bindViewportTextures();
    }
#endif
}

uint64_t Application::reloadCurrentScene() {
    const int index = sceneRuntime_->currentSceneIndex();
    if (index < 0 || index >= static_cast<int>(sceneRegistry_.size()))
        return 0;
    const uint64_t taskId = requestSceneOperation(
        index, false, true, ImportReason::SceneLoad, false, true);
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
    refreshAllArtifactStatuses();
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
    const ShaderVariant *variant = shaderRegistry_.findVariant(id);
    if (!variant || variant->id != id)
        throw RuntimeCommandError("invalid_shader", "Invalid shader ID.");
    if (currentShaderVariantId_ == variant->id)
        return;
    currentShaderVariantId_ = variant->id;
    VKR_LOG_INFO("Renderer", "Shader variant switched to {}",
                 variant->displayName);
}

void Application::applyRenderSettings(const RenderSettingsPatch &patch) {
    if (patch.bloomEnabled && *patch.bloomEnabled && renderer_ &&
        !renderer_->bloomSupported()) {
        throw RuntimeCommandError(
            "bloom_unsupported",
            "Compute Bloom is unavailable: " +
                renderer_->bloomUnsupportedReason());
    }
    if (patch.occlusionCullingEnabled && *patch.occlusionCullingEnabled &&
        renderer_ && !renderer_->occlusionCullingStatus().supported) {
        throw RuntimeCommandError(
            "occlusion_unsupported",
            "GPU occlusion culling is unavailable: " +
                renderer_->occlusionCullingStatus().unavailableReason);
    }
    if (patch.surfaceDebugView &&
        *patch.surfaceDebugView != SurfaceDebugView::None && renderer_ &&
        !renderer_->surfaceDataStatus().supported) {
        throw RuntimeCommandError(
            "surface_data_unsupported",
            "Surface data is unavailable: " +
                renderer_->surfaceDataStatus().unavailableReason);
    }
    if (patch.ambientOcclusionMode &&
        *patch.ambientOcclusionMode == AmbientOcclusionMode::Ssao &&
        renderer_ && !renderer_->screenSpaceEffectsStatus().ssaoSupported) {
        throw RuntimeCommandError(
            "ssao_unsupported",
            "SSAO is unavailable: " +
                renderer_->screenSpaceEffectsStatus().ssaoUnavailableReason);
    }
    if (patch.ambientOcclusionMode &&
        *patch.ambientOcclusionMode == AmbientOcclusionMode::Cacao &&
        renderer_ && !renderer_->screenSpaceEffectsStatus().cacaoInitialized) {
        throw RuntimeCommandError(
            "cacao_unsupported",
            "FidelityFX CACAO is unavailable: " +
                renderer_->screenSpaceEffectsStatus().cacaoUnavailableReason);
    }
    if (patch.ambientOcclusionMode &&
        *patch.ambientOcclusionMode == AmbientOcclusionMode::Gtao && renderer_ &&
        !renderer_->screenSpaceEffectsStatus().gtaoSupported) {
        throw RuntimeCommandError(
            "gtao_unsupported",
            "GTAO is unavailable: " +
                renderer_->screenSpaceEffectsStatus().gtaoUnavailableReason);
    }
    if (patch.temporalAntiAliasingMode &&
        *patch.temporalAntiAliasingMode == TemporalAntiAliasingMode::Taa &&
        renderer_ && !renderer_->screenSpaceEffectsStatus().taaSupported) {
        throw RuntimeCommandError(
            "taa_unsupported",
            "TAA is unavailable: " +
                renderer_->screenSpaceEffectsStatus().taaUnavailableReason);
    }
    if (patch.reflectionMode &&
        *patch.reflectionMode == ReflectionMode::Ssr && renderer_ &&
        !renderer_->screenSpaceEffectsStatus().ssrSupported) {
        throw RuntimeCommandError(
            "ssr_unsupported",
            "SSR is unavailable: " +
                renderer_->screenSpaceEffectsStatus().ssrUnavailableReason);
    }
    if (patch.globalIlluminationMode &&
        (*patch.globalIlluminationMode == GlobalIlluminationMode::Ssgi ||
         *patch.globalIlluminationMode == GlobalIlluminationMode::SsgiDdgi) &&
        renderer_ && !renderer_->screenSpaceEffectsStatus().ssgiSupported) {
        throw RuntimeCommandError(
            "ssgi_unsupported",
            "SSGI is unavailable: " +
                renderer_->screenSpaceEffectsStatus().ssgiUnavailableReason);
    }
    if (patch.globalIlluminationMode &&
        (*patch.globalIlluminationMode == GlobalIlluminationMode::Ddgi ||
         *patch.globalIlluminationMode == GlobalIlluminationMode::SsgiDdgi) &&
        renderer_ && !renderer_->ddgiStatus().supported) {
        throw RuntimeCommandError(
            "ddgi_unsupported",
            "DDGI is unavailable: " +
                renderer_->ddgiStatus().unavailableReason);
    }
    if (patch.surfaceDebugView && patch.screenSpaceDebugView &&
        *patch.surfaceDebugView != SurfaceDebugView::None &&
        *patch.screenSpaceDebugView != ScreenSpaceDebugView::None) {
        throw RuntimeCommandError(
            "conflicting_debug_views",
            "Surface and screen-space debug views cannot be active together.");
    }
    if (patch.screenSpaceDebugView && renderer_) {
        const ScreenSpaceEffectsStatus status =
            renderer_->screenSpaceEffectsStatus();
        const ScreenSpaceDebugView view = *patch.screenSpaceDebugView;
        const bool supported =
            view == ScreenSpaceDebugView::None ||
            (view == ScreenSpaceDebugView::NearestDepth &&
             status.depthPyramidSupported) ||
            (view == ScreenSpaceDebugView::SceneColor &&
             status.colorPyramidSupported) ||
            ((view == ScreenSpaceDebugView::SsaoRaw ||
              view == ScreenSpaceDebugView::SsaoFiltered) &&
              status.ssaoSupported) ||
            (view == ScreenSpaceDebugView::CacaoOutput &&
             status.cacaoInitialized) ||
            ((view == ScreenSpaceDebugView::GtaoRaw ||
              view == ScreenSpaceDebugView::GtaoTemporal ||
              view == ScreenSpaceDebugView::GtaoFiltered ||
              view == ScreenSpaceDebugView::GtaoRejection ||
              view == ScreenSpaceDebugView::GtaoHistoryWeight) &&
             status.gtaoSupported) ||
            ((view == ScreenSpaceDebugView::TaaHistory ||
              view == ScreenSpaceDebugView::TaaRejection ||
              view == ScreenSpaceDebugView::TaaHistoryWeight) &&
             status.taaSupported) ||
            ((view == ScreenSpaceDebugView::SsrRaw ||
              view == ScreenSpaceDebugView::SsrTemporal ||
              view == ScreenSpaceDebugView::SsrFiltered ||
              view == ScreenSpaceDebugView::SsrConfidence ||
              view == ScreenSpaceDebugView::SsrRejection) &&
             status.ssrSupported) ||
            ((view == ScreenSpaceDebugView::SsgiRaw ||
              view == ScreenSpaceDebugView::SsgiTemporal ||
              view == ScreenSpaceDebugView::SsgiFiltered ||
              view == ScreenSpaceDebugView::SsgiConfidence ||
              view == ScreenSpaceDebugView::SsgiVariance ||
              view == ScreenSpaceDebugView::SsgiRejection) &&
             status.ssgiSupported);
        if (!supported) {
            throw RuntimeCommandError(
                "screen_space_unsupported",
                "The requested screen-space debug resource is unavailable.");
        }
    }
    RenderSettings next = renderSettings_;
    applyRenderSettingsPatch(next, patch);
    if (patch.surfaceDebugView &&
        *patch.surfaceDebugView != SurfaceDebugView::None) {
        next.screenSpaceDebugView = ScreenSpaceDebugView::None;
    }
    if (patch.screenSpaceDebugView &&
        *patch.screenSpaceDebugView != ScreenSpaceDebugView::None) {
        next.surfaceDebugView = SurfaceDebugView::None;
    }
    next.shadowReceiverBias =
        glm::clamp(next.shadowReceiverBias, 0.0f, 0.05f);
    next.pointShadowReceiverBiasWorld =
        glm::clamp(next.pointShadowReceiverBiasWorld, 0.0f, 1.0f);
    next.shadowConstantBias =
        glm::clamp(next.shadowConstantBias, 0.0f, 10.0f);
    next.shadowSlopeBias = glm::clamp(next.shadowSlopeBias, 0.0f, 10.0f);
    next.maxPointShadowLights =
        std::min(next.maxPointShadowLights, kMaxPointShadowLights);
    next.maxSpotShadowLights =
        std::min(next.maxSpotShadowLights, kMaxSpotShadowLights);
    next.pointShadowDistance =
        glm::clamp(next.pointShadowDistance, kMinPunctualShadowDistance,
                   kMaxPunctualShadowDistance);
    next.spotShadowDistance =
        glm::clamp(next.spotShadowDistance, kMinPunctualShadowDistance,
                   kMaxPunctualShadowDistance);
    next.exposureEv = glm::clamp(next.exposureEv, -10.0f, 10.0f);
    next.bloomThreshold =
        glm::clamp(next.bloomThreshold, 0.0f, 20.0f);
    next.bloomSoftKnee =
        glm::clamp(next.bloomSoftKnee, 0.0f, 1.0f);
    next.bloomIntensity =
        glm::clamp(next.bloomIntensity, 0.0f, 5.0f);
    next.environmentIntensity =
        glm::clamp(next.environmentIntensity, 0.0f, 100.0f);
    next.environmentRotationRadians =
        std::remainder(next.environmentRotationRadians,
                       glm::two_pi<float>());
    next.surfaceMotionDebugScale =
        glm::clamp(next.surfaceMotionDebugScale, 0.1f, 1024.0f);
    next.ssaoRadius = glm::clamp(next.ssaoRadius, 0.05f, 10.0f);
    next.ssaoBias = glm::clamp(next.ssaoBias, 0.0f, 0.2f);
    next.ssaoIntensity = glm::clamp(next.ssaoIntensity, 0.0f, 4.0f);
    next.ssaoPower = glm::clamp(next.ssaoPower, 0.25f, 4.0f);
    next.cacao.radius = glm::clamp(next.cacao.radius, 0.05f, 10.0f);
    next.cacao.intensity = glm::clamp(next.cacao.intensity, 0.0f, 4.0f);
    next.cacao.power = glm::clamp(next.cacao.power, 0.25f, 4.0f);
    next.gtao.radius = glm::clamp(next.gtao.radius, 0.05f, 10.0f);
    next.gtao.falloff = glm::clamp(next.gtao.falloff, 0.0f, 0.99f);
    next.gtao.intensity = glm::clamp(next.gtao.intensity, 0.0f, 4.0f);
    next.gtao.power = glm::clamp(next.gtao.power, 0.25f, 4.0f);
    next.gtao.temporalWeight =
        glm::clamp(next.gtao.temporalWeight, 0.0f, 0.99f);
    next.taaHistoryWeight =
        glm::clamp(next.taaHistoryWeight, 0.0f, 0.99f);
    next.taaSharpness = glm::clamp(next.taaSharpness, 0.0f, 1.0f);
    next.ssrMaxDistance =
        glm::clamp(next.ssrMaxDistance, 0.1f, 1000.0f);
    next.ssrThickness = glm::clamp(next.ssrThickness, 0.001f, 10.0f);
    next.ssrMaxRoughness = glm::clamp(next.ssrMaxRoughness, 0.0f, 1.0f);
    next.ssrIntensity = glm::clamp(next.ssrIntensity, 0.0f, 4.0f);
    next.ssrHistoryWeight =
        glm::clamp(next.ssrHistoryWeight, 0.0f, 0.99f);
    next.ssgiMaxDistance =
        glm::clamp(next.ssgiMaxDistance, 0.05f, 1000.0f);
    next.ssgiThickness = glm::clamp(next.ssgiThickness, 0.001f, 10.0f);
    next.ssgiIntensity = glm::clamp(next.ssgiIntensity, 0.0f, 4.0f);
    next.ssgiRadianceClamp =
        glm::clamp(next.ssgiRadianceClamp, 0.1f, 100.0f);
    next.ssgiHistoryWeight =
        glm::clamp(next.ssgiHistoryWeight, 0.0f, 0.99f);
    next.ddgi.radianceClamp =
        glm::clamp(next.ddgi.radianceClamp, 0.1f, 100.0f);
    next.screenSpaceDebugMip =
        std::min(next.screenSpaceDebugMip, 31u);
    next.culling.shadowDistance =
        glm::clamp(next.culling.shadowDistance,
                   kMinDirectionalShadowDistance,
                   kMaxDirectionalShadowDistance);
    next.culling.maxDrawDistance =
        glm::clamp(next.culling.maxDrawDistance, 0.1f, 1000000.0f);
    next.culling.minProjectedSizePixels = glm::clamp(
        next.culling.minProjectedSizePixels, 0.0f, 256.0f);
    next.culling.occlusionMinCandidates =
        std::min(next.culling.occlusionMinCandidates, 65536u);
    next.culling.occlusionDepthBias =
        glm::clamp(next.culling.occlusionDepthBias, 0.0f, 0.05f);
    if (renderer_ && frameSync_ &&
        next.cacao.resolution != renderSettings_.cacao.resolution &&
        renderer_->screenSpaceEffectsStatus().cacaoInitialized) {
        frameSync_->waitForAllFrames();
        std::string error;
        if (!renderer_->reconfigureCacao(next.cacao.resolution, error)) {
            throw RuntimeCommandError(
                "cacao_reconfigure_failed",
                error.empty() ? "Failed to reconfigure FidelityFX CACAO."
                              : error);
        }
    }
    renderSettings_ = next;
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
    if (!entry.isModelPreview())
        return {};
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
                std::filesystem::u8path(
                    sceneLoadContext_.derivedTextureCachePath),
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
    if (!entry.isModelPreview())
        return;
    const std::string profileId = profileIdForTextureLimit(entry);
    const std::string key = artifactStatusKey(entry.id, profileId);
    ArtifactStatus status;
    if (!entry.available) {
        status.state = ArtifactState::Missing;
        status.reason = entry.unavailableReason;
    } else {
        const ArtifactStatusRequest request{
            std::filesystem::u8path(
                sceneLoadContext_.derivedTextureCachePath),
            entry.sourcePath,
            catalog_.projectId, entry.id, profileId,
            sceneLoadContext_.maxTextureSize,
            *textureEncoderFromName(
                catalog_.profile(profileId).textureEncoder)};
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

void Application::refreshValidationStatus(int sceneIndex) {
    if (!sceneAssetOperations_ || sceneIndex < 0 ||
        sceneIndex >= static_cast<int>(sceneRegistry_.size()))
        return;

    const SceneEntry &entry = sceneRegistry_[sceneIndex];
    if (!entry.isModelPreview())
        return;
    AssetValidationQuery query;
    const CatalogModel *catalogModel = catalog_.findModel(entry.id);
    if (!catalogModel || catalogModel->type != "gltf") {
        query.state = AssetValidationState::NotApplicable;
        query.reason = "validation applies only to Catalog glTF scenes";
    } else {
        query = querySceneValidation(
            std::filesystem::u8path(
                sceneLoadContext_.derivedTextureCachePath),
            projectContext_.projectRoot, entry.id);
    }
    sceneAssetOperations_->validationStatuses[entry.id] = std::move(query);
}

void Application::refreshAllValidationStatuses() {
    if (!sceneAssetOperations_)
        return;
    sceneAssetOperations_->validationStatuses.clear();
    for (int i = 0; i < static_cast<int>(sceneRegistry_.size()); ++i)
        refreshValidationStatus(i);
}

uint64_t Application::requestSceneOperation(int index, bool sourceFallback,
                                            bool loadAfter,
                                            ImportReason reason,
                                            bool forceReimport,
                                            bool reloadAsset) {
    if (index < 0 || index >= static_cast<int>(sceneRegistry_.size()))
        throw RuntimeCommandError("invalid_scene", "Invalid scene index.");
#if VKL_ENABLE_EDITOR_UI
    if (hasUnsavedSceneChanges()) {
        throw RuntimeCommandError(
            "unsaved_changes",
            "The active native scene has unsaved changes.");
    }
#endif
    const SceneEntry &entry = sceneRegistry_[index];
    if (!entry.available)
        throw RuntimeCommandError("scene_unavailable",
                                  entry.unavailableReason);

    const uint64_t generation =
        sceneAssetOperations_->coordinator.beginOperation();
    sceneAssetOperations_->selectedSceneIndex = index;
    sceneAssetOperations_->error.clear();

    if (entry.isNativeScene()) {
        if (!loadAfter)
            return 0;
        return requestSceneLoad(index, false, reloadAsset);
    }

    if (sourceFallback) {
        if (config_.assetImportMode == AssetImportMode::CookedOnly) {
            throw RuntimeCommandError(
                "source_fallback_disabled",
                "Source fallback is disabled in CookedOnly mode.");
        }
        return loadAfter ? requestSceneLoad(index, true, reloadAsset) : 0;
    }

    refreshArtifactStatus(index, true);
    const std::string profileId = profileIdForTextureLimit(entry);
    const auto found = sceneAssetOperations_->statuses.find(
        artifactStatusKey(entry.id, profileId));
    const bool ready = found != sceneAssetOperations_->statuses.end() &&
                       found->second.ready();
    if (ready && !forceReimport)
        return loadAfter ? requestSceneLoad(index, false, reloadAsset) : 0;

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

bool Application::hasUnsavedSceneChanges() const {
#if VKL_ENABLE_EDITOR_UI
    return sceneEditorSession_ && sceneEditorSession_->active() &&
           sceneEditorSession_->dirty();
#else
    return false;
#endif
}

void Application::requestEditorSceneLoad(int index) {
#if VKL_ENABLE_EDITOR_UI
    if (hasUnsavedSceneChanges()) {
        editorUi_->pendingAction = EditorPendingActionKind::LoadScene;
        editorUi_->pendingSceneIndex = index;
        editorUi_->requestDirtyModal = true;
        return;
    }
#endif
    requestSceneOperation(index);
}

void Application::updateEditorModelBindings() {
#if VKL_ENABLE_EDITOR_UI
    if (!sceneEditorSession_ || !sceneEditorSession_->active() ||
        !sceneRuntime_)
        return;
    const std::shared_ptr<RuntimeWorld> world = sceneEditorSession_->world();
    for (const RuntimeEntitySnapshot &entity : world->entities()) {
        if (!entity.modelInstance ||
            entity.modelBindingState != ModelBindingState::Unresolved)
            continue;
        const std::string modelId = entity.modelInstance->model.value();
        const auto source = resolveModelSource(
            catalog_, projectContext_, ModelAssetId(modelId));
        if (!source || !source->instanceable) {
            world->bindModel(entity.handle, entity.modelBindingRevision, {},
                             {}, "Model is not instanceable: " + modelId);
            continue;
        }
        if (!source->available || !source->prepareFactory) {
            world->bindModel(entity.handle, entity.modelBindingRevision, {},
                             {}, source->unavailableReason.empty()
                                     ? "Model source is unavailable: " +
                                           modelId
                                     : source->unavailableReason);
            continue;
        }
        try {
            SceneLoadContext context = sceneLoadContext_;
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
            ModelAssetHandle handle = sceneRuntime_->requestModel(request);
            world->bindModel(entity.handle, entity.modelBindingRevision,
                             source->profileId, std::move(handle));
        } catch (const std::exception &error) {
            world->bindModel(entity.handle, entity.modelBindingRevision, {},
                             {}, error.what());
        }
    }

    const auto environment = world->worldEnvironment();
    try {
        if (!environment) {
            if (!sceneRuntime_->selectedEnvironmentId().empty())
                setEnvironment("None");
        } else if (sceneRuntime_->selectedEnvironmentId() != environment->id) {
            setEnvironment(environment->id);
        }
        if (environment) {
            renderSettings_.environmentIntensity = environment->intensity;
            renderSettings_.environmentRotationRadians =
                environment->rotationRadians;
        }
    } catch (const std::exception &error) {
        editorUi_->sceneError = error.what();
    }
#endif
}

void Application::updateEditorReflectionProbeBindings() {
#if VKL_ENABLE_EDITOR_UI
    if (!sceneEditorSession_ || !sceneEditorSession_->active() ||
        !sceneRuntime_)
        return;
    const std::shared_ptr<RuntimeWorld> world = sceneEditorSession_->world();
    for (const RuntimeEntitySnapshot &entity : world->entities()) {
        if (!entity.reflectionProbe ||
            entity.reflectionProbeBindingState !=
                ModelBindingState::Unresolved)
            continue;
        if (!entity.reflectionProbe->environmentId) {
            continue;
        }
        const std::string &environmentId =
            *entity.reflectionProbe->environmentId;
        const CatalogEnvironment *environment =
            catalog_.findEnvironment(environmentId);
        if (!environment) {
            world->bindReflectionProbe(
                entity.handle, entity.reflectionProbeBindingRevision, {},
                {}, "Unknown probe environment: " + environmentId);
            continue;
        }
        try {
            EnvironmentAssetHandle handle =
                requestEnvironmentAsset(*environment);
            world->bindReflectionProbe(
                entity.handle, entity.reflectionProbeBindingRevision,
                environment->environmentProfile, std::move(handle));
        } catch (const std::exception &error) {
            world->bindReflectionProbe(
                entity.handle, entity.reflectionProbeBindingRevision, {},
                {}, error.what());
        }
    }
#endif
}

void Application::beginReflectionProbeCapture(
    PersistentEntityId entityId) {
#if VKL_ENABLE_EDITOR_UI
    if (reflectionProbeCapture_)
        throw std::runtime_error(
            "Another reflection probe capture is already active");
    if (!sceneEditorSession_ || !sceneEditorSession_->active())
        throw std::runtime_error("No native scene is open");
    if (!projectContext_.catalogWritable)
        throw std::runtime_error("The project Catalog is read-only");
    if (!captureService_ || !assetImportManager_)
        throw std::runtime_error(
            "Reflection probe capture requires Capture and Asset Authoring");

    const auto entity = sceneEditorSession_->world()->entity(
        sceneEditorSession_->world()->find(entityId));
    if (!entity || !entity->reflectionProbe)
        throw std::runtime_error(
            "The selected entity is not a reflection probe");
    if (catalog_.environmentProfiles.empty())
        throw std::runtime_error(
            "The Catalog does not define an environment profile");

    ReflectionProbeCaptureState state;
    state.entityId = entityId;
    state.environmentId = reflectionProbeEnvironmentId(
        sceneEditorSession_->world()->id(), entityId);
    if (const CatalogEnvironment *existing =
            catalog_.findEnvironment(state.environmentId)) {
        state.profileId = existing->environmentProfile;
        state.sourcePath =
            projectContext_.resolveProjectPath(existing->source);
    } else {
        state.profileId =
            catalog_.environmentProfiles.count("ibl_desktop_v1") != 0
                ? "ibl_desktop_v1"
                : std::min_element(
                      catalog_.environmentProfiles.begin(),
                      catalog_.environmentProfiles.end(),
                      [](const auto &left, const auto &right) {
                          return left.first < right.first;
                      })
                      ->first;
        state.sourcePath =
            projectContext_.projectRoot / "assets" / "environments" /
            state.environmentId / (state.environmentId + ".hdr");
    }
    state.previousExtent = renderer_->viewportExtent();
    state.temporaryDirectory =
        captureService_->captureRoot() / "probe-capture" /
        state.environmentId;
    for (uint32_t face = 0; face < state.facePaths.size(); ++face) {
        state.faceRelativePaths[face] =
            std::filesystem::path("probe-capture") /
            state.environmentId /
            ("face-" + std::to_string(face) + ".hdr");
        state.facePaths[face] =
            captureService_->captureRoot() /
            state.faceRelativePaths[face];
    }
    state.status = "Resizing viewport for probe capture";
    reflectionProbeCapture_ = std::move(state);
    viewportResize_.desiredWidth =
        reflectionProbeCapture_->faceSize;
    viewportResize_.desiredHeight =
        reflectionProbeCapture_->faceSize;
    viewportResize_.changedAt = std::chrono::steady_clock::now();
    viewportResize_.pending = true;
    viewportResize_.immediate = true;
    editorUi_->sceneStatus = reflectionProbeCapture_->status;
#else
    (void)entityId;
#endif
}

void Application::updateReflectionProbeCapture() {
#if VKL_ENABLE_EDITOR_UI
    if (!reflectionProbeCapture_)
        return;

    const auto fail = [this](const std::string &message) {
        ReflectionProbeCaptureState state =
            std::move(*reflectionProbeCapture_);
        reflectionProbeCapture_.reset();
        if (!state.backupPath.empty() &&
            std::filesystem::exists(state.backupPath)) {
            try {
                publishProbeSource(state.backupPath, state.sourcePath);
            } catch (const std::exception &error) {
                VKR_LOG_ERROR("ReflectionProbe",
                              "Could not restore probe source: {}",
                              error.what());
            }
        }
        if (state.catalogEntryAdded) {
            try {
                SceneCatalogEditor::removeEnvironment(
                    projectContext_, state.environmentId);
                std::error_code ignored;
                std::filesystem::remove(state.sourcePath, ignored);
                refreshSceneRegistry();
            } catch (const std::exception &error) {
                VKR_LOG_ERROR("ReflectionProbe",
                              "Could not roll back probe Catalog entry: {}",
                              error.what());
            }
        }
        viewportResize_.desiredWidth = state.previousExtent.width;
        viewportResize_.desiredHeight = state.previousExtent.height;
        viewportResize_.changedAt = std::chrono::steady_clock::now();
        viewportResize_.pending = true;
        viewportResize_.immediate = true;
        editorUi_->sceneError = message;
        VKR_LOG_ERROR("ReflectionProbe", "Probe capture failed: {}",
                      message);
    };

    try {
        ReflectionProbeCaptureState &state = *reflectionProbeCapture_;
        if (!sceneEditorSession_ || !sceneEditorSession_->active() ||
            !sceneEditorSession_->world()->find(state.entityId)) {
            fail("Reflection probe entity was removed during capture");
            return;
        }

        if (state.phase ==
            ReflectionProbeCapturePhase::AwaitingResize) {
            const VkExtent2D extent = renderer_->viewportExtent();
            if (extent.width != state.faceSize ||
                extent.height != state.faceSize)
                return;
            state.phase = ReflectionProbeCapturePhase::CapturingFaces;
            state.status = "Capturing reflection probe face 1/6";
        }

        if (state.phase ==
            ReflectionProbeCapturePhase::CapturingFaces) {
            if (state.captureTaskId == 0) {
                state.captureTaskId = captureService_->requestHdr(
                    state.faceRelativePaths[state.faceIndex]);
                state.status =
                    "Capturing reflection probe face " +
                    std::to_string(state.faceIndex + 1u) + "/6";
                editorUi_->sceneStatus = state.status;
                return;
            }
            const auto capture =
                captureService_->task(state.captureTaskId);
            if (!capture || !isTerminalCaptureTaskState(capture->state))
                return;
            if (capture->state != CaptureTaskState::Completed) {
                fail(capture->result.error.empty()
                         ? "Reflection probe face capture was cancelled"
                         : capture->result.error);
                return;
            }
            state.captureTaskId = 0;
            ++state.faceIndex;
            if (state.faceIndex < state.facePaths.size())
                return;

            state.status = "Stitching reflection probe HDR";
            editorUi_->sceneStatus = state.status;
            std::filesystem::path temporary = state.sourcePath;
            temporary += ".capture-tmp";
            editor::stitchReflectionProbeFaces(
                state.facePaths, temporary, state.faceSize);

            const bool existed =
                std::filesystem::exists(state.sourcePath);
            if (existed) {
                state.backupPath = state.sourcePath;
                state.backupPath += ".probe-backup";
                std::filesystem::copy_file(
                    state.sourcePath, state.backupPath,
                    std::filesystem::copy_options::overwrite_existing);
            }
            publishProbeSource(temporary, state.sourcePath);

            if (!catalog_.findEnvironment(state.environmentId)) {
                CatalogEnvironment environment;
                environment.id = state.environmentId;
                environment.displayName =
                    "Reflection Probe " +
                    state.entityId.toString().substr(0, 8);
                environment.source = std::filesystem::relative(
                    state.sourcePath, projectContext_.projectRoot);
                environment.environmentProfile = state.profileId;
                SceneCatalogEditor::addEnvironment(projectContext_,
                                                   environment);
                state.catalogEntryAdded = true;
                refreshSceneRegistry();
            }

            const auto bake = assetImportManager_->request(
                {state.environmentId, state.profileId,
                 ImportReason::ManualReimport, true,
                 AssetImportKind::Environment});
            state.bakeTaskId = bake->id;
            state.phase = ReflectionProbeCapturePhase::Baking;
            state.status = "Baking reflection probe environment";
            viewportResize_.desiredWidth = state.previousExtent.width;
            viewportResize_.desiredHeight = state.previousExtent.height;
            viewportResize_.changedAt =
                std::chrono::steady_clock::now();
            viewportResize_.pending = true;
            viewportResize_.immediate = true;
            editorUi_->sceneStatus = state.status;
            return;
        }

        if (state.phase == ReflectionProbeCapturePhase::Baking) {
            const auto bake = assetImportManager_->task(state.bakeTaskId);
            if (!bake || !isTerminalAssetImportState(bake->state.load()))
                return;
            if (bake->state.load() != AssetImportState::Completed) {
                std::string error;
                {
                    std::lock_guard<std::mutex> lock(bake->mutex);
                    error = bake->error;
                }
                fail(error.empty() ?
                         "Reflection probe environment bake failed" :
                         error);
                return;
            }
            reloadArtifactIndex();
            sceneRuntime_->invalidateEnvironment(state.environmentId,
                                                 &state.profileId);
            const std::string environmentId = state.environmentId;
            const PersistentEntityId entityId = state.entityId;
            sceneEditorSession_->execute(
                "Capture Reflection Probe",
                [entityId, environmentId](RuntimeWorld &world) {
                    const auto entity = world.entity(world.find(entityId));
                    if (!entity || !entity->reflectionProbe)
                        return false;
                    ReflectionProbeComponentDocument probe =
                        *entity->reflectionProbe;
                    probe.environmentId = environmentId;
                    return world.setReflectionProbe(world.find(entityId),
                                                    probe);
                });
            state.phase = ReflectionProbeCapturePhase::Loading;
            state.status = "Uploading reflection probe environment";
            editorUi_->sceneStatus = state.status;
            return;
        }

        if (state.phase == ReflectionProbeCapturePhase::Loading) {
            const auto entity = sceneEditorSession_->world()->entity(
                sceneEditorSession_->world()->find(state.entityId));
            if (!entity || !entity->reflectionProbe) {
                fail("Reflection probe entity disappeared during upload");
                return;
            }
            if (entity->reflectionProbeBindingState ==
                ModelBindingState::Failed) {
                fail(entity->reflectionProbeBindingError.empty()
                         ? "Reflection probe upload failed"
                         : entity->reflectionProbeBindingError);
                return;
            }
            if (entity->reflectionProbeBindingState !=
                ModelBindingState::Ready)
                return;

            const std::string environmentId = state.environmentId;
            const std::filesystem::path backup = state.backupPath;
            const std::filesystem::path temporaryDirectory =
                state.temporaryDirectory;
            reflectionProbeCapture_.reset();
            std::error_code ignored;
            if (!backup.empty())
                std::filesystem::remove(backup, ignored);
            std::filesystem::remove_all(temporaryDirectory, ignored);
            editorUi_->sceneStatus =
                "Reflection probe ready: " + environmentId;
            editorUi_->sceneError.clear();
            VKR_LOG_INFO("ReflectionProbe",
                         "Reflection probe {} captured and published as {}",
                         entity->id.toString(), environmentId);
        }
    } catch (const std::exception &error) {
        fail(error.what());
    }
#endif
}

void Application::applyReflectionProbeCaptureView(
    RenderViewInput &input, std::string &cameraIdentity) const {
#if VKL_ENABLE_EDITOR_UI
    if (!reflectionProbeCapture_ ||
        reflectionProbeCapture_->phase !=
            ReflectionProbeCapturePhase::CapturingFaces ||
        !sceneEditorSession_ || !sceneEditorSession_->active())
        return;
    const ReflectionProbeCaptureState &state =
        *reflectionProbeCapture_;
    const auto entity = sceneEditorSession_->world()->entity(
        sceneEditorSession_->world()->find(state.entityId));
    if (!entity || !entity->reflectionProbe || state.faceIndex >= 6)
        return;

    const glm::vec3 position =
        glm::vec3(entity->world *
                  glm::vec4(entity->reflectionProbe->captureOffset, 1.0f));
    input.view = editor::reflectionProbeFaceView(state.faceIndex, position);
    input.projection = glm::perspectiveRH_ZO(
        glm::half_pi<float>(), 1.0f, 0.05f,
        std::max(1000.0f, input.cameraFarPlane));
    input.projection[1][1] *= -1.0f;
    input.cameraPosition = position;
    input.cameraNearPlane = 0.05f;
    input.cameraFarPlane = std::max(1000.0f, input.cameraFarPlane);
    input.viewportExtent = {state.faceSize, state.faceSize};
    input.reflectionProbes = nullptr;
    input.settings.iblEnabled = false;
    input.settings.bloomEnabled = false;
    input.settings.ambientOcclusionMode = AmbientOcclusionMode::Off;
    input.settings.temporalAntiAliasingMode =
        TemporalAntiAliasingMode::Off;
    input.settings.reflectionMode = ReflectionMode::IblOnly;
    input.settings.globalIlluminationMode =
        GlobalIlluminationMode::AmbientOrIbl;
    input.settings.surfaceDebugView = SurfaceDebugView::None;
    input.settings.screenSpaceDebugView = ScreenSpaceDebugView::None;
    cameraIdentity = "reflection-probe:" + state.entityId.toString() +
                     ":" + std::to_string(state.faceIndex);
#else
    (void)input;
    (void)cameraIdentity;
#endif
}

void Application::updateAssetImports() {
    VKL_PROFILE_ZONE("Asset Import Update");
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
        if (task->kind == AssetImportKind::SceneValidation) {
            if (sceneIndex >= 0)
                refreshValidationStatus(sceneIndex);
            const AssetValidationState validationState =
                task->validationState.load();
            if (state == AssetImportState::Completed) {
                sceneAssetOperations_->status =
                    "Validation " + task->sceneId + ": " +
                    assetValidationStateName(validationState);
                sceneAssetOperations_->error.clear();
            } else if (state == AssetImportState::Failed) {
                std::lock_guard<std::mutex> lock(task->mutex);
                sceneAssetOperations_->error =
                    task->error.empty() ? "Scene validation failed"
                                        : task->error;
            } else if (state == AssetImportState::Cancelled) {
                sceneAssetOperations_->status = "Scene validation cancelled";
            }
            continue;
        }
        if (task->kind == AssetImportKind::Environment) {
            if (state == AssetImportState::Completed) {
                reloadArtifactIndex();
                editorUi_->environmentStatus =
                    "Built environment artifacts for " + task->sceneId;
                editorUi_->environmentError.clear();
                if (sceneRuntime_->selectedEnvironmentId() == task->sceneId) {
                    try {
                        setEnvironment(task->sceneId);
                    } catch (const std::exception &error) {
                        editorUi_->environmentError = error.what();
                    }
                }
            } else if (state == AssetImportState::Failed) {
                std::lock_guard<std::mutex> lock(task->mutex);
                editorUi_->environmentError =
                    task->error.empty()
                        ? "Environment bake failed"
                        : task->error;
            } else if (state == AssetImportState::Cancelled) {
                editorUi_->environmentStatus =
                    "Environment bake cancelled";
            }
            continue;
        }
        if (state == AssetImportState::Completed) {
            reloadArtifactIndex();
            sceneRuntime_->invalidateModel(ModelAssetId(task->sceneId),
                                           &task->profileId);
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
    if ((taskId & EnvironmentLoadManager::kTaskIdMask) != 0 &&
        (taskId & AssetImportManager::kTaskIdMask) == 0) {
        return cancelEnvironmentLoad(taskId);
    }
    if ((taskId & AssetImportManager::kTaskIdMask) == 0)
        return cancelSceneLoad(taskId);
    const auto linked = sceneAssetOperations_->importToLoadTask.find(taskId);
    if (linked != sceneAssetOperations_->importToLoadTask.end() &&
        linked->second != 0)
        return cancelSceneLoad(linked->second);
    return assetImportManager_ && assetImportManager_->cancel(taskId);
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
    if (assetImportManager_) {
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
    if ((taskId & AssetImportManager::kTaskIdMask) != 0) {
        result =
            loadOperationToJson(assetImportManager_->task(taskId), nullptr);
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
        index, requestSceneOperation(index, false, true,
                                     ImportReason::SceneLoad, false, true));
}

ControlJson
Application::runtimeLoadStatus(std::optional<uint64_t> requestedTaskId) {
    uint64_t taskId = requestedTaskId.value_or(0);
    if (taskId == 0) {
        const auto activeImport =
            assetImportManager_ ? assetImportManager_->activeTask()
                                : std::shared_ptr<AssetImportTask>{};
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
        (taskId & AssetImportManager::kTaskIdMask) == 0) {
        const auto task = sceneRuntime_->environmentLoadTask(taskId);
        if (!task)
            throw RuntimeCommandError("load_not_found",
                                      "Load task was not found.");
        return environmentLoadTaskToJson(task);
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
            loadTask = sceneRuntime_->sceneLoadTask(linked->second);
        return loadOperationToJson(importTask, loadTask);
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
    const auto activeImport =
        assetImportManager_ ? assetImportManager_->activeTask()
                            : std::shared_ptr<AssetImportTask>{};
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
    if ((taskId & AssetImportManager::kTaskIdMask) != 0) {
        result["loadTask"] =
            loadOperationToJson(assetImportManager_->task(taskId), nullptr);
        result["taskId"] = taskId;
    } else if (sceneRuntime_->latestSceneLoadTask() &&
               !isTerminalSceneLoadState(sceneRuntime_->latestSceneLoadTask()->state.load())) {
        result["loadTask"] = sceneLoadTaskToJson(sceneRuntime_->latestSceneLoadTask());
    } else if (sceneRuntime_->lastSceneLoadStats()) {
        result["loadStats"] = sceneLoadStatsToJson(*sceneRuntime_->lastSceneLoadStats());
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
    if (index >= 0 && sceneRegistry_[index].isModelPreview())
        return index;
    index = sceneWorkflow_->findEntryById(name);
    return index >= 0 && sceneRegistry_[index].isModelPreview() ? index : -1;
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
            {"catalog", projectContext_.catalogPath.u8string()},
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

    refreshValidationStatus(index);
    const auto found =
        sceneAssetOperations_->validationStatuses.find(entry.id);
    if (found == sceneAssetOperations_->validationStatuses.end())
        throw RuntimeCommandError("validation_unavailable",
                                  "Validation status is unavailable.");
    ControlJson result = validationQueryToJson(found->second, 32);
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
#endif
}

ControlJson Application::runtimeAssetCacheInfo() {
    const std::filesystem::path root =
        std::filesystem::u8path(
            sceneLoadContext_.derivedTextureCachePath);
    if (artifactIndex_)
        artifactUsage_ = artifactIndex_->usage();
    const ArtifactIndexUsage usage =
        artifactUsage_.value_or(ArtifactIndexUsage{});
    return {{"root", root.u8string()},
            {"index", artifactIndex_ ? artifactIndex_->path().u8string() : ""},
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
    ControlJson entries = ControlJson::array();
    for (const auto &variant : shaderRegistry_.variants()) {
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
    const ShaderVariant *variant = shaderRegistry_.findVariant(name);
    if (!variant) {
        ControlJson candidates = ControlJson::array();
        for (const auto &candidate : shaderRegistry_.variants()) {
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
    if (assetImportManager_) {
        const auto importHistory = assetImportManager_->history();
        const auto activeImport = std::find_if(
            importHistory.rbegin(), importHistory.rend(),
            [](const std::shared_ptr<AssetImportTask> &task) {
                return task &&
                       !isTerminalAssetImportState(task->state.load());
            });
        if (activeImport != importHistory.rend()) {
            std::shared_ptr<SceneLoadTask> linkedLoad;
            const auto linked = sceneAssetOperations_->importToLoadTask.find(
                (*activeImport)->id);
            if (linked != sceneAssetOperations_->importToLoadTask.end() &&
                linked->second != 0) {
                linkedLoad = sceneRuntime_->sceneLoadTask(linked->second);
            }
            loadTask = loadOperationToJson(*activeImport, linkedLoad);
        }
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
    if (gui_) {
        viewportDisplayWidth = viewportDisplayWidth_;
        viewportDisplayHeight = viewportDisplayHeight_;
        viewportVisible = viewportVisible_;
        viewportHovered = viewportHovered_;
        viewportResizePending = viewportResize_.pending;
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
          {"maxPointShadowLights", renderSettings_.maxPointShadowLights},
          {"maxSpotShadowLights", renderSettings_.maxSpotShadowLights},
          {"pointShadowDistance", renderSettings_.pointShadowDistance},
          {"spotShadowDistance", renderSettings_.spotShadowDistance},
          {"pointShadowReceiverBiasWorld",
           renderSettings_.pointShadowReceiverBiasWorld},
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
           renderSettings_.culling.occlusionMinCandidates},
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
                            renderSettings_.surfaceDebugView)},
          {"motionDebugScale", renderSettings_.surfaceMotionDebugScale},
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
                            renderSettings_.screenSpaceDebugView)},
          {"debugMip", renderSettings_.screenSpaceDebugMip},
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
    const OcclusionCullingStatus cullingStatus =
        renderer_->occlusionCullingStatus();
    const ScreenSpaceEffectsStatus screenSpaceStatus =
        renderer_->screenSpaceEffectsStatus();
    return {{"shadowsEnabled", renderSettings_.shadowsEnabled},
            {"shadowMapSize", kDirectionalShadowMapSize},
            {"shadowReceiverBias", renderSettings_.shadowReceiverBias},
            {"pointShadowReceiverBiasWorld",
             renderSettings_.pointShadowReceiverBiasWorld},
            {"shadowConstantBias", renderSettings_.shadowConstantBias},
            {"shadowSlopeBias", renderSettings_.shadowSlopeBias},
            {"maxPointShadowLights",
             renderSettings_.maxPointShadowLights},
            {"maxSpotShadowLights",
             renderSettings_.maxSpotShadowLights},
            {"pointShadowDistance",
             renderSettings_.pointShadowDistance},
            {"spotShadowDistance",
             renderSettings_.spotShadowDistance},
            {"exposureEv", renderSettings_.exposureEv},
            {"toneMapper", toneMapperName(renderSettings_.toneMapper)},
            {"bloomEnabled", renderSettings_.bloomEnabled},
            {"bloomThreshold", renderSettings_.bloomThreshold},
            {"bloomSoftKnee", renderSettings_.bloomSoftKnee},
            {"bloomIntensity", renderSettings_.bloomIntensity},
            {"bloomAvailable", renderer_->bloomSupported()},
            {"bloomActive",
             renderer_->bloomSupported() &&
                 renderSettings_.bloomEnabled &&
                 currentShaderVariant().supportsBloom},
            {"bloomUnavailableReason",
             renderer_->bloomSupported()
                 ? std::string{}
                 : renderer_->bloomUnsupportedReason()},
            {"iblEnabled", renderSettings_.iblEnabled},
            {"skyboxEnabled", renderSettings_.skyboxEnabled},
            {"environmentIntensity",
             renderSettings_.environmentIntensity},
            {"environmentRotationRadians",
             renderSettings_.environmentRotationRadians},
            {"surfaceDebugView",
             surfaceDebugViewName(renderSettings_.surfaceDebugView)},
            {"surfaceMotionDebugScale",
             renderSettings_.surfaceMotionDebugScale},
            {"surfaceDataAvailable",
             renderer_->surfaceDataStatus().supported},
            {"surfaceDataActive", renderer_->surfaceDataStatus().active},
            {"surfaceDataUnavailableReason",
             renderer_->surfaceDataStatus().unavailableReason},
            {"ambientOcclusionMode",
             ambientOcclusionModeName(
                 renderSettings_.ambientOcclusionMode)},
            {"ssaoQuality",
             ssaoQualityName(renderSettings_.ssaoQuality)},
            {"ssaoRadius", renderSettings_.ssaoRadius},
            {"ssaoBias", renderSettings_.ssaoBias},
            {"ssaoIntensity", renderSettings_.ssaoIntensity},
            {"ssaoPower", renderSettings_.ssaoPower},
            {"ssaoAvailable", screenSpaceStatus.ssaoSupported},
            {"ssaoActive",
             screenSpaceStatus.activeMode == AmbientOcclusionMode::Ssao},
             {"ssaoUnavailableReason",
              screenSpaceStatus.ssaoUnavailableReason},
             {"cacaoQuality",
              cacaoQualityName(renderSettings_.cacao.quality)},
             {"cacaoResolution",
              cacaoResolutionName(renderSettings_.cacao.resolution)},
             {"cacaoRadius", renderSettings_.cacao.radius},
             {"cacaoIntensity", renderSettings_.cacao.intensity},
             {"cacaoPower", renderSettings_.cacao.power},
             {"cacaoCompiled", screenSpaceStatus.cacaoCompiled},
             {"cacaoAvailable", screenSpaceStatus.cacaoInitialized},
             {"cacaoActive",
              screenSpaceStatus.activeMode == AmbientOcclusionMode::Cacao},
             {"cacaoFp32", screenSpaceStatus.cacaoFp32},
             {"cacaoGeneration", screenSpaceStatus.cacaoGeneration},
              {"cacaoUnavailableReason",
               screenSpaceStatus.cacaoUnavailableReason},
             {"gtaoQuality",
              gtaoQualityName(renderSettings_.gtao.quality)},
             {"gtaoRadius", renderSettings_.gtao.radius},
             {"gtaoFalloff", renderSettings_.gtao.falloff},
             {"gtaoIntensity", renderSettings_.gtao.intensity},
             {"gtaoPower", renderSettings_.gtao.power},
             {"gtaoTemporalWeight", renderSettings_.gtao.temporalWeight},
             {"gtaoAvailable", screenSpaceStatus.gtaoSupported},
             {"gtaoActive", screenSpaceStatus.gtaoActive},
             {"gtaoHistoryValid", screenSpaceStatus.gtaoHistoryValid},
             {"gtaoHistoryGeneration",
              screenSpaceStatus.gtaoHistoryGeneration},
             {"gtaoLastResetReason",
              screenSpaceStatus.gtaoLastResetReason},
             {"gtaoUnavailableReason",
              screenSpaceStatus.gtaoUnavailableReason},
            {"temporalAntiAliasingMode",
             temporalAntiAliasingModeName(
                 renderSettings_.temporalAntiAliasingMode)},
            {"taaHistoryWeight", renderSettings_.taaHistoryWeight},
            {"taaSharpness", renderSettings_.taaSharpness},
            {"taaAvailable", screenSpaceStatus.taaSupported},
            {"taaActive", screenSpaceStatus.taaActive},
            {"taaHistoryValid", screenSpaceStatus.taaHistoryValid},
            {"taaHistoryGeneration",
             screenSpaceStatus.taaHistoryGeneration},
            {"taaLastResetReason",
             screenSpaceStatus.taaLastResetReason},
            {"taaUnavailableReason",
             screenSpaceStatus.taaUnavailableReason},
            {"reflectionMode",
             reflectionModeName(renderSettings_.reflectionMode)},
            {"ssrQuality", ssrQualityName(renderSettings_.ssrQuality)},
            {"ssrMaxDistance", renderSettings_.ssrMaxDistance},
            {"ssrThickness", renderSettings_.ssrThickness},
            {"ssrMaxRoughness", renderSettings_.ssrMaxRoughness},
            {"ssrIntensity", renderSettings_.ssrIntensity},
            {"ssrHistoryWeight", renderSettings_.ssrHistoryWeight},
            {"ssrAvailable", screenSpaceStatus.ssrSupported},
            {"ssrActive", screenSpaceStatus.ssrActive},
            {"ssrHistoryValid", screenSpaceStatus.ssrHistoryValid},
            {"ssrHistoryGeneration",
             screenSpaceStatus.ssrHistoryGeneration},
            {"ssrLastResetReason",
             screenSpaceStatus.ssrLastResetReason},
            {"ssrUnavailableReason",
             screenSpaceStatus.ssrUnavailableReason},
            {"globalIlluminationMode",
             globalIlluminationModeName(
                 renderSettings_.globalIlluminationMode)},
            {"ssgiQuality", ssgiQualityName(renderSettings_.ssgiQuality)},
            {"ssgiMaxDistance", renderSettings_.ssgiMaxDistance},
            {"ssgiThickness", renderSettings_.ssgiThickness},
            {"ssgiIntensity", renderSettings_.ssgiIntensity},
            {"ssgiRadianceClamp", renderSettings_.ssgiRadianceClamp},
            {"ssgiHistoryWeight", renderSettings_.ssgiHistoryWeight},
            {"ssgiAvailable", screenSpaceStatus.ssgiSupported},
            {"ssgiActive", screenSpaceStatus.ssgiActive},
            {"ssgiHistoryValid", screenSpaceStatus.ssgiHistoryValid},
            {"ssgiHistoryGeneration",
             screenSpaceStatus.ssgiHistoryGeneration},
            {"ssgiLastResetReason",
             screenSpaceStatus.ssgiLastResetReason},
            {"ssgiUnavailableReason",
             screenSpaceStatus.ssgiUnavailableReason},
            {"ddgiRadianceClamp", renderSettings_.ddgi.radianceClamp},
            {"ddgiDebugView",
             ddgiDebugViewName(renderSettings_.ddgi.debugView)},
            {"ddgiSupported", renderer_->ddgiStatus().supported},
            {"ddgiComponentPresent",
             renderer_->ddgiStatus().componentPresent},
            {"ddgiActive", renderer_->ddgiStatus().active},
            {"ddgiUnavailableReason",
             renderer_->ddgiStatus().unavailableReason},
            {"screenSpaceDebugView",
             screenSpaceDebugViewName(
                 renderSettings_.screenSpaceDebugView)},
            {"screenSpaceDebugMip",
             renderSettings_.screenSpaceDebugMip},
            {"frustumCullingEnabled",
             renderSettings_.culling.frustumEnabled},
            {"shadowCullingEnabled",
             renderSettings_.culling.shadowCullingEnabled},
            {"shadowDistance", renderSettings_.culling.shadowDistance},
            {"distanceCullingEnabled",
             renderSettings_.culling.distanceEnabled},
            {"maxDrawDistance",
             renderSettings_.culling.maxDrawDistance},
            {"smallObjectCullingEnabled",
             renderSettings_.culling.smallObjectEnabled},
            {"minProjectedSizePixels",
             renderSettings_.culling.minProjectedSizePixels},
            {"occlusionCullingEnabled",
             renderSettings_.culling.occlusionEnabled},
            {"occlusionMinCandidates",
             renderSettings_.culling.occlusionMinCandidates},
            {"occlusionDepthBias",
             renderSettings_.culling.occlusionDepthBias},
            {"occlusionAvailable", cullingStatus.supported},
            {"occlusionActive", cullingStatus.active},
            {"occlusionUnavailableReason",
             cullingStatus.unavailableReason},
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
    ImGuiIO *io = gui_ ? &ImGui::GetIO() : nullptr;
    const bool activeSceneCamera =
        sceneEditorSession_ && sceneEditorSession_->active() &&
        sceneEditorSession_->cameraMode() == EditorCameraMode::ActiveScene;
    if (activeSceneCamera) {
        if (mode_ == InputMode::CameraDrag) {
            input_->setCursorCaptured(false);
            input_->setCursorPos(savedCursor_);
            if (io)
                io->ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
            mode_ = InputMode::UI;
        }
        return;
    }
#endif

    if (mode_ == InputMode::UI) {
        const bool pressed = input_->isMousePressed(MouseButton::Right);
        bool overUI = gui_ && gui_->wantCaptureMouse();
#if VKL_ENABLE_EDITOR_UI
        const bool overSceneArea = !gui_ || !editorDockWorkspace_ ||
                                   editorDockWorkspace_->viewportState().hovered;
        if (overSceneArea)
            overUI = false;
        overUI = overUI || (io && ImGui::IsAnyItemActive());
        overUI = overUI ||
                 (sceneViewportController_ &&
                  sceneViewportController_->blocksViewportInput());
#else
        constexpr bool overSceneArea = true;
#endif
        if (pressed && overSceneArea && !overUI) {
            savedCursor_ = input_->cursorPos();
            input_->setCursorCaptured(true);
#if VKL_ENABLE_EDITOR_UI
            if (io)
                io->ConfigFlags |= ImGuiConfigFlags_NoMouse;
#endif
            mode_ = InputMode::CameraDrag;
        }
    } else { // CameraDrag
        if (input_->isMouseReleased(MouseButton::Right)) {
            input_->setCursorCaptured(false);
            input_->setCursorPos(savedCursor_);
#if VKL_ENABLE_EDITOR_UI
            if (io)
                io->ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
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

void Application::refreshSceneRegistry(const std::string &selectSceneId) {
    std::unordered_map<std::string, std::string> previousModelSources;
    for (const SceneEntry &entry : sceneRegistry_) {
        if (entry.prepareFactory)
            previousModelSources.emplace(entry.id, entry.sourcePath);
    }
    std::string selectedId = selectSceneId;
    if (selectedId.empty() && sceneAssetOperations_ &&
        sceneAssetOperations_->selectedSceneIndex >= 0 &&
        sceneAssetOperations_->selectedSceneIndex <
            static_cast<int>(sceneRegistry_.size())) {
        selectedId =
            sceneRegistry_[sceneAssetOperations_->selectedSceneIndex].id;
    }

    sceneWorkflow_->refresh(projectContext_);
    for (const auto &[modelId, sourcePath] : previousModelSources) {
        const auto found = std::find_if(
            sceneRegistry_.begin(), sceneRegistry_.end(),
            [&](const SceneEntry &entry) { return entry.id == modelId; });
        if (found == sceneRegistry_.end() ||
            found->sourcePath != sourcePath) {
            sceneRuntime_->invalidateModel(ModelAssetId(modelId));
        }
    }
    reloadArtifactIndex();
    sceneRuntime_->remapCurrentSceneIndex();
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
    refreshAllValidationStatuses();
}

#if VKL_ENABLE_EDITOR_UI
void Application::updateModelImport() {
#if !VKL_ENABLE_ASSET_AUTHORING
    return;
#else
    ModelImportUiState &ui = *modelImportUi_;
    if (ui.validationTask &&
        isTerminalAssetImportState(ui.validationTask->state.load())) {
        const std::shared_ptr<AssetImportTask> task = ui.validationTask;
        ui.validationTask.reset();
        try {
            std::filesystem::path reportPath;
            std::string taskError;
            {
                std::lock_guard<std::mutex> lock(task->mutex);
                reportPath = std::filesystem::u8path(task->manifestPath);
                taskError = task->error;
            }
            if (reportPath.empty()) {
                throw std::runtime_error(
                    taskError.empty() ? "Validator produced no report"
                                      : taskError);
            }
            AssetValidationReport report;
            std::string reportError;
            if (!loadAssetValidationReport(reportPath, report, reportError)) {
                throw std::runtime_error("Could not load validation report: " +
                                         reportError);
            }
            ui.preflight = ModelImportService::preflight(task->sourcePath);
            ui.validationReport = std::move(report);
            ui.validationReportPath = std::move(reportPath);
            std::snprintf(ui.displayName.data(), ui.displayName.size(), "%s",
                          ui.preflight->suggestedDisplayName.c_str());
            std::snprintf(ui.modelId.data(), ui.modelId.size(), "%s",
                          ui.preflight->suggestedModelId.c_str());
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
            ui.allowUnvalidated = false;
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
            const ModelImportResult result = ui.importFuture.get();
            const bool loadAfter = ui.loadAfterActiveImport;
            ui.worker.reset();
            ui.preflight.reset();
            ui.validationReport.reset();
            ui.validationReportPath.clear();
            ui.status = "Imported " + result.model.displayName;
            ui.error.clear();
            refreshSceneRegistry(result.model.id);
            const ImportProfile &profile =
                catalog_.profile(result.model.importProfile);
            sceneLoadContext_.maxTextureSize = profile.textureLimit;
            for (int i = 0; i < static_cast<int>(sceneRegistry_.size()); ++i) {
                if (sceneRegistry_[i].id == result.model.id) {
                    requestSceneOperation(
                        i, false, loadAfter,
                        ImportReason::SceneRegistration, false);
                    ui.status = "Registered " + result.model.displayName +
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
#endif
}

void Application::drawScenePanel(bool modelsOnly) {
    ModelImportUiState &ui = *modelImportUi_;

    if (scenesPanel_) {
        SceneWorkflowSnapshot snapshot = sceneWorkflow_->snapshot();
        snapshot.showImport =
            config_.assetImportMode != AssetImportMode::CookedOnly;
#if VKL_ENABLE_ASSET_AUTHORING
        snapshot.busy =
            (ui.validationTask &&
             !isTerminalAssetImportState(ui.validationTask->state.load())) ||
            ui.importFuture.valid();
        snapshot.canImport =
            config_.assetImportMode == AssetImportMode::OnDemand &&
            projectContext_.catalogWritable;
#endif
        snapshot.status = ui.status;
        if (!sceneAssetOperations_->status.empty()) {
            if (!snapshot.status.empty())
                snapshot.status += " | ";
            snapshot.status += sceneAssetOperations_->status;
        }
        snapshot.error = ui.error;
        if (!sceneAssetOperations_->error.empty()) {
            if (!snapshot.error.empty())
                snapshot.error += " | ";
            snapshot.error += sceneAssetOperations_->error;
        }
        if (ui.worker) {
            const uint64_t total = ui.worker->totalBytes.load();
            const uint64_t completed = ui.worker->completedBytes.load();
            snapshot.copyActive = true;
            snapshot.copyProgress =
                total == 0
                    ? 0.0f
                    : static_cast<float>(completed) /
                          static_cast<float>(total);
            std::lock_guard<std::mutex> lock(ui.worker->mutex);
            snapshot.copyFile = ui.worker->currentFile;
        }
        snapshot.openImportDialog = modelsOnly && ui.requestOpenModal;
        if (modelsOnly)
            ui.requestOpenModal = false;
        snapshot.importPreflight = ui.preflight;
        snapshot.importValidation = ui.validationReport;
        snapshot.importValidationReportPath = ui.validationReportPath;
        snapshot.importProfileIds = ui.profileIds;
        snapshot.defaultImportProfileIndex = ui.profileIndex;
        snapshot.defaultReferenceExisting = ui.referenceExisting;
        snapshot.defaultLoadAfterImport = ui.loadAfterImport;

        const bool canInstantiatePrimitive =
            sceneEditorSession_ && sceneEditorSession_->active() &&
            projectContext_.catalogWritable;
        for (const PrimitiveModelDefinition &primitive :
             primitiveModelDefinitions()) {
            snapshot.enginePrimitives.push_back(
                {std::string(primitive.id),
                 std::string(primitive.displayName),
                 canInstantiatePrimitive});
        }

        for (SceneWorkflowItemSnapshot &item : snapshot.models) {
            if (item.index < 0 ||
                item.index >= static_cast<int>(sceneRegistry_.size()))
                continue;
            const SceneEntry &entry = sceneRegistry_[item.index];
            item.profileId = profileIdForTextureLimit(entry);
            const auto profile = catalog_.importProfiles.find(item.profileId);
            if (profile != catalog_.importProfiles.end())
                item.encoder = profile->second.textureEncoder;
            const auto artifact = sceneAssetOperations_->statuses.find(
                artifactStatusKey(entry.id, item.profileId));
            if (artifact != sceneAssetOperations_->statuses.end()) {
                item.artifactState = artifactStateName(artifact->second.state);
                item.artifactReason = artifact->second.reason;
            }
            const auto validation =
                sceneAssetOperations_->validationStatuses.find(entry.id);
            if (validation !=
                sceneAssetOperations_->validationStatuses.end()) {
                item.validationState =
                    assetValidationStateName(validation->second.state);
                item.validationReason = validation->second.reason;
                item.validationReportPath = validation->second.reportPath;
            }
            item.current = item.index == sceneRuntime_->currentSceneIndex();
            item.canAuthor =
                config_.assetImportMode == AssetImportMode::OnDemand;
            item.canLoadSource =
                config_.assetImportMode != AssetImportMode::CookedOnly;
            item.canEditCatalog =
                projectContext_.catalogWritable &&
                config_.assetImportMode == AssetImportMode::OnDemand;
            const CatalogModel *catalogModel =
                catalog_.findModel(entry.id);
            item.canInstantiate =
                sceneEditorSession_ && sceneEditorSession_->active() &&
                projectContext_.catalogWritable && item.available &&
                catalogModel != nullptr;
        }
        for (SceneWorkflowItemSnapshot &item : snapshot.nativeScenes)
            item.current = item.index == sceneRuntime_->currentSceneIndex();

        SceneWorkflowActions actions;
#if VKL_ENABLE_ASSET_AUTHORING
        actions.beginModelImport = [this, &ui] {
            try {
                const auto selected =
                    openGltfFileDialog(window_->nativeHandle());
                if (!selected)
                    return;
                ui.error.clear();
                ui.preflight.reset();
                ui.validationReport.reset();
                ui.validationReportPath.clear();
                AssetImportRequest request;
                request.kind = AssetImportKind::SceneValidation;
                request.profileId = "validation";
                request.sourcePath = *selected;
                ui.validationTask = assetImportManager_->request(request);
                ui.status = "Validating model and local dependencies...";
            } catch (const std::exception &error) {
                ui.error = error.what();
            }
        };
#endif
        actions.refresh = [this, &ui] {
            try {
                refreshSceneRegistry();
                ui.status = "Catalog refreshed";
                ui.error.clear();
            } catch (const std::exception &error) {
                ui.error = error.what();
            }
        };
        actions.selectModel = [this](int index) {
            sceneAssetOperations_->selectedSceneIndex = index;
        };
        actions.loadPreview = [this](int index) {
            try {
                requestEditorSceneLoad(index);
            } catch (const std::exception &error) {
                sceneAssetOperations_->error = error.what();
            }
        };
        actions.loadSceneDocument = [this](int index) {
            try {
                requestEditorSceneLoad(index);
            } catch (const std::exception &error) {
                sceneAssetOperations_->error = error.what();
            }
        };
        actions.reimportModel = [this](int index) {
            try {
                requestSceneOperation(index, false, false,
                                      ImportReason::ManualReimport, true);
            } catch (const std::exception &error) {
                sceneAssetOperations_->error = error.what();
            }
        };
        actions.validateModel = [this](int index) {
            try {
                const SceneEntry &entry = sceneRegistry_.at(index);
                AssetImportRequest request;
                request.sceneId = entry.id;
                request.profileId = "validation";
                request.kind = AssetImportKind::SceneValidation;
                request.force = true;
                const auto task = assetImportManager_->request(request);
                sceneAssetOperations_->status =
                    "Validating " + entry.name + " (task " +
                    std::to_string(task->id) + ")";
                sceneAssetOperations_->error.clear();
            } catch (const std::exception &error) {
                sceneAssetOperations_->error = error.what();
            }
        };
        actions.loadSourceFallback = [this](int index) {
            try {
                requestSceneOperation(index, true);
            } catch (const std::exception &error) {
                sceneAssetOperations_->error = error.what();
            }
        };
        actions.savePreviewCamera = [this](int index) {
            try {
                const SceneEntry &entry = sceneRegistry_.at(index);
                SceneCatalogEditor::saveModelPreviewCamera(
                    projectContext_, entry.id,
                    CameraPose{camera_.position(), camera_.yaw(),
                               camera_.pitch()});
                sceneAssetOperations_->status =
                    "Saved preview camera for " + entry.name;
                refreshSceneRegistry(entry.id);
            } catch (const std::exception &error) {
                sceneAssetOperations_->error = error.what();
            }
        };
        actions.removeModel = [this](int index) {
            try {
                const SceneEntry entry = sceneRegistry_.at(index);
                SceneCatalogEditor::removeModel(projectContext_, entry.id);
                sceneAssetOperations_->status =
                    "Removed " + entry.name + " from Catalog";
                refreshSceneRegistry();
            } catch (const std::exception &error) {
                sceneAssetOperations_->error = error.what();
            }
        };
        actions.openReport = [](const std::filesystem::path &path) {
            if (!path.empty())
                ShellExecuteW(nullptr, L"open", path.c_str(), nullptr,
                              nullptr, SW_SHOWNORMAL);
        };
        actions.cancelImport = [&ui] {
            if (ui.worker)
                ui.worker->cancel = true;
        };
#if VKL_ENABLE_ASSET_AUTHORING
        actions.confirmImport = [this, &ui](
                                    const ModelImportPanelSubmission &input) {
            if (!ui.preflight || !ui.validationReport)
                return;
            ModelImportRequest request;
            request.sourcePath = ui.preflight->sourcePath;
            request.displayName = input.displayName;
            request.modelId = input.modelId;
            request.profileId = input.profileId;
            request.placement =
                input.referenceExisting
                    ? ModelImportPlacement::ReferenceExisting
                    : ModelImportPlacement::CopyIntoProject;
            request.validation =
                sceneValidationReceipt(*ui.validationReport);
            request.allowUnvalidated = input.allowUnvalidated;
            ui.loadAfterActiveImport = input.loadAfterImport;
            ui.worker = std::make_shared<ModelImportWorkerState>();
            ui.worker->totalBytes = ui.preflight->totalBytes;
            const auto worker = ui.worker;
            ProjectContext project = projectContext_;
            project.cacheRoot = std::filesystem::u8path(
                sceneLoadContext_.derivedTextureCachePath);
            ui.importFuture = std::async(
                std::launch::async, [project, request, worker] {
                    profileSetThreadName("ModelImportCopy");
                    VKL_PROFILE_ZONE("Model Catalog Import");
                    return ModelImportService::importModel(
                        project, request,
                        [worker] { return worker->cancel.load(); },
                        [worker](const ModelImportProgress &progress) {
                            worker->completedBytes = progress.completedBytes;
                            worker->totalBytes = progress.totalBytes;
                            std::lock_guard<std::mutex> lock(worker->mutex);
                            worker->currentFile = progress.currentFile;
                        });
                });
            ui.status = "Importing model source files...";
            ui.error.clear();
        };
#endif
        actions.dismissImport = [&ui] {
            ui.preflight.reset();
            ui.validationReport.reset();
            ui.validationReportPath.clear();
            ui.allowUnvalidated = false;
        };
        scenesPanel_->draw(snapshot, actions, modelsOnly);
        return;
    }
}

void Application::drawAssetsPanel(bool environmentsOnly) {
    if (assetsPanel_) {
        AssetsPanelSnapshot snapshot;
        snapshot.projectId = catalog_.projectId;
        snapshot.mode = assetImportModeName(config_.assetImportMode);
        snapshot.catalogPath = projectContext_.catalogPath.string();
        snapshot.cachePath = sceneLoadContext_.derivedTextureCachePath;
        snapshot.authoringCompiled = build::kAssetAuthoring;
        if (artifactUsage_) {
            snapshot.hasUsage = true;
            snapshot.indexRecords = artifactUsage_->records;
            snapshot.readyRecords = artifactUsage_->readyRecords;
            snapshot.cacheBlobFiles = artifactUsage_->cacheBlobFiles;
            snapshot.cacheBlobBytes = artifactUsage_->cacheBlobBytes;
            snapshot.unreferencedBlobFiles =
                artifactUsage_->unreferencedBlobFiles;
            snapshot.unreferencedBlobBytes =
                artifactUsage_->unreferencedBlobBytes;
        }

        const int selectedModel =
            sceneAssetOperations_->selectedSceneIndex;
        if (selectedModel >= 0 &&
            selectedModel < static_cast<int>(sceneRegistry_.size()) &&
            sceneRegistry_[selectedModel].isModelPreview()) {
            const SceneEntry &entry = sceneRegistry_[selectedModel];
            const std::string profileId =
                profileIdForTextureLimit(entry);
            AssetArtifactSnapshot artifact;
            artifact.modelName = entry.name;
            artifact.profileId = profileId;
            const auto profile = catalog_.importProfiles.find(profileId);
            if (profile != catalog_.importProfiles.end())
                artifact.encoder = profile->second.textureEncoder;
            const auto found = sceneAssetOperations_->statuses.find(
                artifactStatusKey(entry.id, profileId));
            if (found != sceneAssetOperations_->statuses.end()) {
                const ArtifactStatus &status = found->second;
                artifact.state = artifactStateName(status.state);
                artifact.reason = status.reason;
                artifact.payloadKind = status.payloadKind;
                artifact.entryCount = status.entryCount;
                artifact.blobBytes = status.blobBytes;
            } else {
                artifact.state = "Unknown";
            }
            if (artifactIndex_) {
                const auto record = artifactIndex_->records().find(
                    artifactIndexKey(entry.id, profileId));
                if (record != artifactIndex_->records().end()) {
                    artifact.failureCode = record->second.failureCode;
                    artifact.failureMessage = record->second.failureMessage;
                }
            }
            snapshot.selectedModel = std::move(artifact);
        }

        for (const CatalogEnvironment &environment : catalog_.environments) {
            EnvironmentAssetSnapshot item;
            item.id = environment.id;
            item.displayName = environment.displayName;
            item.source = environment.source.generic_string();
            item.profileId = environment.environmentProfile;
            item.artifactState = "Unknown";
            if (!projectContext_.cookedPackage) {
                try {
                    const ArtifactStatus status = inspectEnvironmentArtifacts(
                        {std::filesystem::u8path(
                             config_.derivedTextureCachePath),
                         projectContext_.resolveProjectPath(environment.source),
                         catalog_.projectId, environment.id,
                         environment.environmentProfile});
                    item.artifactState = artifactStateName(status.state);
                    item.artifactReason = status.reason;
                    item.entryCount = status.entryCount;
                    item.blobBytes = status.blobBytes;
                    item.ready = status.ready();
                } catch (const std::exception &error) {
                    item.artifactState = "Invalid";
                    item.artifactReason = error.what();
                }
            }
            snapshot.environments.push_back(std::move(item));
        }
        snapshot.canEditEnvironments =
            projectContext_.catalogWritable &&
            config_.assetImportMode == AssetImportMode::OnDemand;
        snapshot.canBuildEnvironments =
            assetImportManager_ && !projectContext_.cookedPackage;
        snapshot.environmentStatus = editorUi_->environmentStatus;
        snapshot.environmentError = editorUi_->environmentError;

#if VKL_ENABLE_ASSET_AUTHORING
        const auto taskSnapshot = [](const std::shared_ptr<AssetImportTask> &task) {
            AssetTaskSnapshot result;
            if (!task)
                return result;
            result.id = task->id;
            result.kind = assetImportKindName(task->kind);
            result.assetId = task->sceneId;
            result.profileId = task->profileId;
            result.state = assetImportStateName(task->state.load());
            result.completed = task->completedArtifacts.load();
            result.total = task->totalArtifacts.load();
            result.encoded = task->encodedArtifacts.load();
            result.reused = task->reusedArtifacts.load();
            result.failed = task->failedArtifacts.load();
            result.workers = task->workers.load();
            result.elapsedSeconds =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - task->requestedAt)
                    .count();
            result.terminal =
                isTerminalAssetImportState(task->state.load());
            result.logPath = task->logPath;
            {
                std::lock_guard<std::mutex> lock(task->mutex);
                result.error = task->error;
                if (task->kind == AssetImportKind::SceneValidation)
                    result.reportPath =
                        std::filesystem::u8path(task->manifestPath);
            }
            return result;
        };
        if (assetImportManager_) {
            if (const auto active = assetImportManager_->activeTask())
                snapshot.activeTask = taskSnapshot(active);
            const auto history = assetImportManager_->history();
            const size_t count = std::min<size_t>(history.size(), 8);
            for (size_t index = 0; index < count; ++index)
                snapshot.recentTasks.push_back(taskSnapshot(history[index]));
        }
#endif

        AssetsPanelActions actions;
#if VKL_ENABLE_ASSET_AUTHORING
        actions.chooseEnvironment = [this]()
            -> std::optional<EnvironmentImportDefaults> {
            try {
                const auto source =
                    openHdrFileDialog(window_->nativeHandle());
                if (!source)
                    return std::nullopt;
                EnvironmentImportDefaults result;
                result.source = *source;
                result.displayName = source->stem().string();
                result.environmentId =
                    ModelImportService::suggestModelId(result.displayName);
                for (const auto &profile : catalog_.environmentProfiles)
                    result.profileIds.push_back(profile.first);
                std::sort(result.profileIds.begin(), result.profileIds.end());
                editorUi_->environmentError.clear();
                return result;
            } catch (const std::exception &error) {
                editorUi_->environmentError = error.what();
                return std::nullopt;
            }
        };
        actions.importEnvironment =
            [this](const EnvironmentImportSubmission &input) {
                try {
                    const std::filesystem::path relative =
                        std::filesystem::path("assets/environments") /
                        input.environmentId / input.source.filename();
                    const std::filesystem::path destination =
                        (projectContext_.projectRoot / relative)
                            .lexically_normal();
                    if (!pathIsWithin(projectContext_.projectRoot,
                                      destination))
                        throw std::runtime_error(
                            "Environment destination escapes project root");
                    if (catalog_.findEnvironment(input.environmentId))
                        throw std::runtime_error(
                            "Environment ID already exists in the Catalog");
                    if (std::filesystem::exists(destination))
                        throw std::runtime_error(
                            "Environment destination already exists: " +
                            destination.string());
                    std::filesystem::create_directories(
                        destination.parent_path());
                    std::filesystem::copy_file(
                        input.source, destination,
                        std::filesystem::copy_options::none);
                    CatalogEnvironment environment;
                    environment.id = input.environmentId;
                    environment.displayName = input.displayName;
                    environment.source = relative;
                    environment.environmentProfile = input.profileId;
                    try {
                        SceneCatalogEditor::addEnvironment(projectContext_,
                                                           environment);
                    } catch (...) {
                        std::error_code ignored;
                        std::filesystem::remove(destination, ignored);
                        throw;
                    }
                    refreshSceneRegistry();
                    editorUi_->environmentStatus =
                        "Imported " + input.displayName +
                        "; build derived IBL artifacts next";
                    editorUi_->environmentError.clear();
                } catch (const std::exception &error) {
                    editorUi_->environmentError = error.what();
                }
            };
        actions.buildEnvironment =
            [this](const std::string &id, bool force) {
                try {
                    const CatalogEnvironment *environment =
                        catalog_.findEnvironment(id);
                    if (!environment)
                        throw std::runtime_error(
                            "Environment is no longer present in Catalog");
                    assetImportManager_->request(
                        {environment->id, environment->environmentProfile,
                         ImportReason::ManualReimport, force,
                         AssetImportKind::Environment});
                    editorUi_->environmentStatus =
                        "Queued environment bake for " +
                        environment->displayName;
                    editorUi_->environmentError.clear();
                } catch (const std::exception &error) {
                    editorUi_->environmentError = error.what();
                }
            };
        actions.removeEnvironment = [this](const std::string &id) {
            try {
                if (sceneRuntime_->selectedEnvironmentId() == id)
                    setEnvironment("None");
                SceneCatalogEditor::removeEnvironment(projectContext_, id);
                editorUi_->environmentStatus =
                    "Removed " + id + " from Catalog";
                editorUi_->environmentError.clear();
                refreshSceneRegistry();
            } catch (const std::exception &error) {
                editorUi_->environmentError = error.what();
            }
        };
        actions.cancelTask = [this](uint64_t id) {
            if (assetImportManager_)
                assetImportManager_->cancel(id);
        };
#endif
        actions.openPath = [](const std::filesystem::path &path) {
            if (!path.empty())
                ShellExecuteW(nullptr, L"open", path.c_str(), nullptr,
                              nullptr, SW_SHOWNORMAL);
        };
        assetsPanel_->draw(snapshot, actions, environmentsOnly);
        return;
    }
}

void Application::requestManualCapture(bool includeGui) {
    captureUiError_.clear();
    if (!captureService_) {
        captureUiError_ =
            "Capture is unavailable in this runtime configuration.";
        VKR_LOG_WARN("Capture", "{}", captureUiError_);
        return;
    }

    try {
        lastCaptureTaskId_ = captureService_->request({}, includeGui);
    } catch (const std::exception &error) {
        captureUiError_ = error.what();
        VKR_LOG_ERROR("Capture", "Could not queue capture: {}",
                      captureUiError_);
    }
}

void Application::drawCapturePanel() {
    if (!captureService_) {
        editor::emptyState("Capture is unavailable in this package.");
        return;
    }

    ImGui::TextUnformatted("Source");
    constexpr const char *captureSources[] = {"Viewport", "Workspace"};
    int source = captureIncludeGui_ ? 1 : 0;
    if (editor::segmentedControl("CaptureSource", source, captureSources,
                                 std::size(captureSources)))
        captureIncludeGui_ = source == 1;
    const bool supported =
        !captureIncludeGui_ || swapChain_->captureSupported();
    ImGui::BeginDisabled(!supported);
    if (ImGui::Button("Capture"))
        requestManualCapture(captureIncludeGui_);
    ImGui::EndDisabled();

    std::optional<CaptureTaskSnapshot> latest;
    if (lastCaptureTaskId_ != 0)
        latest = captureService_->task(lastCaptureTaskId_);
    const bool canCancel =
        latest && !isTerminalCaptureTaskState(latest->state) &&
        latest->state != CaptureTaskState::Cancelling;
    ImGui::SameLine();
    ImGui::BeginDisabled(!canCancel);
    if (ImGui::Button("Cancel") && latest)
        captureService_->cancel(latest->request.taskId);
    ImGui::EndDisabled();

    if (!supported) {
        editor::statusIndicator(
            "Capture source unavailable", editor::StatusTone::Warning,
            swapChain_->captureUnsupportedReason().c_str());
    }
    if (!captureUiError_.empty())
        editor::statusIndicator(captureUiError_.c_str(),
                                editor::StatusTone::Error);

    ImGui::Separator();
    ImGui::TextDisabled("Output root");
    editor::pathValue(captureService_->captureRoot().string());
    const std::vector<CaptureTaskSnapshot> tasks = captureService_->tasks();
    ImGui::Text("Tasks: %llu",
                static_cast<unsigned long long>(tasks.size()));
    for (auto it = tasks.rbegin(); it != tasks.rend(); ++it) {
        const CaptureTaskSnapshot &task = *it;
        ImGui::PushID(static_cast<int>(task.request.taskId & 0x7fffffff));
        const bool open = ImGui::TreeNode(
            "Task", "Task %llu - %s",
            static_cast<unsigned long long>(task.request.taskId),
            captureTaskStateName(task.state));
        if (open) {
            ImGui::Text("Source: %s",
                        task.request.includeGui ? "Workspace" : "Viewport");
            if (task.result.width != 0) {
                ImGui::Text("Image: %ux%u %s", task.result.width,
                            task.result.height,
                            describeCaptureFormat(task.result.format).name);
                ImGui::Text("Frame serial: %llu",
                            static_cast<unsigned long long>(
                                task.result.frameSerial));
            }
            if (!task.result.outputPath.empty())
                ImGui::TextWrapped("Output: %s",
                                   task.result.outputPath.string().c_str());
            if (!task.result.sha256.empty())
                ImGui::TextWrapped("SHA-256: %s",
                                   task.result.sha256.c_str());
            if (!task.result.error.empty())
                ImGui::TextWrapped("Error: %s",
                                   task.result.error.c_str());
            if (isTerminalCaptureTaskState(task.state)) {
                ImGui::Text(
                    "Timing: %.2f ms total, %.2f GPU, %.2f CPU, %.2f encode",
                    task.result.timings.totalMs,
                    task.result.timings.gpuWaitMs,
                    task.result.timings.cpuCopyMs,
                    task.result.timings.encodeMs);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void Application::drawRenderPanel() {
    const auto &shaderVariants = shaderRegistry_.variants();
    if (!shaderVariants.empty()) {
        const ShaderVariant &currentVariant = currentShaderVariant();
        const char *current = currentVariant.displayName.c_str();
        if (ImGui::BeginCombo("Shader", current)) {
            auto drawCategory = [&](const char *category,
                                    const char *displayName) {
                bool hasVariants = false;
                for (const ShaderVariant &variant : shaderVariants)
                    hasVariants |= variant.category == category;
                if (!hasVariants)
                    return;

                ImGui::SeparatorText(displayName);
                for (const ShaderVariant &variant : shaderVariants) {
                    if (variant.category != category)
                        continue;
                    const bool selected =
                        variant.id == currentShaderVariantId_;
                    if (ImGui::Selectable(variant.displayName.c_str(),
                                          selected))
                        setShaderVariant(variant.id);
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
            };
            drawCategory("legacy", "Legacy");
            drawCategory("pbr", "PBR");
            drawCategory("debug", "Debug");

            bool hasOtherVariants = false;
            for (const ShaderVariant &variant : shaderVariants) {
                hasOtherVariants |= variant.category != "legacy" &&
                                    variant.category != "pbr" &&
                                    variant.category != "debug";
            }
            if (hasOtherVariants) {
                ImGui::SeparatorText("Other");
                for (const ShaderVariant &variant : shaderVariants) {
                    if (variant.category == "legacy" ||
                        variant.category == "pbr" ||
                        variant.category == "debug")
                        continue;
                    const bool selected =
                        variant.id == currentShaderVariantId_;
                    if (ImGui::Selectable(variant.displayName.c_str(),
                                          selected))
                        setShaderVariant(variant.id);
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", current);
    }
    constexpr uint32_t textureLimits[] = {0, 2048, 1024, 512};
    const bool nativeSceneActive =
        sceneRuntime_->currentSceneIndex() >= 0 &&
        sceneRuntime_->currentSceneIndex() < static_cast<int>(sceneRegistry_.size()) &&
        sceneRegistry_[sceneRuntime_->currentSceneIndex()].isNativeScene();
    ImGui::BeginDisabled(config_.assetImportMode ==
                             AssetImportMode::CookedOnly ||
                         nativeSceneActive);
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
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            nativeSceneActive
                ? "Preview only. Native scenes use each model's Catalog profile."
                : "Changing texture quality reloads the current model preview.");
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    float exposureEv = renderSettings_.exposureEv;
    if (ImGui::DragFloat("Exposure EV", &exposureEv, 0.05f, -10.0f,
                         10.0f)) {
        RenderSettingsPatch patch;
        patch.exposureEv = exposureEv;
        applyRenderSettings(patch);
    }
    constexpr const char *toneMapperLabels[] = {"PassThrough", "Reinhard",
                                                "ACES"};
    int toneMapperIndex = static_cast<int>(renderSettings_.toneMapper);
    if (ImGui::Combo("PBR Tone Mapper", &toneMapperIndex,
                     toneMapperLabels,
                     static_cast<int>(std::size(toneMapperLabels)))) {
        RenderSettingsPatch patch;
        patch.toneMapper = static_cast<ToneMapper>(toneMapperIndex);
        applyRenderSettings(patch);
    }
    ImGui::TextDisabled("Legacy/debug use PassThrough");
}

void Application::drawPostProcessingPanel() {
    const bool available = renderer_ && renderer_->bloomSupported();
    const bool compatible = currentShaderVariant().supportsBloom;
    ImGui::BeginDisabled(!available);
    bool enabled = renderSettings_.bloomEnabled;
    if (ImGui::Checkbox("Bloom", &enabled)) {
        RenderSettingsPatch patch;
        patch.bloomEnabled = enabled;
        applyRenderSettings(patch);
    }
    ImGui::BeginDisabled(!renderSettings_.bloomEnabled);
    float intensity = renderSettings_.bloomIntensity;
    if (ImGui::DragFloat("Intensity", &intensity, 0.01f, 0.0f, 5.0f)) {
        RenderSettingsPatch patch;
        patch.bloomIntensity = intensity;
        applyRenderSettings(patch);
    }
    if (ImGui::TreeNodeEx("Bloom Tuning")) {
        float threshold = renderSettings_.bloomThreshold;
        if (ImGui::DragFloat("Threshold", &threshold, 0.05f, 0.0f,
                             20.0f)) {
            RenderSettingsPatch patch;
            patch.bloomThreshold = threshold;
            applyRenderSettings(patch);
        }
        float softKnee = renderSettings_.bloomSoftKnee;
        if (ImGui::DragFloat("Soft Knee", &softKnee, 0.01f, 0.0f,
                             1.0f)) {
            RenderSettingsPatch patch;
            patch.bloomSoftKnee = softKnee;
            applyRenderSettings(patch);
        }
        ImGui::TextDisabled("Up to 6 half-resolution levels");
        ImGui::TreePop();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    const bool active =
        available && renderSettings_.bloomEnabled && compatible;
    editor::statusIndicator(
        active ? "Bloom active" : "Bloom inactive",
        active ? editor::StatusTone::Success : editor::StatusTone::Neutral);
    if (!available && renderer_) {
        editor::statusIndicator(
            "Bloom unavailable", editor::StatusTone::Warning,
            renderer_->bloomUnsupportedReason().c_str());
    } else if (renderSettings_.bloomEnabled && !compatible) {
        ImGui::TextDisabled("Selected Shader does not support Bloom.");
    }

    ImGui::SeparatorText("Ambient Occlusion");
    const ScreenSpaceEffectsStatus screenStatus =
        renderer_->screenSpaceEffectsStatus();
    constexpr const char *aoModeLabels[] = {"Off", "SSAO", "CACAO", "GTAO"};
    int aoMode = static_cast<int>(renderSettings_.ambientOcclusionMode);
    if (ImGui::BeginCombo("Mode##AmbientOcclusion", aoModeLabels[aoMode])) {
        for (int index = 0; index < static_cast<int>(std::size(aoModeLabels));
             ++index) {
            const auto mode = static_cast<AmbientOcclusionMode>(index);
            const bool supported =
                mode == AmbientOcclusionMode::Off ||
                (mode == AmbientOcclusionMode::Ssao &&
                 screenStatus.ssaoSupported) ||
                (mode == AmbientOcclusionMode::Cacao &&
                  screenStatus.cacaoInitialized) ||
                 (mode == AmbientOcclusionMode::Gtao &&
                  screenStatus.gtaoSupported);
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(aoModeLabels[index], index == aoMode)) {
                RenderSettingsPatch patch;
                patch.ambientOcclusionMode = mode;
                applyRenderSettings(patch);
            }
            if (index == aoMode)
                ImGui::SetItemDefaultFocus();
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    const bool ssaoRequested =
        renderSettings_.ambientOcclusionMode == AmbientOcclusionMode::Ssao;
    const bool ssaoDebugRequested =
        renderSettings_.screenSpaceDebugView ==
            ScreenSpaceDebugView::SsaoRaw ||
        renderSettings_.screenSpaceDebugView ==
            ScreenSpaceDebugView::SsaoFiltered;
    ImGui::BeginDisabled(!ssaoRequested && !ssaoDebugRequested);
    constexpr const char *qualityLabels[] = {"Low (8)", "Medium (16)",
                                              "High (32)"};
    int quality = static_cast<int>(renderSettings_.ssaoQuality);
    if (ImGui::Combo("Quality##SSAO", &quality, qualityLabels,
                     static_cast<int>(std::size(qualityLabels)))) {
        RenderSettingsPatch patch;
        patch.ssaoQuality = static_cast<SsaoQuality>(quality);
        applyRenderSettings(patch);
    }
    float radius = renderSettings_.ssaoRadius;
    if (ImGui::DragFloat("Radius##SSAO", &radius, 0.01f, 0.05f, 10.0f,
                         "%.2f")) {
        RenderSettingsPatch patch;
        patch.ssaoRadius = radius;
        applyRenderSettings(patch);
    }
    float bias = renderSettings_.ssaoBias;
    if (ImGui::DragFloat("Bias##SSAO", &bias, 0.001f, 0.0f, 0.2f,
                         "%.3f")) {
        RenderSettingsPatch patch;
        patch.ssaoBias = bias;
        applyRenderSettings(patch);
    }
    float aoIntensity = renderSettings_.ssaoIntensity;
    if (ImGui::DragFloat("Intensity##SSAO", &aoIntensity, 0.02f, 0.0f,
                         4.0f, "%.2f")) {
        RenderSettingsPatch patch;
        patch.ssaoIntensity = aoIntensity;
        applyRenderSettings(patch);
    }
    float aoPower = renderSettings_.ssaoPower;
    if (ImGui::DragFloat("Power##SSAO", &aoPower, 0.02f, 0.25f, 4.0f,
                         "%.2f")) {
        RenderSettingsPatch patch;
        patch.ssaoPower = aoPower;
        applyRenderSettings(patch);
    }
    ImGui::EndDisabled();

    const bool ssaoActive =
        screenStatus.activeMode == AmbientOcclusionMode::Ssao;
    editor::statusIndicator(
        ssaoActive ? "SSAO active" : "SSAO inactive",
        ssaoActive ? editor::StatusTone::Success
                   : editor::StatusTone::Neutral,
        !screenStatus.ssaoSupported
            ? screenStatus.ssaoUnavailableReason.c_str()
            : (ssaoRequested && !currentShaderVariant().supportsScreenSpace
                   ? "Selected Shader does not consume screen-space AO."
                   : nullptr));

    const bool cacaoRequested =
        renderSettings_.ambientOcclusionMode == AmbientOcclusionMode::Cacao;
    const bool cacaoDebugRequested =
        renderSettings_.screenSpaceDebugView ==
        ScreenSpaceDebugView::CacaoOutput;
    ImGui::BeginDisabled(!screenStatus.cacaoInitialized ||
                         (!cacaoRequested && !cacaoDebugRequested));
    constexpr const char *cacaoQualityLabels[] = {
        "Lowest", "Low", "Medium", "High", "Highest"};
    int cacaoQuality = static_cast<int>(renderSettings_.cacao.quality);
    if (ImGui::Combo("Quality##CACAO", &cacaoQuality, cacaoQualityLabels,
                     static_cast<int>(std::size(cacaoQualityLabels)))) {
        RenderSettingsPatch patch;
        patch.cacaoQuality = static_cast<CacaoQuality>(cacaoQuality);
        applyRenderSettings(patch);
    }
    constexpr const char *cacaoResolutionLabels[] = {"Native", "Half"};
    int cacaoResolution = static_cast<int>(renderSettings_.cacao.resolution);
    if (ImGui::Combo("Resolution##CACAO", &cacaoResolution,
                     cacaoResolutionLabels,
                     static_cast<int>(std::size(cacaoResolutionLabels)))) {
        RenderSettingsPatch patch;
        patch.cacaoResolution =
            static_cast<CacaoResolution>(cacaoResolution);
        applyRenderSettings(patch);
    }
    float cacaoRadius = renderSettings_.cacao.radius;
    if (ImGui::DragFloat("Radius##CACAO", &cacaoRadius, 0.01f, 0.05f,
                         10.0f, "%.2f")) {
        RenderSettingsPatch patch;
        patch.cacaoRadius = cacaoRadius;
        applyRenderSettings(patch);
    }
    float cacaoIntensity = renderSettings_.cacao.intensity;
    if (ImGui::DragFloat("Intensity##CACAO", &cacaoIntensity, 0.02f, 0.0f,
                         4.0f, "%.2f")) {
        RenderSettingsPatch patch;
        patch.cacaoIntensity = cacaoIntensity;
        applyRenderSettings(patch);
    }
    float cacaoPower = renderSettings_.cacao.power;
    if (ImGui::DragFloat("Power##CACAO", &cacaoPower, 0.02f, 0.25f, 4.0f,
                         "%.2f")) {
        RenderSettingsPatch patch;
        patch.cacaoPower = cacaoPower;
        applyRenderSettings(patch);
    }
    ImGui::EndDisabled();

    const bool cacaoActive =
        screenStatus.activeMode == AmbientOcclusionMode::Cacao;
    editor::statusIndicator(
        cacaoActive ? "CACAO active" : "CACAO inactive",
        cacaoActive ? editor::StatusTone::Success
                    : editor::StatusTone::Neutral,
        !screenStatus.cacaoInitialized
            ? screenStatus.cacaoUnavailableReason.c_str()
            : (cacaoRequested && !currentShaderVariant().supportsScreenSpace
                   ? "Selected Shader does not consume screen-space AO."
                   : nullptr));
    if (screenStatus.cacaoInitialized) {
        ImGui::TextDisabled("CACAO: %s, generation %llu, %ux%u",
                            screenStatus.cacaoFp32 ? "FP32" : "FP16",
                            static_cast<unsigned long long>(
                                screenStatus.cacaoGeneration),
                            screenStatus.cacaoOutputExtent.width,
                            screenStatus.cacaoOutputExtent.height);
    }

    const bool gtaoRequested =
        renderSettings_.ambientOcclusionMode == AmbientOcclusionMode::Gtao;
    const bool gtaoDebugRequested =
        renderSettings_.screenSpaceDebugView == ScreenSpaceDebugView::GtaoRaw ||
        renderSettings_.screenSpaceDebugView ==
            ScreenSpaceDebugView::GtaoTemporal ||
        renderSettings_.screenSpaceDebugView ==
            ScreenSpaceDebugView::GtaoFiltered ||
        renderSettings_.screenSpaceDebugView ==
            ScreenSpaceDebugView::GtaoRejection ||
        renderSettings_.screenSpaceDebugView ==
            ScreenSpaceDebugView::GtaoHistoryWeight;
    ImGui::BeginDisabled(!screenStatus.gtaoSupported ||
                         (!gtaoRequested && !gtaoDebugRequested));
    constexpr const char *gtaoQualityLabels[] = {
        "Low (2x2)", "Medium (3x4)", "High (4x6)"};
    int gtaoQuality = static_cast<int>(renderSettings_.gtao.quality);
    if (ImGui::Combo("Quality##GTAO", &gtaoQuality, gtaoQualityLabels,
                     static_cast<int>(std::size(gtaoQualityLabels)))) {
        RenderSettingsPatch patch;
        patch.gtaoQuality = static_cast<GtaoQuality>(gtaoQuality);
        applyRenderSettings(patch);
    }
    float gtaoRadius = renderSettings_.gtao.radius;
    if (ImGui::DragFloat("Radius##GTAO", &gtaoRadius, 0.01f, 0.05f, 10.0f,
                         "%.2f")) {
        RenderSettingsPatch patch;
        patch.gtaoRadius = gtaoRadius;
        applyRenderSettings(patch);
    }
    float gtaoFalloff = renderSettings_.gtao.falloff;
    if (ImGui::SliderFloat("Falloff##GTAO", &gtaoFalloff, 0.0f, 0.99f,
                           "%.2f")) {
        RenderSettingsPatch patch;
        patch.gtaoFalloff = gtaoFalloff;
        applyRenderSettings(patch);
    }
    float gtaoIntensity = renderSettings_.gtao.intensity;
    if (ImGui::DragFloat("Intensity##GTAO", &gtaoIntensity, 0.02f, 0.0f,
                         4.0f, "%.2f")) {
        RenderSettingsPatch patch;
        patch.gtaoIntensity = gtaoIntensity;
        applyRenderSettings(patch);
    }
    float gtaoPower = renderSettings_.gtao.power;
    if (ImGui::DragFloat("Power##GTAO", &gtaoPower, 0.02f, 0.25f, 4.0f,
                         "%.2f")) {
        RenderSettingsPatch patch;
        patch.gtaoPower = gtaoPower;
        applyRenderSettings(patch);
    }
    float gtaoTemporalWeight = renderSettings_.gtao.temporalWeight;
    if (ImGui::SliderFloat("History Weight##GTAO", &gtaoTemporalWeight,
                           0.0f, 0.99f, "%.2f")) {
        RenderSettingsPatch patch;
        patch.gtaoTemporalWeight = gtaoTemporalWeight;
        applyRenderSettings(patch);
    }
    ImGui::EndDisabled();

    editor::statusIndicator(
        screenStatus.gtaoActive ? "GTAO active" : "GTAO inactive",
        screenStatus.gtaoActive ? editor::StatusTone::Success
                                : editor::StatusTone::Neutral,
        !screenStatus.gtaoSupported
            ? screenStatus.gtaoUnavailableReason.c_str()
            : (gtaoRequested && !currentShaderVariant().supportsScreenSpace
                   ? "Selected Shader does not consume screen-space AO."
                   : nullptr));
    if (screenStatus.gtaoSupported &&
        (gtaoRequested || gtaoDebugRequested)) {
        ImGui::TextDisabled("History: %s, generation %llu, %ux%u",
                            screenStatus.gtaoHistoryValid ? "valid" : "reset",
                            static_cast<unsigned long long>(
                                screenStatus.gtaoHistoryGeneration),
                            screenStatus.gtaoExtent.width,
                            screenStatus.gtaoExtent.height);
        if (!screenStatus.gtaoLastResetReason.empty()) {
            ImGui::TextDisabled("Reset: %s",
                                screenStatus.gtaoLastResetReason.c_str());
        }
    }

    ImGui::SeparatorText("Temporal Anti-Aliasing");
    constexpr const char *taaModeLabels[] = {"Off", "TAA"};
    int taaMode =
        static_cast<int>(renderSettings_.temporalAntiAliasingMode);
    ImGui::BeginDisabled(!screenStatus.taaSupported);
    if (ImGui::Combo("Mode##TAA", &taaMode, taaModeLabels,
                     static_cast<int>(std::size(taaModeLabels)))) {
        RenderSettingsPatch patch;
        patch.temporalAntiAliasingMode =
            static_cast<TemporalAntiAliasingMode>(taaMode);
        applyRenderSettings(patch);
    }
    const bool taaRequested =
        renderSettings_.temporalAntiAliasingMode ==
        TemporalAntiAliasingMode::Taa;
    const bool taaDebugRequested =
        renderSettings_.screenSpaceDebugView ==
            ScreenSpaceDebugView::TaaHistory ||
        renderSettings_.screenSpaceDebugView ==
            ScreenSpaceDebugView::TaaRejection ||
        renderSettings_.screenSpaceDebugView ==
            ScreenSpaceDebugView::TaaHistoryWeight;
    ImGui::BeginDisabled(!taaRequested && !taaDebugRequested);
    float historyWeight = renderSettings_.taaHistoryWeight;
    if (ImGui::SliderFloat("History Weight##TAA", &historyWeight, 0.0f,
                           0.99f, "%.2f")) {
        RenderSettingsPatch patch;
        patch.taaHistoryWeight = historyWeight;
        applyRenderSettings(patch);
    }
    float sharpness = renderSettings_.taaSharpness;
    if (ImGui::SliderFloat("Sharpness##TAA", &sharpness, 0.0f, 1.0f,
                           "%.2f")) {
        RenderSettingsPatch patch;
        patch.taaSharpness = sharpness;
        applyRenderSettings(patch);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    editor::statusIndicator(
        screenStatus.taaActive ? "TAA active" : "TAA inactive",
        screenStatus.taaActive ? editor::StatusTone::Success
                               : editor::StatusTone::Neutral,
        !screenStatus.taaSupported
            ? screenStatus.taaUnavailableReason.c_str()
            : nullptr);
    if (screenStatus.taaSupported) {
        ImGui::TextDisabled(
            "History: %s, generation %llu, jitter (%.2f, %.2f)",
            screenStatus.taaHistoryValid ? "valid" : "reset",
            static_cast<unsigned long long>(
                screenStatus.taaHistoryGeneration),
            screenStatus.taaJitterPixels.x,
            screenStatus.taaJitterPixels.y);
        if (!screenStatus.taaLastResetReason.empty()) {
            ImGui::TextDisabled("Reset: %s",
                                screenStatus.taaLastResetReason.c_str());
        }
    }

    ImGui::SeparatorText("Reflections");
    constexpr const char *reflectionLabels[] = {"IBL Only", "SSR"};
    int reflectionMode = static_cast<int>(renderSettings_.reflectionMode);
    ImGui::BeginDisabled(!screenStatus.ssrSupported);
    if (ImGui::Combo("Mode##Reflections", &reflectionMode,
                     reflectionLabels,
                     static_cast<int>(std::size(reflectionLabels)))) {
        RenderSettingsPatch patch;
        patch.reflectionMode = static_cast<ReflectionMode>(reflectionMode);
        applyRenderSettings(patch);
    }
    const bool ssrRequested =
        renderSettings_.reflectionMode == ReflectionMode::Ssr;
    ImGui::BeginDisabled(!ssrRequested);
    constexpr const char *ssrQualityLabels[] = {"Low", "Medium", "High"};
    int ssrQuality = static_cast<int>(renderSettings_.ssrQuality);
    if (ImGui::Combo("Quality##SSR", &ssrQuality, ssrQualityLabels,
                     static_cast<int>(std::size(ssrQualityLabels)))) {
        RenderSettingsPatch patch;
        patch.ssrQuality = static_cast<SsrQuality>(ssrQuality);
        applyRenderSettings(patch);
    }
    float ssrDistance = renderSettings_.ssrMaxDistance;
    if (ImGui::DragFloat("Max Distance##SSR", &ssrDistance, 0.25f,
                         0.1f, 1000.0f, "%.1f")) {
        RenderSettingsPatch patch; patch.ssrMaxDistance = ssrDistance;
        applyRenderSettings(patch);
    }
    float ssrThickness = renderSettings_.ssrThickness;
    if (ImGui::DragFloat("Thickness##SSR", &ssrThickness, 0.005f,
                         0.001f, 10.0f, "%.3f")) {
        RenderSettingsPatch patch; patch.ssrThickness = ssrThickness;
        applyRenderSettings(patch);
    }
    float ssrRoughness = renderSettings_.ssrMaxRoughness;
    if (ImGui::SliderFloat("Max Roughness##SSR", &ssrRoughness,
                           0.0f, 1.0f, "%.2f")) {
        RenderSettingsPatch patch; patch.ssrMaxRoughness = ssrRoughness;
        applyRenderSettings(patch);
    }
    float ssrIntensity = renderSettings_.ssrIntensity;
    if (ImGui::SliderFloat("Intensity##SSR", &ssrIntensity,
                           0.0f, 4.0f, "%.2f")) {
        RenderSettingsPatch patch; patch.ssrIntensity = ssrIntensity;
        applyRenderSettings(patch);
    }
    float ssrHistory = renderSettings_.ssrHistoryWeight;
    if (ImGui::SliderFloat("History Weight##SSR", &ssrHistory,
                           0.0f, 0.99f, "%.2f")) {
        RenderSettingsPatch patch; patch.ssrHistoryWeight = ssrHistory;
        applyRenderSettings(patch);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    editor::statusIndicator(
        screenStatus.ssrActive ? "SSR active" : "SSR inactive",
        screenStatus.ssrActive ? editor::StatusTone::Success
                               : editor::StatusTone::Neutral,
        !screenStatus.ssrSupported
            ? screenStatus.ssrUnavailableReason.c_str()
            : (ssrRequested && !currentShaderVariant().supportsScreenSpace
                   ? "Selected Shader does not consume screen-space reflections."
                   : nullptr));
    if (screenStatus.ssrSupported && ssrRequested) {
        ImGui::TextDisabled("History: %s, generation %llu, %ux%u",
                            screenStatus.ssrHistoryValid ? "valid" : "reset",
                            static_cast<unsigned long long>(
                                screenStatus.ssrHistoryGeneration),
                            screenStatus.ssrExtent.width,
                            screenStatus.ssrExtent.height);
        if (!screenStatus.ssrLastResetReason.empty())
            ImGui::TextDisabled("Reset: %s",
                                screenStatus.ssrLastResetReason.c_str());
    }

    ImGui::SeparatorText("Global Illumination");
    constexpr const char *giModeLabels[] = {
        "Ambient / IBL", "SSGI", "DDGI", "SSGI + DDGI"};
    int giMode = static_cast<int>(renderSettings_.globalIlluminationMode);
    const DdgiRuntimeStatus ddgiStatus =
        renderer_ ? renderer_->ddgiStatus() : DdgiRuntimeStatus{};
    if (ImGui::BeginCombo("Mode##GI", giModeLabels[giMode])) {
        for (int index = 0;
             index < static_cast<int>(std::size(giModeLabels)); ++index) {
            const auto mode = static_cast<GlobalIlluminationMode>(index);
            const bool needsSsgi = mode == GlobalIlluminationMode::Ssgi ||
                                   mode == GlobalIlluminationMode::SsgiDdgi;
            const bool needsDdgi = mode == GlobalIlluminationMode::Ddgi ||
                                   mode == GlobalIlluminationMode::SsgiDdgi;
            const bool supported =
                (!needsSsgi || screenStatus.ssgiSupported) &&
                (!needsDdgi || ddgiStatus.supported);
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(giModeLabels[index], giMode == index)) {
                RenderSettingsPatch patch;
                patch.globalIlluminationMode = mode;
                applyRenderSettings(patch);
            }
            ImGui::EndDisabled();
            if (giMode == index)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    const bool ssgiRequested =
        renderSettings_.globalIlluminationMode == GlobalIlluminationMode::Ssgi ||
        renderSettings_.globalIlluminationMode ==
            GlobalIlluminationMode::SsgiDdgi;
    ImGui::BeginDisabled(!ssgiRequested);
    constexpr const char *ssgiQualityLabels[] = {"Low", "Medium", "High"};
    int ssgiQuality = static_cast<int>(renderSettings_.ssgiQuality);
    if (ImGui::Combo("Quality##SSGI", &ssgiQuality, ssgiQualityLabels,
                     static_cast<int>(std::size(ssgiQualityLabels)))) {
        RenderSettingsPatch patch;
        patch.ssgiQuality = static_cast<SsgiQuality>(ssgiQuality);
        applyRenderSettings(patch);
    }
    float ssgiDistance = renderSettings_.ssgiMaxDistance;
    if (ImGui::DragFloat("Max Distance##SSGI", &ssgiDistance, 0.1f,
                         0.05f, 1000.0f, "%.2f")) {
        RenderSettingsPatch patch;
        patch.ssgiMaxDistance = ssgiDistance;
        applyRenderSettings(patch);
    }
    float ssgiThickness = renderSettings_.ssgiThickness;
    if (ImGui::DragFloat("Thickness##SSGI", &ssgiThickness, 0.005f,
                         0.001f, 10.0f, "%.3f")) {
        RenderSettingsPatch patch;
        patch.ssgiThickness = ssgiThickness;
        applyRenderSettings(patch);
    }
    float ssgiIntensity = renderSettings_.ssgiIntensity;
    if (ImGui::SliderFloat("Intensity##SSGI", &ssgiIntensity,
                           0.0f, 4.0f, "%.2f")) {
        RenderSettingsPatch patch;
        patch.ssgiIntensity = ssgiIntensity;
        applyRenderSettings(patch);
    }
    float ssgiClamp = renderSettings_.ssgiRadianceClamp;
    if (ImGui::DragFloat("Radiance Clamp##SSGI", &ssgiClamp, 0.1f,
                         0.1f, 100.0f, "%.1f")) {
        RenderSettingsPatch patch;
        patch.ssgiRadianceClamp = ssgiClamp;
        applyRenderSettings(patch);
    }
    float ssgiHistory = renderSettings_.ssgiHistoryWeight;
    if (ImGui::SliderFloat("History Weight##SSGI", &ssgiHistory,
                           0.0f, 0.99f, "%.2f")) {
        RenderSettingsPatch patch;
        patch.ssgiHistoryWeight = ssgiHistory;
        applyRenderSettings(patch);
    }
    ImGui::EndDisabled();
    editor::statusIndicator(
        screenStatus.ssgiActive ? "SSGI active" : "SSGI inactive",
        screenStatus.ssgiActive ? editor::StatusTone::Success
                                : editor::StatusTone::Neutral,
        !screenStatus.ssgiSupported
            ? screenStatus.ssgiUnavailableReason.c_str()
            : (ssgiRequested && !currentShaderVariant().supportsScreenSpace
                   ? "Selected Shader does not consume screen-space GI."
                   : nullptr));
    if (screenStatus.ssgiSupported && ssgiRequested) {
        ImGui::TextDisabled("History: %s, generation %llu, %ux%u",
                            screenStatus.ssgiHistoryValid ? "valid" : "reset",
                            static_cast<unsigned long long>(
                                screenStatus.ssgiHistoryGeneration),
                            screenStatus.ssgiExtent.width,
                            screenStatus.ssgiExtent.height);
        if (!screenStatus.ssgiLastResetReason.empty())
            ImGui::TextDisabled("Reset: %s",
                                screenStatus.ssgiLastResetReason.c_str());
    }

    const bool ddgiRequested =
        renderSettings_.globalIlluminationMode == GlobalIlluminationMode::Ddgi ||
        renderSettings_.globalIlluminationMode ==
            GlobalIlluminationMode::SsgiDdgi;
    ImGui::BeginDisabled(!ddgiRequested || !ddgiStatus.supported);
    float ddgiClamp = renderSettings_.ddgi.radianceClamp;
    if (ImGui::DragFloat("Radiance Clamp##DDGI", &ddgiClamp, 0.1f,
                         0.1f, 100.0f, "%.1f")) {
        RenderSettingsPatch patch;
        patch.ddgiRadianceClamp = ddgiClamp;
        applyRenderSettings(patch);
    }
    constexpr const char *ddgiDebugLabels[] = {
        "None", "Irradiance", "Distance", "Classification"};
    int ddgiDebug = static_cast<int>(renderSettings_.ddgi.debugView);
    if (ImGui::Combo("Debug##DDGI", &ddgiDebug, ddgiDebugLabels,
                     static_cast<int>(std::size(ddgiDebugLabels)))) {
        RenderSettingsPatch patch;
        patch.ddgiDebugView = static_cast<DdgiDebugView>(ddgiDebug);
        applyRenderSettings(patch);
    }
    ImGui::EndDisabled();
    editor::statusIndicator(
        ddgiStatus.active ? "DDGI active" : "DDGI inactive",
        ddgiStatus.active ? editor::StatusTone::Success
                          : editor::StatusTone::Neutral,
        !ddgiStatus.supported
            ? ddgiStatus.unavailableReason.c_str()
            : (ddgiRequested && !ddgiStatus.componentPresent
                   ? "The active native scene has no DDGI Probe Volume."
                   : (ddgiRequested &&
                              !currentShaderVariant().supportsDdgi
                          ? "Selected Shader does not consume DDGI."
                          : nullptr)));
    if (ddgiStatus.componentPresent) {
        ImGui::TextDisabled(
            "Probes: %u, update %u x %u rays, cursor %u",
            ddgiStatus.probeCount, ddgiStatus.probesUpdatedPerFrame,
            ddgiStatus.raysPerProbe, ddgiStatus.updateCursor);
        ImGui::TextDisabled(
            "TLAS instances: %u, generation %llu, memory %.2f MiB",
            ddgiStatus.tracedInstanceCount,
            static_cast<unsigned long long>(ddgiStatus.generation),
            static_cast<double>(ddgiStatus.allocatedBytes) /
                (1024.0 * 1024.0));
    }

    ImGui::SeparatorText("Screen-Space Debug");
    constexpr const char *screenDebugLabels[] = {
        "None", "Nearest Depth", "Scene Color", "SSAO Raw",
        "SSAO Filtered", "CACAO Output", "GTAO Raw", "GTAO Temporal",
        "GTAO Filtered", "GTAO Rejection", "GTAO History Weight",
        "TAA History", "TAA Rejection", "TAA History Weight",
        "SSR Raw", "SSR Temporal", "SSR Filtered", "SSR Confidence",
        "SSR Rejection", "SSGI Raw", "SSGI Temporal", "SSGI Filtered",
        "SSGI Confidence", "SSGI Variance", "SSGI Rejection"};
    int screenDebug =
        static_cast<int>(renderSettings_.screenSpaceDebugView);
    const char *preview = screenDebugLabels[screenDebug];
    if (ImGui::BeginCombo("Debug View##ScreenSpace", preview)) {
        for (int index = 0; index < static_cast<int>(std::size(screenDebugLabels));
             ++index) {
            const auto view = static_cast<ScreenSpaceDebugView>(index);
            const bool supported =
                view == ScreenSpaceDebugView::None ||
                (view == ScreenSpaceDebugView::NearestDepth &&
                 screenStatus.depthPyramidSupported) ||
                (view == ScreenSpaceDebugView::SceneColor &&
                 screenStatus.colorPyramidSupported) ||
                ((view == ScreenSpaceDebugView::SsaoRaw ||
                  view == ScreenSpaceDebugView::SsaoFiltered) &&
                  screenStatus.ssaoSupported) ||
                (view == ScreenSpaceDebugView::CacaoOutput &&
                  screenStatus.cacaoInitialized) ||
                ((view == ScreenSpaceDebugView::GtaoRaw ||
                  view == ScreenSpaceDebugView::GtaoTemporal ||
                  view == ScreenSpaceDebugView::GtaoFiltered ||
                  view == ScreenSpaceDebugView::GtaoRejection ||
                  view == ScreenSpaceDebugView::GtaoHistoryWeight) &&
                 screenStatus.gtaoSupported) ||
                ((view == ScreenSpaceDebugView::TaaHistory ||
                  view == ScreenSpaceDebugView::TaaRejection ||
                  view == ScreenSpaceDebugView::TaaHistoryWeight) &&
                 screenStatus.taaSupported) ||
                ((view == ScreenSpaceDebugView::SsrRaw ||
                  view == ScreenSpaceDebugView::SsrTemporal ||
                  view == ScreenSpaceDebugView::SsrFiltered ||
                  view == ScreenSpaceDebugView::SsrConfidence ||
                  view == ScreenSpaceDebugView::SsrRejection) &&
                 screenStatus.ssrSupported) ||
                ((view == ScreenSpaceDebugView::SsgiRaw ||
                  view == ScreenSpaceDebugView::SsgiTemporal ||
                  view == ScreenSpaceDebugView::SsgiFiltered ||
                  view == ScreenSpaceDebugView::SsgiConfidence ||
                  view == ScreenSpaceDebugView::SsgiVariance ||
                  view == ScreenSpaceDebugView::SsgiRejection) &&
                 screenStatus.ssgiSupported);
            ImGui::BeginDisabled(!supported);
            const bool selected = index == screenDebug;
            if (ImGui::Selectable(screenDebugLabels[index], selected)) {
                RenderSettingsPatch patch;
                patch.screenSpaceDebugView = view;
                if (view != ScreenSpaceDebugView::None)
                    patch.surfaceDebugView = SurfaceDebugView::None;
                applyRenderSettings(patch);
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    uint32_t maximumMip = 0;
    if (renderSettings_.screenSpaceDebugView ==
        ScreenSpaceDebugView::NearestDepth) {
        maximumMip = screenStatus.depthMipLevels > 0
                         ? screenStatus.depthMipLevels - 1u
                         : 0u;
    } else if (renderSettings_.screenSpaceDebugView ==
               ScreenSpaceDebugView::SceneColor) {
        maximumMip = screenStatus.colorMipLevels > 0
                         ? screenStatus.colorMipLevels - 1u
                         : 0u;
    }
    int debugMip = static_cast<int>(std::min(
        renderSettings_.screenSpaceDebugMip, maximumMip));
    ImGui::BeginDisabled(maximumMip == 0);
    if (ImGui::SliderInt("Mip##ScreenSpace", &debugMip, 0,
                         static_cast<int>(maximumMip))) {
        RenderSettingsPatch patch;
        patch.screenSpaceDebugMip = static_cast<uint32_t>(debugMip);
        applyRenderSettings(patch);
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Resources: %.2f MiB",
                        static_cast<double>(screenStatus.estimatedMemoryBytes) /
                            (1024.0 * 1024.0));
}

void Application::drawSurfaceDataPanel() {
    const SurfaceDataStatus status = renderer_->surfaceDataStatus();
    constexpr const char *labels[] = {
        "None", "Normal", "Roughness", "Motion", "History Validity"};
    int mode = static_cast<int>(renderSettings_.surfaceDebugView);
    ImGui::BeginDisabled(!status.supported);
    if (ImGui::Combo("Debug View", &mode, labels,
                     static_cast<int>(std::size(labels)))) {
        RenderSettingsPatch patch;
        patch.surfaceDebugView = static_cast<SurfaceDebugView>(mode);
        if (patch.surfaceDebugView != SurfaceDebugView::None)
            patch.screenSpaceDebugView = ScreenSpaceDebugView::None;
        applyRenderSettings(patch);
    }
    ImGui::BeginDisabled(
        renderSettings_.surfaceDebugView != SurfaceDebugView::Motion);
    float motionScale = renderSettings_.surfaceMotionDebugScale;
    if (ImGui::DragFloat("Motion Scale", &motionScale, 0.5f, 0.1f,
                         1024.0f, "%.1f")) {
        RenderSettingsPatch patch;
        patch.surfaceMotionDebugScale = motionScale;
        applyRenderSettings(patch);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (status.supported) {
        editor::statusIndicator(
            status.active ? "Surface data active" : "Surface data inactive",
            status.active ? editor::StatusTone::Success
                          : editor::StatusTone::Neutral);
        ImGui::Text("History: %u / %u items",
                    visibilityFrame_.history.historyValidItems,
                    static_cast<uint32_t>(visibilityFrame_.items.size()));
        ImGui::TextDisabled("Generation %llu",
                            static_cast<unsigned long long>(
                                visibilityFrame_.history.historyGeneration));
        if (!visibilityFrame_.history.invalidationReason.empty()) {
            ImGui::TextDisabled("Reset: %s",
                                visibilityFrame_.history.invalidationReason
                                    .c_str());
        }
        ImGui::TextDisabled("Formats: depth=%d normal=%d motion=%d",
                            static_cast<int>(status.depthFormat),
                            static_cast<int>(status.normalRoughnessFormat),
                            static_cast<int>(status.motionFormat));
        ImGui::TextDisabled("History buffers: %u / %u (%llu KiB)",
                            status.historyCapacities[0],
                            status.historyCapacities[1],
                            static_cast<unsigned long long>(
                                status.allocatedBytes / 1024u));
    } else {
        editor::statusIndicator("Surface data unavailable",
                                editor::StatusTone::Warning,
                                status.unavailableReason.c_str());
    }
}

void Application::drawCullingPanel() {
    bool frustumEnabled = renderSettings_.culling.frustumEnabled;
    if (ImGui::Checkbox("Camera Frustum", &frustumEnabled)) {
        RenderSettingsPatch patch;
        patch.frustumCullingEnabled = frustumEnabled;
        applyRenderSettings(patch);
    }

    bool distanceEnabled = renderSettings_.culling.distanceEnabled;
    if (ImGui::Checkbox("Max Draw Distance", &distanceEnabled)) {
        RenderSettingsPatch patch;
        patch.distanceCullingEnabled = distanceEnabled;
        applyRenderSettings(patch);
    }
    ImGui::BeginDisabled(!renderSettings_.culling.distanceEnabled);
    float maxDrawDistance = renderSettings_.culling.maxDrawDistance;
    if (ImGui::DragFloat("Distance", &maxDrawDistance, 1.0f, 0.1f,
                         1000000.0f, "%.1f")) {
        RenderSettingsPatch patch;
        patch.maxDrawDistance = maxDrawDistance;
        applyRenderSettings(patch);
    }
    ImGui::EndDisabled();

    bool smallObjectEnabled = renderSettings_.culling.smallObjectEnabled;
    if (ImGui::Checkbox("Small Objects", &smallObjectEnabled)) {
        RenderSettingsPatch patch;
        patch.smallObjectCullingEnabled = smallObjectEnabled;
        applyRenderSettings(patch);
    }
    ImGui::BeginDisabled(!renderSettings_.culling.smallObjectEnabled);
    float minimumPixels = renderSettings_.culling.minProjectedSizePixels;
    if (ImGui::DragFloat("Minimum Size", &minimumPixels, 0.1f, 0.0f,
                         256.0f, "%.1f px")) {
        RenderSettingsPatch patch;
        patch.minProjectedSizePixels = minimumPixels;
        applyRenderSettings(patch);
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Shadow Casters");
    bool shadowCullingEnabled =
        renderSettings_.culling.shadowCullingEnabled;
    if (ImGui::Checkbox("Cull Shadow Casters", &shadowCullingEnabled)) {
        RenderSettingsPatch patch;
        patch.shadowCullingEnabled = shadowCullingEnabled;
        applyRenderSettings(patch);
    }
    ImGui::BeginDisabled(!renderSettings_.culling.shadowCullingEnabled);
    float shadowDistance = renderSettings_.culling.shadowDistance;
    if (ImGui::DragFloat("Shadow Distance", &shadowDistance, 1.0f,
                         kMinDirectionalShadowDistance,
                         kMaxDirectionalShadowDistance, "%.1f")) {
        RenderSettingsPatch patch;
        patch.shadowDistance = shadowDistance;
        applyRenderSettings(patch);
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("GPU Occlusion");
    const OcclusionCullingStatus status =
        renderer_->occlusionCullingStatus();
    ImGui::BeginDisabled(!status.supported);
    bool occlusionEnabled = renderSettings_.culling.occlusionEnabled;
    if (ImGui::Checkbox("Hi-Z Occlusion", &occlusionEnabled)) {
        RenderSettingsPatch patch;
        patch.occlusionCullingEnabled = occlusionEnabled;
        applyRenderSettings(patch);
    }
    ImGui::BeginDisabled(!renderSettings_.culling.occlusionEnabled);
    int minimumCandidates = static_cast<int>(
        renderSettings_.culling.occlusionMinCandidates);
    if (ImGui::DragInt("Minimum Candidates", &minimumCandidates, 1.0f,
                       0, 65536)) {
        RenderSettingsPatch patch;
        patch.occlusionMinCandidates =
            static_cast<uint32_t>(std::max(minimumCandidates, 0));
        applyRenderSettings(patch);
    }
    float depthBias = renderSettings_.culling.occlusionDepthBias;
    if (ImGui::DragFloat("Depth Bias", &depthBias, 0.00005f, 0.0f, 0.05f,
                         "%.5f")) {
        RenderSettingsPatch patch;
        patch.occlusionDepthBias = depthBias;
        applyRenderSettings(patch);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (status.supported) {
        editor::statusIndicator(
            status.active ? "Occlusion active" : "Occlusion inactive",
            status.active ? editor::StatusTone::Success
                          : editor::StatusTone::Neutral);
    } else {
        editor::statusIndicator("Occlusion unavailable",
                                editor::StatusTone::Warning,
                                status.unavailableReason.c_str());
    }

    if (ImGui::TreeNodeEx("Culling Statistics",
                          ImGuiTreeNodeFlags_DefaultOpen)) {
        const VisibilityCpuStatistics &stats = visibilityFrame_.cpuStats;
        ImGui::Text("Source: %u  Visible: %u", stats.sourceDraws,
                    stats.cameraVisible);
        ImGui::Text("Camera culled: %u frustum, %u distance, %u small",
                    stats.frustumCulled, stats.distanceCulled,
                    stats.smallObjectCulled);
        ImGui::Text("Camera queues: %u opaque, %u transparent",
                    stats.cameraOpaque, stats.cameraTransparent);
        ImGui::Text("Invalid bounds: %u", stats.invalidBounds);
        ImGui::Text("Shadow: %u / %u visible, %u culled",
                    stats.shadowVisible, stats.shadowCandidates,
                    stats.shadowCulled);
        ImGui::Text("CSM draws: %u / %u / %u / %u",
                    stats.directionalShadowDraws[0],
                    stats.directionalShadowDraws[1],
                    stats.directionalShadowDraws[2],
                    stats.directionalShadowDraws[3]);
        uint32_t pointShadowDraws = 0;
        for (uint32_t count : stats.pointShadowDraws)
            pointShadowDraws += count;
        uint32_t spotShadowDraws = 0;
        for (uint32_t count : stats.spotShadowDraws)
            spotShadowDraws += count;
        ImGui::Text("Punctual shadow draws: %u point, %u spot",
                    pointShadowDraws, spotShadowDraws);
        ImGui::Text("Depth draws: %u", stats.depthPrepassDraws);
        ImGui::Text("GPU occluded: %u / %u",
                    status.completed.occluded,
                    status.completed.candidates);
        if (status.latestUncullable != 0)
            ImGui::Text("GPU uncullable: %u", status.latestUncullable);
        if (status.completed.frameSerial != 0) {
            ImGui::TextDisabled("GPU result frame: %llu",
                                static_cast<unsigned long long>(
                                    status.completed.frameSerial));
        }
        ImGui::Text("Hi-Z mips: %u", status.hiZMipLevels);
        ImGui::Text("Indirect capacity: %u / %u",
                    status.indirectCapacities[0],
                    status.indirectCapacities[1]);
        ImGui::Text("Visibility buffers: %.2f KiB",
                    static_cast<double>(status.allocatedBytes) / 1024.0);
        ImGui::TreePop();
    }
}

void Application::drawSceneLoadingPanel() {
    if (!sceneRuntime_->latestSceneLoadTask())
        return;

    const auto task = sceneRuntime_->latestSceneLoadTask();
    const SceneLoadState state = task->state.load();
    if (isTerminalSceneLoadState(state))
        return;

    ImGui::Text("Task: %llu",
                static_cast<unsigned long long>(task->id));
    ImGui::Text("Scene: %s", task->sceneName.c_str());
    ImGui::Text("State: %s", sceneLoadStateName(state));
    ImGui::Text("Phase: %s", sceneLoadPhaseName(task->phase.load()));
    if (task->kind == SceneLoadKind::NativeScene) {
        ImGui::Text("Models: %llu / %llu",
                    static_cast<unsigned long long>(task->readyModelCount),
                    static_cast<unsigned long long>(task->uniqueModelCount));
        if (!task->targetEnvironmentId.empty())
            ImGui::Text("Environment: %s",
                        task->targetEnvironmentId.c_str());
        if (!task->failedModelId.empty())
            ImGui::Text("Failed Model: %s",
                        task->failedModelId.c_str());
    }
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
    if (state != SceneLoadState::ReadyToPublish &&
        ImGui::Button("Cancel Load"))
        cancelSceneLoad(task->id);
    std::lock_guard<std::mutex> lock(task->mutex);
    if (!task->error.empty())
        ImGui::TextWrapped("Error: %s", task->error.c_str());
}

void Application::drawLightingPanel() {
    bool shadowsEnabled = renderSettings_.shadowsEnabled;
    if (ImGui::Checkbox("Shadows", &shadowsEnabled)) {
        RenderSettingsPatch patch;
        patch.shadowsEnabled = shadowsEnabled;
        applyRenderSettings(patch);
    }
    ImGui::BeginDisabled(!renderSettings_.shadowsEnabled);
    if (ImGui::TreeNodeEx("Shadow Tuning")) {
        float receiverBias = renderSettings_.shadowReceiverBias;
        if (ImGui::DragFloat("Receiver Bias", &receiverBias, 0.00005f,
                             0.0f, 0.05f, "%.5f")) {
            RenderSettingsPatch patch;
            patch.shadowReceiverBias = receiverBias;
            applyRenderSettings(patch);
        }
        float pointReceiverBias =
            renderSettings_.pointShadowReceiverBiasWorld;
        if (ImGui::DragFloat("Point Bias (World)", &pointReceiverBias,
                             0.001f, 0.0f, 1.0f, "%.3f")) {
            RenderSettingsPatch patch;
            patch.pointShadowReceiverBiasWorld = pointReceiverBias;
            applyRenderSettings(patch);
        }
        float constantBias = renderSettings_.shadowConstantBias;
        if (ImGui::DragFloat("Constant Bias", &constantBias, 0.05f, 0.0f,
                             10.0f)) {
            RenderSettingsPatch patch;
            patch.shadowConstantBias = constantBias;
            applyRenderSettings(patch);
        }
        float slopeBias = renderSettings_.shadowSlopeBias;
        if (ImGui::DragFloat("Slope Bias", &slopeBias, 0.05f, 0.0f,
                             10.0f)) {
            RenderSettingsPatch patch;
            patch.shadowSlopeBias = slopeBias;
            applyRenderSettings(patch);
        }
        ImGui::TextDisabled("Shadow map %ux%u",
                            kDirectionalShadowMapSize,
                            kDirectionalShadowMapSize);
        int maxPoint = static_cast<int>(
            renderSettings_.maxPointShadowLights);
        if (ImGui::SliderInt("Max Point Shadows", &maxPoint, 0,
                             static_cast<int>(kMaxPointShadowLights))) {
            RenderSettingsPatch patch;
            patch.maxPointShadowLights =
                static_cast<uint32_t>(maxPoint);
            applyRenderSettings(patch);
        }
        float pointDistance = renderSettings_.pointShadowDistance;
        if (ImGui::DragFloat("Point Shadow Distance", &pointDistance,
                             1.0f, kMinPunctualShadowDistance,
                             kMaxPunctualShadowDistance, "%.1f")) {
            RenderSettingsPatch patch;
            patch.pointShadowDistance = pointDistance;
            applyRenderSettings(patch);
        }
        int maxSpot = static_cast<int>(
            renderSettings_.maxSpotShadowLights);
        if (ImGui::SliderInt("Max Spot Shadows", &maxSpot, 0,
                             static_cast<int>(kMaxSpotShadowLights))) {
            RenderSettingsPatch patch;
            patch.maxSpotShadowLights =
                static_cast<uint32_t>(maxSpot);
            applyRenderSettings(patch);
        }
        float spotDistance = renderSettings_.spotShadowDistance;
        if (ImGui::DragFloat("Spot Shadow Distance", &spotDistance,
                             1.0f, kMinPunctualShadowDistance,
                             kMaxPunctualShadowDistance, "%.1f")) {
            RenderSettingsPatch patch;
            patch.spotShadowDistance = spotDistance;
            applyRenderSettings(patch);
        }
        ImGui::TextDisabled(
            "Active: %u point, %u spot",
            lastLightStats_.pointShadowLights,
            lastLightStats_.spotShadowLights);
        ImGui::TextDisabled(
            "CSM casters: %u / %u / %u / %u",
            visibilityFrame_.cpuStats.directionalShadowDraws[0],
            visibilityFrame_.cpuStats.directionalShadowDraws[1],
            visibilityFrame_.cpuStats.directionalShadowDraws[2],
            visibilityFrame_.cpuStats.directionalShadowDraws[3]);
        for (const PunctualShadowSelection &selection :
             lastLightStats_.pointShadowSelections) {
            uint32_t casterDraws = 0;
            for (uint32_t face = 0; face < kPointShadowFaceCount; ++face) {
                casterDraws += visibilityFrame_.cpuStats.pointShadowDraws[
                    selection.slot * kPointShadowFaceCount + face];
            }
            const std::string &label = selection.name.empty()
                                           ? selection.stableKey
                                           : selection.name;
            ImGui::BulletText(
                "Point [%u] %s, %s, score %.2f, age %u, far %.1f, %u draws%s",
                selection.slot, label.c_str(),
                shadowCastingPolicyName(selection.policy), selection.score,
                selection.age, selection.farPlane, casterDraws,
                selection.focused ? ", selected" :
                selection.retained ? ", retained" : "");
        }
        for (const PunctualShadowSelection &selection :
             lastLightStats_.spotShadowSelections) {
            const std::string &label = selection.name.empty()
                                           ? selection.stableKey
                                           : selection.name;
            ImGui::BulletText(
                "Spot [%u] %s, %s, score %.2f, age %u, far %.1f, %u draws%s",
                selection.slot, label.c_str(),
                shadowCastingPolicyName(selection.policy), selection.score,
                selection.age, selection.farPlane,
                visibilityFrame_.cpuStats.spotShadowDraws[selection.slot],
                selection.focused ? ", selected" :
                selection.retained ? ", retained" : "");
        }
        ImGui::TextDisabled("Shadow revision: %llu%s",
                            static_cast<unsigned long long>(
                                lastLightStats_.shadowContentRevision),
                            lastLightStats_.shadowTemporalReactive
                                ? " (TAA reactive)"
                                : "");
        for (const ShadowEviction &eviction :
             lastLightStats_.shadowEvictions) {
            ImGui::BulletText("Evicted %s: %s",
                              eviction.stableKey.c_str(),
                              eviction.reason.c_str());
        }
        constexpr uint64_t shadowTexelBytes = 4;
        constexpr uint64_t shadowBytes =
            shadowTexelBytes *
            (uint64_t{kCsmCascadeCount} * kDirectionalShadowMapSize *
                 kDirectionalShadowMapSize +
             uint64_t{kPointShadowLayers} * kPointShadowMapSize *
                 kPointShadowMapSize +
             uint64_t{kMaxSpotShadowLights} * kSpotShadowMapSize *
                 kSpotShadowMapSize);
        ImGui::TextDisabled("Shared shadow storage: %.1f MiB",
                            static_cast<double>(shadowBytes) /
                                (1024.0 * 1024.0));
        ImGui::TreePop();
    }
    ImGui::EndDisabled();
    ImGui::Separator();

    const CatalogEnvironment *selectedEnvironment =
        sceneRuntime_->selectedEnvironmentId().empty()
            ? nullptr
            : catalog_.findEnvironment(sceneRuntime_->selectedEnvironmentId());
    const char *environmentLabel =
        selectedEnvironment ? selectedEnvironment->displayName.c_str()
                            : "None";
    ImGui::BeginDisabled(!device_->environmentIblSupported());
    if (ImGui::BeginCombo("Environment", environmentLabel)) {
        const bool noneSelected = selectedEnvironment == nullptr;
        if (ImGui::Selectable("None", noneSelected)) {
            try {
                setEnvironment("None");
                editorUi_->environmentError.clear();
            } catch (const std::exception &error) {
                editorUi_->environmentError = error.what();
            }
        }
        if (noneSelected)
            ImGui::SetItemDefaultFocus();
        for (const CatalogEnvironment &environment :
             catalog_.environments) {
            const bool selected =
                sceneRuntime_->selectedEnvironmentId() == environment.id;
            if (ImGui::Selectable(environment.displayName.c_str(),
                                  selected)) {
                try {
                    setEnvironment(environment.id);
                    editorUi_->environmentError.clear();
                } catch (const std::exception &error) {
                    editorUi_->environmentError = error.what();
                }
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    bool iblEnabled = renderSettings_.iblEnabled;
    if (ImGui::Checkbox("Image-Based Lighting", &iblEnabled)) {
        RenderSettingsPatch patch;
        patch.iblEnabled = iblEnabled;
        applyRenderSettings(patch);
    }
    bool skyboxEnabled = renderSettings_.skyboxEnabled;
    if (ImGui::Checkbox("Skybox", &skyboxEnabled)) {
        RenderSettingsPatch patch;
        patch.skyboxEnabled = skyboxEnabled;
        applyRenderSettings(patch);
    }
    float environmentIntensity =
        renderSettings_.environmentIntensity;
    if (ImGui::DragFloat("Environment Intensity",
                         &environmentIntensity, 0.02f, 0.0f, 100.0f)) {
        RenderSettingsPatch patch;
        patch.environmentIntensity = environmentIntensity;
        applyRenderSettings(patch);
    }
    float environmentRotation =
        renderSettings_.environmentRotationRadians;
    if (ImGui::SliderAngle("Environment Rotation",
                           &environmentRotation, -180.0f, 180.0f,
                           "%.1f deg",
                           ImGuiSliderFlags_AlwaysClamp)) {
        RenderSettingsPatch patch;
        patch.environmentRotationRadians = environmentRotation;
        applyRenderSettings(patch);
    }
    ImGui::EndDisabled();
    const AtmosphereRuntimeStatus atmosphereStatus =
        renderer_->atmosphereStatus();
    if (ImGui::TreeNodeEx("Sky Atmosphere")) {
        ImGui::Text("Support: %s",
                    atmosphereStatus.supported ? "Available" : "Unavailable");
        ImGui::Text("Component: %s",
                    atmosphereStatus.componentPresent ? "Present" : "None");
        ImGui::Text("Active: %s",
                    atmosphereStatus.active ? "Yes" : "No");
        if (atmosphereStatus.componentPresent) {
            const std::string componentId =
                atmosphereStatus.componentEntity.toString();
            ImGui::TextWrapped("Component Entity: %s", componentId.c_str());
            if (atmosphereStatus.sunEntity) {
                const std::string sunId =
                    atmosphereStatus.sunEntity->toString();
                ImGui::TextWrapped("Sun Entity: %s", sunId.c_str());
                ImGui::Text("Sun Buffer Index: %d",
                            atmosphereStatus.sunBufferIndex);
            } else {
                ImGui::TextDisabled("Atmosphere Sun: None");
            }
            ImGui::Text("Camera Altitude: %.3f km",
                        atmosphereStatus.cameraAltitudeKm);
            ImGui::Text("Static LUT: %s%s",
                        atmosphereStatus.staticLutReady ? "Ready" : "Pending",
                        atmosphereStatus.staticLutDirty ? " (dirty)" : "");
            ImGui::Text("LUT Generation: %llu",
                        static_cast<unsigned long long>(
                            atmosphereStatus.lutGeneration));
            ImGui::Text("Last Static Update: %.3f ms",
                        atmosphereStatus.lastUpdateMs);
        }
        if (!atmosphereStatus.unavailableReason.empty()) {
            ImGui::TextWrapped("Unavailable: %s",
                               atmosphereStatus.unavailableReason.c_str());
        }
        ImGui::TreePop();
    }
    if (!device_->environmentIblSupported()) {
        ImGui::TextDisabled(
            "IBL unsupported: RGBA16F cube/RG16F linear filtering required.");
    } else if (sceneRuntime_->latestEnvironmentLoadTask() &&
               !isTerminalEnvironmentLoadState(
                   sceneRuntime_->latestEnvironmentLoadTask()->state.load())) {
        ImGui::Text(
            "Environment: %s (%u/%u)",
            environmentLoadStateName(
                sceneRuntime_->latestEnvironmentLoadTask()->state.load()),
            sceneRuntime_->latestEnvironmentLoadTask()->uploadedImages.load(),
            sceneRuntime_->latestEnvironmentLoadTask()->totalImages);
        if (ImGui::Button("Cancel Environment Load")) {
            cancelEnvironmentLoad(sceneRuntime_->latestEnvironmentLoadTask()->id);
        }
    } else {
        ImGui::Text("Environment ready: %s",
                    renderer_->environmentReady() ? "Yes" : "No");
    }
    if (!editorUi_->environmentError.empty()) {
        ImGui::TextWrapped("Environment error: %s",
                           editorUi_->environmentError.c_str());
    }
    if (ImGui::TreeNodeEx("Reflection Probes")) {
        const ReflectionProbeRuntimeStatus probeStatus =
            renderer_->reflectionProbeStatus();
        const EnvironmentAssetRepositorySnapshot repository =
            sceneRuntime_->environmentRepositorySnapshot();
        ImGui::Text("Active: %u / %u", probeStatus.activeCount,
                    probeStatus.limit);
        ImGui::Text("Sources: %u", probeStatus.sourceCount);
        ImGui::Text("Ignored: %u", probeStatus.ignoredCount);
        ImGui::Text("Descriptor generation: %llu",
                    static_cast<unsigned long long>(
                        probeStatus.descriptorGeneration));
        ImGui::Text("Probe metadata buffers: %.2f KiB",
                    static_cast<double>(probeStatus.allocatedBytes) /
                        1024.0);
        ImGui::Text("Environment repository: %llu ready, %llu loading, "
                    "%llu retiring",
                    static_cast<unsigned long long>(repository.readyCount),
                    static_cast<unsigned long long>(repository.loadingCount),
                    static_cast<unsigned long long>(repository.retiringCount));
        if (reflectionProbeCapture_)
            ImGui::TextWrapped("Capture: %s",
                               reflectionProbeCapture_->status.c_str());
        if (probeStatus.ignoredCount > 0) {
            for (const PersistentEntityId &id :
                 probeStatus.ignoredEntityIds) {
                ImGui::BulletText("Ignored %s", id.toString().c_str());
            }
        }
        ImGui::TreePop();
    }
    ImGui::Separator();
    ImGui::ColorEdit3("Ambient Color", &ambientColor_.x);
    ImGui::DragFloat("Ambient Intensity", &ambientIntensity_, 0.01f, 0.0f,
                     10.0f);
    const size_t sceneLightCount = sceneRuntime_->currentWorld() ? sceneRuntime_->currentWorld()->lights().size()
                                                 : 0;
    const size_t effectiveSceneLightCount =
        sceneRuntime_->currentWorld()
            ? static_cast<size_t>(std::count_if(
                  sceneRuntime_->currentWorld()->lights().begin(),
                  sceneRuntime_->currentWorld()->lights().end(), isEffectiveSceneLight))
            : 0;
    if (ImGui::TreeNodeEx("Light Diagnostics")) {
        const SceneLightBufferStatus bufferStatus =
            renderer_->sceneLightBufferStatus();
        ImGui::Text("Scene lights: %zu", sceneLightCount);
        ImGui::Text("Active scene lights: %zu", effectiveSceneLightCount);
        ImGui::Text("Uploaded: %u / %u", lastLightStats_.totalLights,
                    bufferStatus.limit);
        ImGui::Text("Directional %u  Point %u  Spot %u",
                    lastLightStats_.directionalLights,
                    lastLightStats_.pointLights,
                    lastLightStats_.spotLights);
        ImGui::Text("Frame capacity: %u / %u",
                    bufferStatus.frameCapacities[0],
                    bufferStatus.frameCapacities[1]);
        ImGui::Text("Light buffers: %.2f KiB",
                    static_cast<double>(bufferStatus.allocatedBytes) /
                        1024.0);
        if (lastLightStats_.ignoredLights > 0)
            ImGui::Text("Ignored: %u", lastLightStats_.ignoredLights);
        if (!lastLightStats_.shadowCasterKey.empty()) {
            const std::string &casterName =
                lastLightStats_.shadowCasterName.empty()
                    ? lastLightStats_.shadowCasterKey
                    : lastLightStats_.shadowCasterName;
            ImGui::Text("Shadow caster: %s", casterName.c_str());
            ImGui::TextDisabled("Shadow: %s",
                                lastLightStats_.shadowCasterActive
                                    ? "Active"
                                    : "Eligible");
        } else {
            ImGui::TextDisabled("Shadow caster: None");
        }
        if (sceneRuntime_->currentWorld() && !sceneRuntime_->currentWorld()->lights().empty()) {
            const auto &lights = sceneRuntime_->currentWorld()->lights();
            for (size_t index = 0; index < lights.size(); ++index) {
                const SceneLight &light = lights[index];
                const std::string name =
                    light.debugName.empty() ? "Light" : light.debugName;
                const std::string label =
                    std::to_string(index) + "  " + name;
                if (!ImGui::TreeNode(label.c_str()))
                    continue;
                ImGui::Text("Type: %s", lightTypeName(light.type));
                ImGui::Text("Color: %.3f %.3f %.3f", light.color.r,
                            light.color.g, light.color.b);
                ImGui::Text("Intensity: %.3f %s", light.intensity,
                            lightIntensityUnit(light.type));
                if (light.type != LightType::Directional) {
                    ImGui::Text("Position: %.3f %.3f %.3f",
                                light.positionWS.x, light.positionWS.y,
                                light.positionWS.z);
                    if (light.range > 0.0f)
                        ImGui::Text("Range: %.3f", light.range);
                    else
                        ImGui::TextUnformatted("Range: Infinite");
                }
                if (light.type == LightType::Directional) {
                    ImGui::Text("Surface-to-light: %.3f %.3f %.3f",
                                light.directionWS.x, light.directionWS.y,
                                light.directionWS.z);
                } else if (light.type == LightType::Spot) {
                    ImGui::Text("Emission direction: %.3f %.3f %.3f",
                                light.directionWS.x, light.directionWS.y,
                                light.directionWS.z);
                    const float innerAngle = glm::degrees(std::acos(
                        std::clamp(light.innerConeCos, -1.0f, 1.0f)));
                    const float outerAngle = glm::degrees(std::acos(
                        std::clamp(light.outerConeCos, -1.0f, 1.0f)));
                    ImGui::Text("Cone: %.2f / %.2f deg", innerAngle,
                                outerAngle);
                }
                ImGui::TreePop();
            }
        }
        ImGui::TreePop();
    }
    if (sceneLightCount > 0 && effectiveSceneLightCount == 0) {
        ImGui::TextDisabled(
            "All scene lights are disabled; using fallback Sun.");
    }
    if (effectiveSceneLightCount == 0) {
        ImGui::Separator();
        float sunAzimuth = 0.0f;
        float sunElevation = 0.0f;
        sunAnglesFromDirection(defaultSunDirection_, sunAzimuth,
                               sunElevation);
        constexpr ImGuiSliderFlags angleFlags =
            ImGuiSliderFlags_AlwaysClamp;
        bool sunDirectionChanged = ImGui::SliderAngle(
            "Sun Azimuth", &sunAzimuth, -180.0f, 180.0f, "%.1f deg",
            angleFlags);
        sunDirectionChanged |= ImGui::SliderAngle(
            "Sun Elevation", &sunElevation, -89.0f, 89.0f, "%.1f deg",
            angleFlags);
        if (sunDirectionChanged) {
            defaultSunDirection_ =
                sunDirectionFromAngles(sunAzimuth, sunElevation);
        }
        ImGui::ColorEdit3("Sun Color", &defaultSunColor_.x);
        ImGui::DragFloat("Sun Intensity", &defaultSunIntensity_, 0.05f, 0.0f,
                         20.0f);
    }
}

void Application::drawMaterialsPanel() {
    EditorUiState &ui = *editorUi_;
    const MaterialBindingStatus &bindingStatus = materialSystem_->status();
    ImGui::TextDisabled("GPU binding: %s", materialBindingModeName(
                                              bindingStatus.active));
    ImGui::SameLine();
    ImGui::TextDisabled("%u / %u materials", bindingStatus.activeMaterials,
                        bindingStatus.materialCapacity);
    ImGui::Separator();
    std::vector<std::shared_ptr<MaterialInstance>> selectedMaterials;
    const std::vector<std::shared_ptr<MaterialInstance>> *materialsView =
        sceneRuntime_->currentWorld() ? &sceneRuntime_->currentWorld()->materials() : nullptr;
    if (sceneEditorSession_ && sceneEditorSession_->active()) {
        materialsView = &selectedMaterials;
    }
    if (sceneEditorSession_ && sceneEditorSession_->active() &&
        sceneEditorSession_->selection()) {
        const std::shared_ptr<RuntimeWorld> world =
            sceneEditorSession_->world();
        const auto asset = world->modelAsset(
            world->find(*sceneEditorSession_->selection()));
        if (asset) {
            selectedMaterials = asset->materials;
        }
    }
    if (ui.materialSceneGeneration != sceneRuntime_->sceneGeneration()) {
        ui.materialSceneGeneration = sceneRuntime_->sceneGeneration();
        ui.selectedMaterialIndex = 0;
        if (materialsView) {
            const auto &materials = *materialsView;
            const auto firstValid =
                std::find_if(materials.begin(), materials.end(),
                             [](const auto &material) {
                                 return static_cast<bool>(material);
                             });
            if (firstValid != materials.end()) {
                ui.selectedMaterialIndex =
                    static_cast<size_t>(firstValid - materials.begin());
            }
        }
    }

    if (!materialsView) {
        editor::emptyState("No scene is loaded.");
        return;
    }

    const auto &materials = *materialsView;
    ImGui::InputTextWithHint("##MaterialSearch", "Search materials...",
                             ui.materialSearch.data(),
                             ui.materialSearch.size());
    ImGui::SameLine();
    ImGui::TextDisabled("%zu", materials.size());

    const std::string search = ui.materialSearch.data();
    std::vector<size_t> filteredIndices;
    filteredIndices.reserve(materials.size());
    for (size_t i = 0; i < materials.size(); ++i) {
        const auto &material = materials[i];
        const std::string name =
            material && !material->params().debugName.empty()
                ? material->params().debugName
                : "<unnamed>";
        if (asciiContainsIgnoreCase(name, search) ||
            asciiContainsIgnoreCase(std::to_string(i), search))
            filteredIndices.push_back(i);
    }

    const bool selectionVisible =
        std::find(filteredIndices.begin(), filteredIndices.end(),
                  ui.selectedMaterialIndex) != filteredIndices.end();
    if (!selectionVisible && !filteredIndices.empty())
        ui.selectedMaterialIndex = filteredIndices.front();

    const float availableHeight = ImGui::GetContentRegionAvail().y;
    const float listHeight =
        std::clamp(availableHeight * 0.35f, 100.0f, 220.0f);
    ImGui::BeginChild("MaterialList", ImVec2(0.0f, listHeight),
                      ImGuiChildFlags_Borders);
    if (filteredIndices.empty())
        editor::emptyState("No matching materials.");
    for (size_t materialIndex : filteredIndices) {
        const auto &material = materials[materialIndex];
        const std::string name =
            material && !material->params().debugName.empty()
                ? material->params().debugName
                : "<unnamed>";
        const std::string label =
            std::to_string(materialIndex) + "  " + name;
        ImGui::PushID(static_cast<int>(materialIndex));
        if (ImGui::Selectable(label.c_str(),
                              materialIndex == ui.selectedMaterialIndex))
            ui.selectedMaterialIndex = materialIndex;
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (materials.empty() ||
        ui.selectedMaterialIndex >= materials.size()) {
        editor::emptyState("No material selected.");
        return;
    }

    const auto &material = materials[ui.selectedMaterialIndex];
    if (!material) {
        editor::emptyState("Selected material is unavailable.");
        return;
    }

    const MaterialParams &params = material->params();
    auto beginPropertyTable = [](const char *id) {
        return editor::beginPropertyGrid(id, 0.46f);
    };
    auto beginProperty = [](const char *name) {
        editor::propertyLabel(name);
    };

    if (ImGui::CollapsingHeader("Surface",
                                ImGuiTreeNodeFlags_DefaultOpen) &&
        beginPropertyTable("SurfaceProperties")) {
        beginProperty("Name");
        ImGui::TextWrapped("%s", params.debugName.empty()
                                    ? "<unnamed>"
                                    : params.debugName.c_str());
        beginProperty("Index");
        ImGui::Text("%zu", ui.selectedMaterialIndex);
        beginProperty("GPU Material Index");
        ImGui::Text("%u", material->materialIndex());
        beginProperty("Alpha Mode");
        ImGui::TextUnformatted(alphaModeName(params.alphaMode));
        beginProperty("Alpha Cutoff");
        ImGui::Text("%.3f", params.alphaCutoff);
        beginProperty("Double Sided");
        ImGui::TextUnformatted(params.doubleSided ? "true" : "false");
        editor::endPropertyGrid();
    }

    if (ImGui::CollapsingHeader("PBR",
                                ImGuiTreeNodeFlags_DefaultOpen) &&
        beginPropertyTable("PbrProperties")) {
        beginProperty("Base Color");
        ImGui::Text("%.3f %.3f %.3f %.3f", params.baseColorFactor.r,
                    params.baseColorFactor.g, params.baseColorFactor.b,
                    params.baseColorFactor.a);
        beginProperty("Metallic");
        ImGui::Text("%.3f", params.metallicFactor);
        beginProperty("Roughness");
        ImGui::Text("%.3f", params.roughnessFactor);
        beginProperty("Normal Scale");
        ImGui::Text("%.3f", params.normalScale);
        beginProperty("Occlusion Strength");
        ImGui::Text("%.3f", params.occlusionStrength);
        beginProperty("Occlusion UV");
        ImGui::Text("%u", params.occlusionTexCoord);
        beginProperty("Emissive");
        ImGui::Text("%.3f %.3f %.3f", params.emissiveFactor.r,
                    params.emissiveFactor.g, params.emissiveFactor.b);
        beginProperty("Emissive Strength");
        ImGui::Text("%.3f", params.emissiveStrength);
        beginProperty("Transmission");
        ImGui::Text("%.3f", params.transmissionFactor);
        editor::endPropertyGrid();
    }

    if (ImGui::CollapsingHeader("Textures",
                                ImGuiTreeNodeFlags_DefaultOpen) &&
        beginPropertyTable("TextureProperties")) {
        const auto &textures = material->textures();
        const GpuMaterial *gpuMaterial =
            materialSystem_->gpuMaterial(material->materialHandle());
        const std::array<uint32_t, kMaterialTextureSlotCount> gpuSlots{
            gpuMaterial ? gpuMaterial->textureIndices0.x : 0u,
            gpuMaterial ? gpuMaterial->textureIndices0.y : 0u,
            gpuMaterial ? gpuMaterial->textureIndices0.z : 0u,
            gpuMaterial ? gpuMaterial->textureIndices0.w : 0u,
            gpuMaterial ? gpuMaterial->textureIndices1.x : 0u};
        for (size_t slotIndex = 0; slotIndex < kMaterialTextureSlotCount;
             ++slotIndex) {
            const auto slot =
                static_cast<MaterialTextureSlot>(slotIndex);
            beginProperty(slotName(slot));
            if (!textures[slotIndex]) {
                ImGui::TextUnformatted("Missing");
            } else if (bindingStatus.active ==
                       MaterialBindingMode::Bindless) {
                ImGui::Text("Bound (slot %u)", gpuSlots[slotIndex]);
            } else {
                ImGui::Text("Bound (fixed binding %zu)", slotIndex + 1);
            }
        }
        editor::endPropertyGrid();
    }

    if (ImGui::CollapsingHeader("Derived Render State",
                                ImGuiTreeNodeFlags_DefaultOpen) &&
        beginPropertyTable("DerivedProperties")) {
        beginProperty("Render Queue");
        ImGui::TextUnformatted(
            isTransparentMaterial(params) ? "Transparent" : "Opaque");
        beginProperty("Cull");
        ImGui::TextUnformatted(params.doubleSided ? "None" : "Back");
        editor::endPropertyGrid();
    }
}

void Application::drawCameraPanel() {
    const auto cameraPos = camera_.position();
    ImGui::Text("Position: (%.2f, %.2f, %.2f)", cameraPos.x, cameraPos.y,
                cameraPos.z);
    constexpr ImGuiSliderFlags moveSpeedFlags =
        ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp;
    ImGui::SliderFloat("Move Speed", &config_.moveSpeed, 0.1f, 100.0f,
                       "%.2f", moveSpeedFlags);
    float nearPlane = camera_.nearPlane();
    float farPlane = camera_.farPlane();
    bool  clipChanged = false;
    clipChanged |= ImGui::DragFloat("Near Plane", &nearPlane, 0.001f, 0.001f,
                                    100.0f, "%.4f");
    clipChanged |= ImGui::DragFloat("Far Plane", &farPlane, 0.1f, 1.0f,
                                    100000.0f, "%.2f");
    if (clipChanged)
        camera_.setClipPlanes(nearPlane, farPlane);

    if (sceneRuntime_->currentWorld() && sceneRuntime_->currentWorld()->bounds().valid) {
        const Bounds &bounds = sceneRuntime_->currentWorld()->bounds();
        ImGui::Separator();
        ImGui::Text("Bounds Center: (%.2f, %.2f, %.2f)", bounds.center.x,
                    bounds.center.y, bounds.center.z);
        ImGui::Text("Bounds Radius: %.2f", bounds.radius);
    } else {
        ImGui::Separator();
        ImGui::Text("Bounds: unavailable");
    }
}

void Application::drawPerformancePanel() {
    if (editor::beginPropertyGrid("PerformanceSummary", 0.36f)) {
        editor::propertyLabel("FPS");
        ImGui::Text("%.1f", ImGui::GetIO().Framerate);
        editor::propertyLabel("Input Mode");
        ImGui::TextUnformatted(mode_ == InputMode::UI ? "UI"
                                                     : "CameraDrag");
        editor::propertyLabel("Objects");
        ImGui::Text("%zu", sceneRuntime_->currentWorld() ? sceneRuntime_->currentWorld()->renderableCount()
                                         : 0);
        editor::propertyLabel("Tracy");
        if (build::kTracy) {
            const bool connected = device_->tracyProfiler().connected();
            editor::statusIndicator(
                connected ? "Connected" : "Waiting",
                connected ? editor::StatusTone::Success
                          : editor::StatusTone::Neutral,
                device_->tracyProfiler().gpuAvailable()
                    ? "Vulkan GPU profiling available"
                    : "Vulkan GPU profiling unavailable");
        } else {
            ImGui::TextDisabled("Not compiled");
        }
        editor::endPropertyGrid();
    }

    const EditorUiState &ui = *editorUi_;
    if (ui.performanceCount > 1) {
        std::array<float, EditorUiState::kPerformanceHistorySize> fps{};
        std::array<float, EditorUiState::kPerformanceHistorySize> gpu{};
        const size_t start =
            (ui.performanceCursor + EditorUiState::kPerformanceHistorySize -
             ui.performanceCount) %
            EditorUiState::kPerformanceHistorySize;
        for (size_t i = 0; i < ui.performanceCount; ++i) {
            const size_t source =
                (start + i) % EditorUiState::kPerformanceHistorySize;
            fps[i] = ui.fpsHistory[source];
            gpu[i] = ui.gpuHistory[source];
        }
        const float maxFps = std::max(
            60.0f, *std::max_element(fps.begin(),
                                     fps.begin() + ui.performanceCount));
        const float maxGpu = std::max(
            1.0f, *std::max_element(gpu.begin(),
                                    gpu.begin() + ui.performanceCount));
        ImGui::PlotLines("FPS History", fps.data(),
                         static_cast<int>(ui.performanceCount), 0, nullptr,
                         0.0f, maxFps * 1.1f, ImVec2(0.0f, 54.0f));
        ImGui::PlotLines("GPU ms History", gpu.data(),
                         static_cast<int>(ui.performanceCount), 0, nullptr,
                         0.0f, maxGpu * 1.1f, ImVec2(0.0f, 54.0f));
    }

    if (ImGui::CollapsingHeader("GPU Pass Timings",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        const GpuPassTimings &timings = renderer_->gpuPassTimings();
        if (!build::kGpuProfiling) {
            ImGui::TextUnformatted("Not compiled");
        } else if (!timings.available) {
            ImGui::TextUnformatted("Unavailable");
        } else {
            ImGui::Text("Frame Serial: %llu",
                        static_cast<unsigned long long>(
                            timings.frameSerial));
            if (editor::beginPropertyGrid("GpuPassBreakdown", 0.42f)) {
                for (const GpuPassTiming &pass : timings.passes) {
                    editor::propertyLabel(pass.name.c_str());
                    const float fraction =
                        timings.totalMs > 0.0
                            ? static_cast<float>(pass.milliseconds /
                                                 timings.totalMs)
                            : 0.0f;
                    char overlay[48]{};
                    std::snprintf(overlay, sizeof(overlay), "%.3f ms",
                                  pass.milliseconds);
                    ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f),
                                       ImVec2(-1.0f, 0.0f), overlay);
                }
                editor::propertyLabel("Total");
                ImGui::Text("%.3f ms", timings.totalMs);
                editor::endPropertyGrid();
            }
        }
    }

    if (ImGui::CollapsingHeader("Render Graph",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        const RenderGraphDiagnostics &graph =
            renderer_->renderGraphDiagnostics();
        if (editor::beginPropertyGrid("RenderGraphSummary", 0.46f)) {
            editor::propertyLabel("Topology");
            ImGui::Text("%016llx",
                        static_cast<unsigned long long>(graph.topologyHash));
            editor::propertyLabel("Passes");
            ImGui::Text("%u active / %u culled", graph.activePasses,
                        graph.culledPasses);
            editor::propertyLabel("Dependencies");
            ImGui::Text("%u", graph.dependencyEdges);
            editor::propertyLabel("Auto Barriers");
            ImGui::Text("%u (%u layout, %u hazard)",
                        graph.automaticBarriers, graph.layoutBarriers,
                        graph.hazardBarriers);
            editor::propertyLabel("Image Memory");
            ImGui::Text("%.1f / %.1f MiB active/resident",
                        graph.activeImageBytes / (1024.0 * 1024.0),
                        graph.residentImageBytes / (1024.0 * 1024.0));
            editor::propertyLabel("Declared / Retiring");
            ImGui::Text("%.1f / %.1f MiB",
                        graph.logicalImageBytes / (1024.0 * 1024.0),
                        graph.retiringImageBytes / (1024.0 * 1024.0));
            editor::endPropertyGrid();
        }
        if (ImGui::TreeNode("Execution Order")) {
            for (uint32_t index = 0; index < graph.executionOrder.size();
                 ++index) {
                ImGui::Text("%02u  %s", index,
                            graph.executionOrder[index].c_str());
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Culled Passes")) {
            for (const std::string &name : graph.culledNames)
                ImGui::TextDisabled("%s", name.c_str());
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Resources")) {
            for (const auto &resource : graph.resources) {
                ImGui::PushID(static_cast<int>(resource.index));
                if (ImGui::TreeNode(
                        "Resource", "%s  [%s, v%u]",
                        resource.name.c_str(), resource.lifetime.c_str(),
                        resource.versions)) {
                    ImGui::Text("Memory: %.2f MiB",
                                resource.residentBytes /
                                    (1024.0 * 1024.0));
                    ImGui::Text("Layout: %d -> %d",
                                static_cast<int>(resource.initialLayout),
                                static_cast<int>(resource.finalLayout));
                    ImGui::TextUnformatted("Producers:");
                    for (const std::string &producer : resource.producers)
                        ImGui::BulletText("%s", producer.c_str());
                    ImGui::TextUnformatted("Consumers:");
                    for (const std::string &consumer : resource.consumers)
                        ImGui::BulletText("%s", consumer.c_str());
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Buffers")) {
            for (const auto &buffer : graph.buffers) {
                ImGui::PushID(static_cast<int>(buffer.index));
                ImGui::PushID("RenderGraphBuffer");
                if (ImGui::TreeNode(
                        "Buffer", "%s  [%s, v%u]", buffer.name.c_str(),
                        buffer.lifetime.c_str(), buffer.versions)) {
                    if (buffer.declaredRangeBytes > 0) {
                        ImGui::Text("Declared range: %.2f KiB",
                                    buffer.declaredRangeBytes / 1024.0);
                    } else {
                        ImGui::TextDisabled("Declared range: whole buffer");
                    }
                    ImGui::TextUnformatted("Producers:");
                    for (const std::string &producer : buffer.producers)
                        ImGui::BulletText("%s", producer.c_str());
                    ImGui::TextUnformatted("Consumers:");
                    for (const std::string &consumer : buffer.consumers)
                        ImGui::BulletText("%s", consumer.c_str());
                    ImGui::TreePop();
                }
                ImGui::PopID();
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        if (ImGui::Button("Export JSON")) {
            std::filesystem::create_directories("logs");
            std::ofstream("logs/render_graph.json", std::ios::binary)
                << renderer_->renderGraphJson();
        }
        ImGui::SameLine();
        if (ImGui::Button("Export DOT")) {
            std::filesystem::create_directories("logs");
            std::ofstream("logs/render_graph.dot", std::ios::binary)
                << renderer_->renderGraphDot();
        }
    }
}

void Application::drawLoadStatsPanel() {
    if (sceneRuntime_->lastSceneLoadStats() &&
        ImGui::CollapsingHeader("Last Scene Load",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        const SceneLoadStats &stats = *sceneRuntime_->lastSceneLoadStats();
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
            ImGui::Text("Task: %llu  Operation Generation: %llu",
                        static_cast<unsigned long long>(stats.taskId),
                        static_cast<unsigned long long>(stats.generation));
            ImGui::Text("Model Generation: %llu  Repository: %s%s",
                        static_cast<unsigned long long>(
                            stats.modelGeneration),
                        stats.repositoryHit ? "Ready hit" : "Build",
                        stats.coalescedRequest ? " / Coalesced" : "");
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
        ImGui::Text("Native BC7 Read: %.2f ms",
                    resources.nativeTextureReadMs);
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
        ImGui::Text("Native BC7: %llu  UASTC: %llu  Transcodes: %llu",
                    static_cast<unsigned long long>(
                        resources.nativeBc7CacheHits),
                    static_cast<unsigned long long>(
                        resources.basisUastcCacheHits),
                    static_cast<unsigned long long>(
                        resources.basisTranscodeCount));
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
        ImGui::Text("Materials: %llu  Primitives: %llu",
                    static_cast<unsigned long long>(stats.materialCount),
                    static_cast<unsigned long long>(stats.primitiveCount));
        ImGui::Text(
            "Lights: %llu instances / %llu definitions "
            "(%llu directional, %llu point, %llu spot)",
            static_cast<unsigned long long>(stats.lightInstanceCount),
            static_cast<unsigned long long>(
                stats.gltfLightDefinitionCount),
            static_cast<unsigned long long>(
                stats.directionalLightCount),
            static_cast<unsigned long long>(stats.pointLightCount),
            static_cast<unsigned long long>(stats.spotLightCount));
        ImGui::Text("Texture bytes: encoded %.2f, decoded %.2f MiB",
                    bytesToMiB(resources.encodedSourceBytes),
                    bytesToMiB(resources.decodedRgbaBytes));
        ImGui::Text("Texture upload: %.2f MiB  GPU estimate: %.2f MiB",
                    bytesToMiB(resources.textureUploadBytes),
                    bytesToMiB(resources.textureGpuBytesEstimated));
        ImGui::Text("Native BC7 read: %.2f MiB",
                    bytesToMiB(resources.nativeTextureReadBytes));
        ImGui::Text("Mesh upload: %.2f MiB",
                    bytesToMiB(resources.vertexUploadBytes +
                               resources.indexUploadBytes));
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
    } else if (!sceneRuntime_->lastSceneLoadStats()) {
        ImGui::TextDisabled("No scene load statistics are available.");
    }

    if (ImGui::CollapsingHeader("Material Resources",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        const MaterialBindingStatus &status = materialSystem_->status();
        ImGui::Text("Requested %s  Active %s",
                    materialBindingModeName(status.requested),
                    materialBindingModeName(status.active));
        if (!status.fallbackReason.empty())
            ImGui::TextWrapped("Fallback: %s", status.fallbackReason.c_str());
        ImGui::Text("Textures %u / %u  Retiring %u", status.activeTextures,
                    status.textureCapacity, status.retiringTextures);
        ImGui::Text("Materials %u / %u  Retiring %u", status.activeMaterials,
                    status.materialCapacity, status.retiringMaterials);
        ImGui::Text("High water: textures %u  materials %u",
                    status.textureHighWaterMark,
                    status.materialHighWaterMark);
        ImGui::Text("Descriptor writes %llu  Slot reuse T/M %llu / %llu",
                    static_cast<unsigned long long>(status.descriptorWrites),
                    static_cast<unsigned long long>(status.textureSlotReuses),
                    static_cast<unsigned long long>(status.materialSlotReuses));
        ImGui::Text("Capacity failures T/M %llu / %llu",
                    static_cast<unsigned long long>(
                        status.textureCapacityFailures),
                    static_cast<unsigned long long>(
                        status.materialCapacityFailures));
    }

    if (ImGui::CollapsingHeader("Model Asset Repository",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        const AssetRepositorySnapshot snapshot =
            sceneRuntime_->modelRepositorySnapshot();
        ImGui::Text("Ready %llu  Loading %llu  Failed %llu  Retiring %llu",
                    static_cast<unsigned long long>(snapshot.readyCount),
                    static_cast<unsigned long long>(snapshot.loadingCount),
                    static_cast<unsigned long long>(snapshot.failedCount),
                    static_cast<unsigned long long>(snapshot.retiringCount));
        ImGui::Text("CPU prepares %llu  GPU builds %llu",
                    static_cast<unsigned long long>(
                        snapshot.cpuPrepareStarts),
                    static_cast<unsigned long long>(snapshot.gpuBuildStarts));
        ImGui::Text("Ready hits %llu  Coalesced requests %llu",
                    static_cast<unsigned long long>(snapshot.readyHits),
                    static_cast<unsigned long long>(
                        snapshot.coalescedRequests));
        if (ImGui::BeginTable(
                "ModelAssetRepositoryRecords", 5,
                ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Model");
            ImGui::TableSetupColumn("Profile");
            ImGui::TableSetupColumn("Gen");
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("Users");
            ImGui::TableHeadersRow();
            for (const ModelAssetRecordSnapshot &record : snapshot.records) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(record.key.modelId.value().c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(record.key.profileId.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%llu", static_cast<unsigned long long>(
                                        record.generation));
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(modelAssetStateName(record.state));
                if (!record.error.empty() && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", record.error.c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%llu", static_cast<unsigned long long>(
                                        record.consumerCount));
            }
            ImGui::EndTable();
        }
    }
}

void Application::saveEditorScene() {
    if (!sceneEditorSession_ || !sceneEditorSession_->active())
        return;
    try {
        sceneEditorSession_->save(projectContext_.projectRoot,
                                  catalog_.documentReferences());
        editorUi_->sceneStatus = "Scene saved";
        editorUi_->sceneError.clear();
    } catch (const std::exception &error) {
        if (std::string(error.what()) == "scene_changed_on_disk")
            editorUi_->requestSceneConflictModal = true;
        else
            editorUi_->sceneError = error.what();
    }
}

void Application::executePendingEditorAction(bool saveFirst) {
    if (saveFirst) {
        saveEditorScene();
        if (hasUnsavedSceneChanges() ||
            editorUi_->requestSceneConflictModal)
            return;
    }
    const EditorPendingActionKind action = editorUi_->pendingAction;
    const int sceneIndex = editorUi_->pendingSceneIndex;
    editorUi_->pendingAction = EditorPendingActionKind::None;
    editorUi_->pendingSceneIndex = -1;
    editorUi_->requestDirtyModal = false;
    if (!saveFirst && sceneEditorSession_ && sceneEditorSession_->active())
        sceneEditorSession_->discardChanges();

    try {
        switch (action) {
        case EditorPendingActionKind::None:
            break;
        case EditorPendingActionKind::NewScene:
            editorUi_->requestNewSceneModal = true;
            break;
        case EditorPendingActionKind::LoadScene:
            requestSceneOperation(sceneIndex);
            break;
        case EditorPendingActionKind::CloseScene:
            if (sceneEditorSession_)
                sceneEditorSession_->detach();
            sceneRuntime_->closeWorld();
            shadowSystem_.reset();
            break;
        case EditorPendingActionKind::Quit:
            if (sceneEditorSession_)
                sceneEditorSession_->detach();
            editorUi_->quitConfirmed = true;
            window_->setShouldClose(true);
            break;
        }
    } catch (const std::exception &error) {
        editorUi_->sceneError = error.what();
    }
}

void Application::drawSceneAuthoringDialogs() {
    EditorUiState &ui = *editorUi_;
    if (ui.requestNewSceneModal) {
        ui.requestNewSceneModal = false;
        std::snprintf(ui.sceneDisplayName.data(), ui.sceneDisplayName.size(),
                      "%s", "New Scene");
        std::snprintf(ui.sceneId.data(), ui.sceneId.size(), "%s",
                      "new-scene");
        ImGui::OpenPopup("New Scene");
    }
    if (ImGui::BeginPopupModal("New Scene", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Name", ui.sceneDisplayName.data(),
                         ui.sceneDisplayName.size());
        ImGui::InputText("Scene ID", ui.sceneId.data(), ui.sceneId.size());
        const bool valid = ui.sceneDisplayName[0] != '\0' &&
                           isStableAssetId(ui.sceneId.data());
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Create")) {
            try {
                const std::string id = ui.sceneId.data();
                const std::string name = ui.sceneDisplayName.data();
                const std::filesystem::path relative =
                    std::filesystem::path("assets/scenes") /
                    (id + ".vkscene.json");
                const std::filesystem::path path =
                    projectContext_.resolveProjectPath(relative);
                std::filesystem::create_directories(path.parent_path());
                const SceneDocument document =
                    SceneDocumentService::createDefault(id, name);
                SceneDocumentService::saveAtomic(
                    path, projectContext_.projectRoot, document,
                    std::nullopt);
                try {
                    SceneCatalogStore::addSceneDocument(
                        projectContext_, {id, name, relative, false});
                } catch (...) {
                    std::error_code ignored;
                    std::filesystem::remove(path, ignored);
                    throw;
                }
                refreshSceneRegistry(id);
                const int index = sceneWorkflow_->findEntryById(id);
                requestSceneOperation(index);
                ui.sceneStatus = "Created " + name;
                ui.sceneError.clear();
                ImGui::CloseCurrentPopup();
            } catch (const std::exception &error) {
                ui.sceneError = error.what();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ui.requestOpenSceneModal) {
        ui.requestOpenSceneModal = false;
        ui.openSceneIndex = -1;
        ImGui::OpenPopup("Open Scene");
    }
    if (ImGui::BeginPopupModal("Open Scene", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::BeginChild("OpenSceneList", ImVec2(420.0f, 260.0f),
                          ImGuiChildFlags_Borders);
        for (int index = 0; index < static_cast<int>(sceneRegistry_.size());
             ++index) {
            const SceneEntry &entry = sceneRegistry_[index];
            if (!entry.isNativeScene())
                continue;
            ImGui::BeginDisabled(!entry.available);
            if (ImGui::Selectable(entry.name.c_str(),
                                  ui.openSceneIndex == index))
                ui.openSceneIndex = index;
            ImGui::EndDisabled();
        }
        ImGui::EndChild();
        ImGui::BeginDisabled(ui.openSceneIndex < 0);
        if (ImGui::Button("Open")) {
            requestEditorSceneLoad(ui.openSceneIndex);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ui.requestSaveAsModal) {
        ui.requestSaveAsModal = false;
        const auto world = sceneEditorSession_ ? sceneEditorSession_->world()
                                               : nullptr;
        std::snprintf(ui.sceneDisplayName.data(), ui.sceneDisplayName.size(),
                      "%s", world ? world->displayName().c_str() : "Scene");
        std::snprintf(ui.sceneId.data(), ui.sceneId.size(), "%s",
                      world ? (world->id().value() + "-copy").c_str()
                            : "scene-copy");
        ImGui::OpenPopup("Save Scene As");
    }
    if (ImGui::BeginPopupModal("Save Scene As", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Name", ui.sceneDisplayName.data(),
                         ui.sceneDisplayName.size());
        ImGui::InputText("Scene ID", ui.sceneId.data(), ui.sceneId.size());
        const bool valid = sceneEditorSession_ &&
                           sceneEditorSession_->active() &&
                           ui.sceneDisplayName[0] != '\0' &&
                           isStableAssetId(ui.sceneId.data());
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Save As")) {
            try {
                const std::string id = ui.sceneId.data();
                const std::string name = ui.sceneDisplayName.data();
                const std::filesystem::path relative =
                    std::filesystem::path("assets/scenes") /
                    (id + ".vkscene.json");
                const std::filesystem::path path =
                    projectContext_.resolveProjectPath(relative);
                std::filesystem::create_directories(path.parent_path());
                SceneDocument document =
                    sceneEditorSession_->world()->toDocument();
                document.id = SceneDocumentId(id);
                document.displayName = name;
                const SceneDocumentReferences references =
                    catalog_.documentReferences();
                SceneDocumentService::validate(document, &references);
                const SceneDocumentFileStamp stamp =
                    SceneDocumentService::saveAtomic(
                        path, projectContext_.projectRoot, document,
                        std::nullopt);
                try {
                    SceneCatalogStore::addSceneDocument(
                        projectContext_, {id, name, relative, false});
                } catch (...) {
                    std::error_code ignored;
                    std::filesystem::remove(path, ignored);
                    throw;
                }
                const auto world = sceneEditorSession_->world();
                world->replaceDocument(document);
                sceneEditorSession_->attach(world, path, stamp);
                sceneEditorSession_->setWorldChangedCallback(
                    [this]() {
                        updateEditorModelBindings();
                        updateEditorReflectionProbeBindings();
                    });
                refreshSceneRegistry(id);
                sceneRuntime_->adoptCurrentSceneIndex(
                    sceneWorkflow_->findEntryById(id));
                ui.sceneStatus = "Saved as " + name;
                ui.sceneError.clear();
                ImGui::CloseCurrentPopup();
            } catch (const std::exception &error) {
                ui.sceneError = error.what();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ui.requestConvertPreviewModal) {
        ui.requestConvertPreviewModal = false;
        const SceneEntry *entry =
            sceneRuntime_->currentSceneIndex() >= 0
                ? &sceneRegistry_[sceneRuntime_->currentSceneIndex()]
                : nullptr;
        const std::string baseId = entry ? entry->id + "-scene" : "scene";
        const std::string baseName =
            entry ? entry->name + " Scene" : "Converted Scene";
        std::snprintf(ui.sceneDisplayName.data(), ui.sceneDisplayName.size(),
                      "%s", baseName.c_str());
        std::snprintf(ui.sceneId.data(), ui.sceneId.size(), "%s",
                      baseId.c_str());
        ImGui::OpenPopup("Convert Model Preview");
    }
    if (ImGui::BeginPopupModal("Convert Model Preview", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Name", ui.sceneDisplayName.data(),
                         ui.sceneDisplayName.size());
        ImGui::InputText("Scene ID", ui.sceneId.data(), ui.sceneId.size());
        const bool previewValid =
            sceneRuntime_->currentSceneIndex() >= 0 &&
            sceneRuntime_->currentSceneIndex() < static_cast<int>(sceneRegistry_.size()) &&
            sceneRegistry_[sceneRuntime_->currentSceneIndex()].isModelPreview();
        const bool valid = previewValid &&
                           ui.sceneDisplayName[0] != '\0' &&
                           isStableAssetId(ui.sceneId.data());
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Convert")) {
            try {
                const SceneEntry preview = sceneRegistry_[sceneRuntime_->currentSceneIndex()];
                const std::string id = ui.sceneId.data();
                const std::string name = ui.sceneDisplayName.data();
                SceneDocument document =
                    SceneDocumentService::createDefault(id, name);
                SceneEntityDocument model;
                model.id = PersistentEntityId::generate();
                model.name = preview.name;
                model.modelInstance =
                    ModelInstanceDocument{ModelAssetId(preview.id)};
                document.entities.push_back(std::move(model));
                for (SceneEntityDocument &entity : document.entities) {
                    if (entity.camera) {
                        entity.transform.translation = camera_.position();
                        entity.transform.rotation = rotationLookingAlong(
                            camera_.front(), camera_.up());
                        break;
                    }
                }
                const std::filesystem::path relative =
                    std::filesystem::path("assets/scenes") /
                    (id + ".vkscene.json");
                const std::filesystem::path path =
                    projectContext_.resolveProjectPath(relative);
                std::filesystem::create_directories(path.parent_path());
                SceneDocumentService::saveAtomic(
                    path, projectContext_.projectRoot, document,
                    std::nullopt);
                try {
                    SceneCatalogStore::addSceneDocument(
                        projectContext_, {id, name, relative, false});
                } catch (...) {
                    std::error_code ignored;
                    std::filesystem::remove(path, ignored);
                    throw;
                }
                refreshSceneRegistry(id);
                requestEditorSceneLoad(sceneWorkflow_->findEntryById(id));
                ui.sceneStatus = "Converted preview to " + name;
                ImGui::CloseCurrentPopup();
            } catch (const std::exception &error) {
                ui.sceneError = error.what();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ui.requestDirtyModal) {
        ui.requestDirtyModal = false;
        ImGui::OpenPopup("Unsaved Scene Changes");
    }
    if (ImGui::BeginPopupModal("Unsaved Scene Changes", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "The current scene has unsaved changes. Save them before "
            "continuing?");
        if (ImGui::Button("Save")) {
            executePendingEditorAction(true);
            if (editorUi_->requestSceneConflictModal) {
                editorUi_->pendingAction = EditorPendingActionKind::None;
                editorUi_->pendingSceneIndex = -1;
                ImGui::CloseCurrentPopup();
            } else if (!hasUnsavedSceneChanges()) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            executePendingEditorAction(false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ui.pendingAction = EditorPendingActionKind::None;
            ui.pendingSceneIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ui.requestSceneConflictModal) {
        ui.requestSceneConflictModal = false;
        ImGui::OpenPopup("Scene Changed On Disk");
    }
    if (ImGui::BeginPopupModal("Scene Changed On Disk", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "The scene file changed outside VulkanLab. Reload it or save "
            "the current work under a new scene ID.");
        if (ImGui::Button("Reload")) {
            try {
                sceneEditorSession_->reload(
                    projectContext_.projectRoot,
                    catalog_.documentReferences());
                ImGui::CloseCurrentPopup();
            } catch (const std::exception &error) {
                ui.sceneError = error.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Save As")) {
            ui.requestSaveAsModal = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void Application::deleteSelectedEditorEntity() {
    if (!sceneEditorSession_ || !sceneEditorSession_->active() ||
        !sceneEditorSession_->selection())
        return;
    const PersistentEntityId id = *sceneEditorSession_->selection();
    try {
        if (sceneEditorSession_->execute(
                 "Delete Entity", [id](RuntimeWorld &world) {
                    const auto selected = world.entity(world.find(id));
                    if (selected && selected->atmosphere) {
                        for (const RuntimeEntitySnapshot &entity :
                             world.entities()) {
                            if (!entity.light ||
                                !entity.light->atmosphereSunIndex)
                                continue;
                            LightComponentDocument light = *entity.light;
                            light.atmosphereSunIndex.reset();
                            if (!world.setLight(world.find(entity.id), light))
                                return false;
                        }
                    }
                     return world.destroyEntity(world.find(id), true);
                 })) {
            sceneEditorSession_->select(std::nullopt);
        }
    } catch (const std::exception &error) {
        editorUi_->sceneError = error.what();
    }
}

void Application::duplicateSelectedEditorEntity() {
    if (!sceneEditorSession_ || !sceneEditorSession_->active() ||
        !sceneEditorSession_->selection())
        return;
    const PersistentEntityId rootId = *sceneEditorSession_->selection();
    try {
        if (const auto selected = sceneEditorSession_->world()->entity(
                sceneEditorSession_->world()->find(rootId));
            selected && (selected->atmosphere || selected->ddgiProbeVolume)) {
            editorUi_->sceneError = selected->atmosphere
                                        ? "Sky Atmosphere is a singleton and cannot be duplicated."
                                        : "DDGI Probe Volume is a singleton and cannot be duplicated.";
            return;
        }
        PersistentEntityId duplicateRoot;
        sceneEditorSession_->execute(
            "Duplicate Entity", [&](RuntimeWorld &world) {
                const std::vector<RuntimeEntitySnapshot> entities =
                    world.entities();
                std::unordered_set<PersistentEntityId,
                                   PersistentEntityIdHash>
                    subtree{rootId};
                bool changed = true;
                while (changed) {
                    changed = false;
                    for (const RuntimeEntitySnapshot &entity : entities) {
                        if (entity.parent &&
                            subtree.count(*entity.parent) != 0 &&
                            subtree.insert(entity.id).second) {
                            changed = true;
                        }
                    }
                }
                std::unordered_map<PersistentEntityId, PersistentEntityId,
                                   PersistentEntityIdHash>
                    remap;
                for (const RuntimeEntitySnapshot &source : entities) {
                    if (subtree.count(source.id) == 0)
                        continue;
                    SceneEntityDocument copy;
                    copy.id = PersistentEntityId::generate();
                    copy.name = source.id == rootId
                                    ? source.name + " Copy"
                                    : source.name;
                    copy.enabled = source.enabled;
                    copy.transform = source.transform;
                    copy.modelInstance = source.modelInstance;
                    copy.light = source.light;
                    if (copy.light)
                        copy.light->atmosphereSunIndex.reset();
                    copy.camera = source.camera;
                    copy.reflectionProbe = source.reflectionProbe;
                    copy.ddgiProbeVolume = source.ddgiProbeVolume;
                    remap[source.id] = copy.id;
                    if (source.id == rootId)
                        duplicateRoot = copy.id;
                    world.createEntity(std::move(copy));
                }
                for (const RuntimeEntitySnapshot &source : entities) {
                    if (subtree.count(source.id) == 0)
                        continue;
                    std::optional<EntityHandle> parent;
                    if (source.parent) {
                        const auto remapped = remap.find(*source.parent);
                        parent = remapped != remap.end()
                                     ? world.find(remapped->second)
                                     : world.find(*source.parent);
                    }
                    if (!world.setParent(world.find(remap.at(source.id)),
                                         parent)) {
                        return false;
                    }
                }
                return true;
            });
        if (!duplicateRoot.empty())
            sceneEditorSession_->select(duplicateRoot);
    } catch (const std::exception &error) {
        editorUi_->sceneError = error.what();
    }
}

void Application::drawOutlinerPanel() {
    if (!outlinerPanel_ || !sceneEditorSession_ ||
        !sceneEditorSession_->active()) {
        ImGui::TextDisabled("Open a native scene to edit entities.");
        return;
    }
    const std::shared_ptr<RuntimeWorld> world = sceneEditorSession_->world();
    OutlinerPanelSnapshot snapshot;
    snapshot.editable = projectContext_.catalogWritable;
    const std::vector<RuntimeEntitySnapshot> entities = world->entities();
    snapshot.canCreateAtmosphere = std::none_of(
        entities.begin(), entities.end(),
        [](const RuntimeEntitySnapshot &entity) {
            return entity.atmosphere.has_value();
        });
    snapshot.canCreateDdgiProbeVolume = std::none_of(
        entities.begin(), entities.end(),
        [](const RuntimeEntitySnapshot &entity) {
            return entity.ddgiProbeVolume.has_value();
        });
    std::unordered_set<PersistentEntityId, PersistentEntityIdHash>
        lightLimitExceeded;
    lightLimitExceeded.insert(lastLightStats_.ignoredEntityIds.begin(),
                              lastLightStats_.ignoredEntityIds.end());
    for (const RuntimeEntitySnapshot &entity : entities) {
        snapshot.entities.push_back(
            {entity.id, entity.parent, entity.name, entity.enabled,
             sceneEditorSession_->selection() &&
                 *sceneEditorSession_->selection() == entity.id,
             entity.modelBindingState, entity.modelInstance.has_value(),
             lightLimitExceeded.count(entity.id) != 0,
             entity.atmosphere.has_value(),
             entity.reflectionProbeBindingState,
             entity.reflectionProbe.has_value(),
             entity.ddgiProbeVolume.has_value()});
    }

    OutlinerPanelActions actions;
    actions.select = [this](std::optional<PersistentEntityId> id) {
        sceneEditorSession_->select(std::move(id));
    };
    actions.create = [this](OutlinerCreateKind kind,
                            std::optional<PersistentEntityId> parentId) {
        try {
            SceneEntityDocument entity;
            entity.id = PersistentEntityId::generate();
            const PrimitiveModelDefinition *primitive = nullptr;
            switch (kind) {
            case OutlinerCreateKind::Empty:
                entity.name = "Entity";
                break;
            case OutlinerCreateKind::Model:
                entity.name = "Model";
                break;
            case OutlinerCreateKind::Plane:
                primitive = findPrimitiveModel(PrimitiveType::Plane);
                break;
            case OutlinerCreateKind::Cube:
                primitive = findPrimitiveModel(PrimitiveType::Cube);
                break;
            case OutlinerCreateKind::Sphere:
                primitive = findPrimitiveModel(PrimitiveType::Sphere);
                break;
            case OutlinerCreateKind::Cylinder:
                primitive = findPrimitiveModel(PrimitiveType::Cylinder);
                break;
            case OutlinerCreateKind::Cone:
                primitive = findPrimitiveModel(PrimitiveType::Cone);
                break;
            case OutlinerCreateKind::Capsule:
                primitive = findPrimitiveModel(PrimitiveType::Capsule);
                break;
            case OutlinerCreateKind::DirectionalLight:
                entity.name = "Directional Light";
                entity.light = LightComponentDocument{};
                entity.light->type = SceneDocumentLightType::Directional;
                entity.light->intensity = 3.0f;
                break;
            case OutlinerCreateKind::PointLight:
                entity.name = "Point Light";
                entity.light = LightComponentDocument{};
                entity.light->type = SceneDocumentLightType::Point;
                entity.light->range = 10.0f;
                break;
            case OutlinerCreateKind::SpotLight:
                entity.name = "Spot Light";
                entity.light = LightComponentDocument{};
                entity.light->type = SceneDocumentLightType::Spot;
                entity.light->range = 10.0f;
                break;
            case OutlinerCreateKind::Camera:
                entity.name = "Camera";
                entity.camera = CameraComponentDocument{};
                break;
            case OutlinerCreateKind::SkyAtmosphere:
                entity.name = "Sky Atmosphere";
                entity.atmosphere = AtmosphereComponentDocument{};
                parentId.reset();
                break;
            case OutlinerCreateKind::ReflectionProbe:
                entity.name = "Reflection Probe";
                entity.reflectionProbe =
                    ReflectionProbeComponentDocument{};
                break;
            case OutlinerCreateKind::DdgiProbeVolume:
                entity.name = "DDGI Probe Volume";
                entity.ddgiProbeVolume =
                    DdgiProbeVolumeComponentDocument{};
                parentId.reset();
                break;
            }
            if (primitive) {
                entity.name = std::string(primitive->displayName);
                entity.modelInstance = ModelInstanceDocument{
                    ModelAssetId(std::string(primitive->id))};
            }
            const PersistentEntityId createdId = entity.id;
            sceneEditorSession_->execute(
                "Create Entity",
                [entity = std::move(entity), parentId](RuntimeWorld &world) {
                    const EntityHandle created = world.createEntity(entity);
                    if (parentId) {
                        const EntityHandle parent = world.find(*parentId);
                        return parent && world.setParent(created, parent);
                    }
                    return true;
                });
            sceneEditorSession_->select(createdId);
        } catch (const std::exception &error) {
            editorUi_->sceneError = error.what();
        }
    };
    actions.rename = [this](PersistentEntityId id, std::string name) {
        try {
            sceneEditorSession_->execute(
                "Rename Entity", [id, name = std::move(name)](
                                     RuntimeWorld &world) mutable {
                    return world.setName(world.find(id), std::move(name));
                });
        } catch (const std::exception &error) {
            editorUi_->sceneError = error.what();
        }
    };
    actions.setEnabled = [this](PersistentEntityId id, bool enabled) {
        try {
            sceneEditorSession_->execute(
                "Set Entity Enabled", [id, enabled](RuntimeWorld &world) {
                    return world.setEnabled(world.find(id), enabled);
                });
        } catch (const std::exception &error) {
            editorUi_->sceneError = error.what();
        }
    };
    actions.remove = [this](PersistentEntityId id) {
        sceneEditorSession_->select(id);
        deleteSelectedEditorEntity();
    };
    actions.duplicate = [this](PersistentEntityId rootId) {
        sceneEditorSession_->select(rootId);
        duplicateSelectedEditorEntity();
    };
    actions.reparent =
        [this](PersistentEntityId childId,
               std::optional<PersistentEntityId> parentId) {
            try {
                std::string failure;
                const bool changed = sceneEditorSession_->execute(
                    "Reparent Entity",
                    [childId, parentId, &failure](RuntimeWorld &world) {
                        const EntityHandle child = world.find(childId);
                        const std::optional<EntityHandle> parent =
                            parentId
                                ? std::optional<EntityHandle>(
                                      world.find(*parentId))
                                : std::nullopt;
                        if (!child || (parentId && (!parent || !*parent))) {
                            failure = "invalid_parent";
                            return false;
                        }
                        return world.setParent(child, parent,
                                               ReparentMode::KeepWorld,
                                               &failure);
                    });
                if (!changed) {
                    editorUi_->sceneError =
                        failure.empty() ? "reparent_failed" : failure;
                } else {
                    editorUi_->sceneError.clear();
                }
            } catch (const std::exception &error) {
                editorUi_->sceneError = error.what();
            }
        };
    outlinerPanel_->draw(snapshot, actions);
}

void Application::drawInspectorPanel() {
    if (!inspectorPanel_ || !sceneEditorSession_ ||
        !sceneEditorSession_->active()) {
        ImGui::TextDisabled("Open a native scene to inspect it.");
        return;
    }
    const std::shared_ptr<RuntimeWorld> world = sceneEditorSession_->world();
    InspectorPanelSnapshot snapshot;
    snapshot.sceneId = world->id();
    snapshot.sceneDisplayName = world->displayName();
    snapshot.ambient = world->ambient();
    snapshot.environment = world->environment();
    snapshot.activeCamera = world->activeCameraId();
    snapshot.editable = projectContext_.catalogWritable;
    snapshot.reflectionProbeCaptureAvailable =
        projectContext_.catalogWritable && captureService_ &&
        assetImportManager_;
    snapshot.reflectionProbeCaptureActive =
        reflectionProbeCapture_.has_value();
    if (reflectionProbeCapture_)
        snapshot.reflectionProbeCaptureStatus =
            reflectionProbeCapture_->status;
    const std::vector<RuntimeEntitySnapshot> entities = world->entities();
    snapshot.entities = entities;
    snapshot.atmospherePresent = std::any_of(
        entities.begin(), entities.end(),
        [](const RuntimeEntitySnapshot &entity) {
            return entity.atmosphere.has_value();
        });
    if (sceneEditorSession_->selection()) {
        snapshot.entity =
            world->entity(world->find(*sceneEditorSession_->selection()));
    }
    for (const RuntimeEntitySnapshot &entity : entities) {
        if (entity.camera)
            snapshot.cameraEntities.push_back(entity);
    }
    if (snapshot.entity && snapshot.entity->light) {
        const RuntimeEntitySnapshot &entity = *snapshot.entity;
        const LightComponentDocument &light = *entity.light;
        const bool contributes =
            entity.effectiveEnabled && std::isfinite(light.intensity) &&
            light.intensity > 1.0e-5f && std::isfinite(light.color.r) &&
            std::isfinite(light.color.g) && std::isfinite(light.color.b) &&
            (light.color.r > 1.0e-5f || light.color.g > 1.0e-5f ||
             light.color.b > 1.0e-5f);
        const bool ignored =
            std::find(lastLightStats_.ignoredEntityIds.begin(),
                      lastLightStats_.ignoredEntityIds.end(), entity.id) !=
            lastLightStats_.ignoredEntityIds.end();
        snapshot.selectedLightUploadStatus =
            ignored ? InspectorLightUploadStatus::NotUploaded
                    : contributes ? InspectorLightUploadStatus::Active
                                  : InspectorLightUploadStatus::Ineffective;

        if (!light.castsShadow) {
            snapshot.selectedLightShadowStatus =
                InspectorLightShadowStatus::Disabled;
        } else if (light.type == SceneDocumentLightType::Directional) {
            const bool selectedCaster =
                lastLightStats_.shadowCasterEntity &&
                *lastLightStats_.shadowCasterEntity == entity.id;
            snapshot.selectedLightShadowStatus =
                selectedCaster && lastLightStats_.shadowCasterActive
                    ? InspectorLightShadowStatus::Active
                    : InspectorLightShadowStatus::Eligible;
        } else {
            const auto &selections =
                light.type == SceneDocumentLightType::Point
                    ? lastLightStats_.pointShadowSelections
                    : lastLightStats_.spotShadowSelections;
            const bool selected = std::any_of(
                selections.begin(), selections.end(),
                [&entity](const PunctualShadowSelection &selection) {
                    return selection.entity &&
                           *selection.entity == entity.id;
                });
            snapshot.selectedLightShadowStatus =
                selected ? InspectorLightShadowStatus::Active
                         : InspectorLightShadowStatus::BudgetExceeded;
        }
    }
    for (const CatalogModel &model : catalog_.models) {
        snapshot.models.push_back({model.id, model.displayName, true});
    }
    for (const PrimitiveModelDefinition &primitive :
         primitiveModelDefinitions()) {
        snapshot.models.push_back({std::string(primitive.id),
                                   std::string(primitive.displayName), true});
    }
    for (const CatalogEnvironment &environment : catalog_.environments) {
        snapshot.environments.push_back(
            {environment.id, environment.displayName});
    }

    InspectorPanelActions actions;
    actions.setName = [this](PersistentEntityId id, std::string value) {
        sceneEditorSession_->execute(
            "Rename Entity", [id, value = std::move(value)](
                                 RuntimeWorld &world) mutable {
                return world.setName(world.find(id), std::move(value));
            });
    };
    actions.setEnabled = [this](PersistentEntityId id, bool value) {
        sceneEditorSession_->execute(
            "Set Entity Enabled", [id, value](RuntimeWorld &world) {
                return world.setEnabled(world.find(id), value);
            });
    };
    actions.setParent =
        [this](PersistentEntityId id,
               std::optional<PersistentEntityId> parentId) {
            sceneEditorSession_->execute(
                "Change Parent", [id, parentId](RuntimeWorld &world) {
                    const EntityHandle handle = world.find(id);
                    const std::optional<EntityHandle> parent =
                        parentId
                            ? std::optional<EntityHandle>(
                                  world.find(*parentId))
                            : std::nullopt;
                    return handle && (!parentId || (parent && *parent)) &&
                           world.setParent(handle, parent);
                });
        };
    actions.setTransform = [this](PersistentEntityId id,
                                  SceneTransformDocument value) {
        if (auto world = sceneEditorSession_->world())
            world->setTransform(world->find(id), value);
    };
    actions.setModel = [this](PersistentEntityId id,
                              std::optional<ModelInstanceDocument> value) {
        sceneEditorSession_->execute(
            "Set Model Component", [id, value = std::move(value)](
                                       RuntimeWorld &world) mutable {
                return world.setModelInstance(world.find(id),
                                              std::move(value));
            });
    };
    actions.setLight = [this](PersistentEntityId id,
                              std::optional<LightComponentDocument> value) {
        if (sceneEditorSession_->continuousEditActive()) {
            if (auto world = sceneEditorSession_->world())
                world->setLight(world->find(id), std::move(value));
            return;
        }
        sceneEditorSession_->execute(
            "Set Light Component", [id, value = std::move(value)](
                                       RuntimeWorld &world) mutable {
                return world.setLight(world.find(id), std::move(value));
            });
    };
    actions.setCamera = [this](PersistentEntityId id,
                               std::optional<CameraComponentDocument> value) {
        if (sceneEditorSession_->continuousEditActive()) {
            if (auto world = sceneEditorSession_->world())
                world->setCamera(world->find(id), std::move(value));
            return;
        }
        sceneEditorSession_->execute(
            "Set Camera Component", [id, value = std::move(value)](
                                        RuntimeWorld &world) mutable {
                return world.setCamera(world.find(id), std::move(value));
            });
    };
    actions.setAtmosphere =
        [this](PersistentEntityId id,
               std::optional<AtmosphereComponentDocument> value) {
            if (sceneEditorSession_->continuousEditActive()) {
                if (auto world = sceneEditorSession_->world())
                    world->setAtmosphere(world->find(id), std::move(value));
                return;
            }
            sceneEditorSession_->execute(
                "Set Sky Atmosphere",
                [id, value = std::move(value)](RuntimeWorld &world) mutable {
                    return world.setAtmosphere(world.find(id),
                                               std::move(value));
                });
        };
    actions.setAtmosphereSun =
        [this](PersistentEntityId id, bool enabled) {
            sceneEditorSession_->execute(
                "Set Atmosphere Sun", [id, enabled](RuntimeWorld &world) {
                    for (const RuntimeEntitySnapshot &entity :
                         world.entities()) {
                        if (!entity.light)
                            continue;
                        LightComponentDocument light = *entity.light;
                        const bool selected = entity.id == id;
                        const bool shouldUse = enabled && selected;
                        if ((light.atmosphereSunIndex == 0u) == shouldUse)
                            continue;
                        light.atmosphereSunIndex =
                            shouldUse ? std::optional<uint32_t>(0u)
                                      : std::nullopt;
                        if (!world.setLight(world.find(entity.id), light))
                            return false;
                    }
                    return true;
                });
        };
    actions.setReflectionProbe =
        [this](PersistentEntityId id,
               std::optional<ReflectionProbeComponentDocument> value) {
            if (sceneEditorSession_->continuousEditActive()) {
                if (auto world = sceneEditorSession_->world())
                    world->setReflectionProbe(world->find(id),
                                              std::move(value));
                return;
            }
            sceneEditorSession_->execute(
                "Set Reflection Probe",
                [id, value = std::move(value)](
                    RuntimeWorld &world) mutable {
                    return world.setReflectionProbe(world.find(id),
                                                    std::move(value));
                });
        };
    actions.setDdgiProbeVolume =
        [this](PersistentEntityId id,
               std::optional<DdgiProbeVolumeComponentDocument> value) {
            if (sceneEditorSession_->continuousEditActive()) {
                if (auto world = sceneEditorSession_->world())
                    world->setDdgiProbeVolume(world->find(id),
                                              std::move(value));
                return;
            }
            sceneEditorSession_->execute(
                "Set DDGI Probe Volume",
                [id, value = std::move(value)](
                    RuntimeWorld &world) mutable {
                    return world.setDdgiProbeVolume(world.find(id),
                                                    std::move(value));
                });
        };
    actions.captureReflectionProbe =
        [this](PersistentEntityId id) {
            try {
                beginReflectionProbeCapture(id);
                editorUi_->sceneError.clear();
            } catch (const std::exception &error) {
                editorUi_->sceneError = error.what();
            }
        };
    actions.setActiveCamera = [this](PersistentEntityId id) {
        sceneEditorSession_->execute(
            "Set Active Camera", [id](RuntimeWorld &world) {
                return world.setActiveCamera(id);
            });
    };
    actions.setAmbient = [this](SceneAmbientDocument value) {
        if (sceneEditorSession_->continuousEditActive()) {
            if (auto world = sceneEditorSession_->world())
                world->setAmbient(value);
        } else {
            sceneEditorSession_->execute(
                "Edit Ambient", [value](RuntimeWorld &world) {
                    world.setAmbient(value);
                    return true;
                });
        }
    };
    actions.setEnvironment =
        [this](std::optional<SceneEnvironmentDocument> value) {
            if (sceneEditorSession_->continuousEditActive()) {
                if (auto world = sceneEditorSession_->world())
                    world->setEnvironment(std::move(value));
            } else {
                sceneEditorSession_->execute(
                    "Set Environment", [value = std::move(value)](
                                           RuntimeWorld &world) mutable {
                        world.setEnvironment(std::move(value));
                        return true;
                    });
            }
        };
    actions.beginContinuous = [this](std::string label) {
        sceneEditorSession_->beginContinuousEdit(std::move(label));
    };
    actions.commitContinuous = [this]() {
        try {
            sceneEditorSession_->commitContinuousEdit();
        } catch (const std::exception &error) {
            editorUi_->sceneError = error.what();
        }
    };
    try {
        inspectorPanel_->draw(snapshot, actions);
    } catch (const std::exception &error) {
        editorUi_->sceneError = error.what();
    }
}

void Application::handleEditorShortcuts() {
    if (!editorUi_ || ImGui::GetIO().WantTextInput ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) ||
        ImGui::IsAnyItemActive() ||
        (sceneViewportController_ &&
         sceneViewportController_->blocksViewportInput()) ||
        ImGui::GetDragDropPayload() != nullptr) {
        return;
    }
    const ImGuiIO &io = ImGui::GetIO();
    const auto pressed = [](ImGuiKey key) {
        return ImGui::IsKeyPressed(key, false);
    };
    if (io.KeyCtrl && io.KeyShift && pressed(ImGuiKey_S)) {
        if (sceneEditorSession_ && sceneEditorSession_->active())
            editorUi_->requestSaveAsModal = true;
        return;
    }
    if (io.KeyCtrl && pressed(ImGuiKey_N)) {
        if (hasUnsavedSceneChanges()) {
            editorUi_->pendingAction = EditorPendingActionKind::NewScene;
            editorUi_->requestDirtyModal = true;
        } else {
            editorUi_->requestNewSceneModal = true;
        }
        return;
    }
    if (io.KeyCtrl && pressed(ImGuiKey_O)) {
        editorUi_->requestOpenSceneModal = true;
        return;
    }
    if (io.KeyCtrl && pressed(ImGuiKey_S)) {
        saveEditorScene();
        return;
    }
    if (io.KeyCtrl && pressed(ImGuiKey_Z)) {
        if (sceneEditorSession_)
            sceneEditorSession_->undo();
        return;
    }
    if (io.KeyCtrl && pressed(ImGuiKey_Y)) {
        if (sceneEditorSession_)
            sceneEditorSession_->redo();
        return;
    }
    if (io.KeyCtrl && pressed(ImGuiKey_D)) {
        duplicateSelectedEditorEntity();
        return;
    }
    if (pressed(ImGuiKey_Delete)) {
        deleteSelectedEditorEntity();
        return;
    }
    if (pressed(ImGuiKey_F2) && outlinerPanel_ && sceneEditorSession_ &&
        sceneEditorSession_->selection()) {
        const auto entity = sceneEditorSession_->world()->entity(
            sceneEditorSession_->world()->find(
                *sceneEditorSession_->selection()));
        if (entity)
            outlinerPanel_->beginRename(entity->id, entity->name);
    }
}

void Application::drawGui() {
    VKL_PROFILE_ZONE("Build Editor UI");
    updateModelImport();
    if (!editorDockWorkspace_)
        return;

    std::string sceneName = "No Scene";
    if (sceneRuntime_->currentSceneIndex() >= 0 &&
        sceneRuntime_->currentSceneIndex() < static_cast<int>(sceneRegistry_.size()))
        sceneName = sceneRegistry_[sceneRuntime_->currentSceneIndex()].name;
    EditorFrameStatus status{};
    status.sceneName = sceneName;
    status.fps = ImGui::GetIO().Framerate;
    const GpuPassTimings &gpuTimings = renderer_->gpuPassTimings();
    if (gpuTimings.available)
        status.gpuFrameMs = static_cast<float>(gpuTimings.totalMs);

    EditorUiState &uiState = *editorUi_;
    uiState.fpsHistory[uiState.performanceCursor] = status.fps;
    uiState.gpuHistory[uiState.performanceCursor] =
        std::max(status.gpuFrameMs, 0.0f);
    uiState.performanceCursor =
        (uiState.performanceCursor + 1) %
        EditorUiState::kPerformanceHistorySize;
    uiState.performanceCount = std::min(
        uiState.performanceCount + 1,
        EditorUiState::kPerformanceHistorySize);

    bool hasActiveLoad = false;
    if (sceneRuntime_->latestSceneLoadTask()) {
        const SceneLoadState state = sceneRuntime_->latestSceneLoadTask()->state.load();
        hasActiveLoad = !isTerminalSceneLoadState(state);
        if (hasActiveLoad) {
            const auto &progress = sceneRuntime_->latestSceneLoadTask()->progress;
            const uint64_t completed =
                progress.completedTextures.load() +
                progress.completedMeshes.load() +
                progress.uploadedTextures.load() +
                progress.uploadedMeshes.load();
            const uint64_t total =
                progress.totalTextures.load() +
                progress.totalMeshes.load() +
                progress.uploadTextureTotal.load() +
                progress.uploadMeshTotal.load();
            const float fraction =
                total == 0
                    ? 0.0f
                    : std::clamp(static_cast<float>(completed) /
                                     static_cast<float>(total),
                                 0.0f, 1.0f);
            status.loading = true;
            status.loadingLabel = sceneLoadStateName(state);
            status.loadingProgress = fraction;
        }
    }

    EditorPanelCallbacks panels{};
    panels.outliner = [this]() { drawOutlinerPanel(); };
    panels.inspector = [this]() { drawInspectorPanel(); };
    panels.sceneSessionActive =
        sceneEditorSession_ && sceneEditorSession_->active();
    panels.sceneDirty = hasUnsavedSceneChanges();
    panels.canUndo = panels.sceneSessionActive &&
                     sceneEditorSession_->canUndo();
    panels.canRedo = panels.sceneSessionActive &&
                     sceneEditorSession_->canRedo();
    if (panels.canUndo)
        panels.undoLabel = sceneEditorSession_->undoLabel();
    if (panels.canRedo)
        panels.redoLabel = sceneEditorSession_->redoLabel();
    panels.newScene = [this]() {
        if (hasUnsavedSceneChanges()) {
            editorUi_->pendingAction = EditorPendingActionKind::NewScene;
            editorUi_->requestDirtyModal = true;
        } else {
            editorUi_->requestNewSceneModal = true;
        }
    };
    panels.openScene = [this]() {
        editorUi_->requestOpenSceneModal = true;
    };
    panels.saveScene = [this]() { saveEditorScene(); };
    panels.saveSceneAs = [this]() {
        editorUi_->requestSaveAsModal = true;
    };
    panels.closeScene = [this]() {
        editorUi_->pendingAction = EditorPendingActionKind::CloseScene;
        if (hasUnsavedSceneChanges())
            editorUi_->requestDirtyModal = true;
        else
            executePendingEditorAction(false);
    };
    panels.convertPreview = [this]() {
        editorUi_->requestConvertPreviewModal = true;
    };
    panels.undo = [this]() {
        if (sceneEditorSession_)
            sceneEditorSession_->undo();
    };
    panels.redo = [this]() {
        if (sceneEditorSession_)
            sceneEditorSession_->redo();
    };
    if (panels.sceneSessionActive) {
        panels.viewportToolbar = [this]() {
            if (sceneViewportController_) {
                sceneViewportController_->drawToolbar();
                const float contentRight =
                    ImGui::GetWindowPos().x +
                    ImGui::GetWindowContentRegionMax().x;
                const float remaining =
                    contentRight - ImGui::GetItemRectMax().x;
                if (remaining >=
                    132.0f + ImGui::GetStyle().ItemSpacing.x) {
                    ImGui::SameLine();
                }
            }
            const bool editorCamera = sceneEditorSession_->cameraMode() ==
                                      EditorCameraMode::Editor;
            ImGui::SetNextItemWidth(132.0f);
            if (ImGui::BeginCombo("##ViewportCamera",
                                  editorCamera ? "Editor Camera"
                                               : "Active Camera")) {
                if (ImGui::Selectable("Editor Camera", editorCamera))
                    sceneEditorSession_->setCameraMode(
                        EditorCameraMode::Editor);
                if (ImGui::Selectable("Active Camera", !editorCamera))
                    sceneEditorSession_->setCameraMode(
                        EditorCameraMode::ActiveScene);
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Viewport camera source");
        };
        panels.viewportOverlay = [this](
                                     const EditorViewportState &viewport) {
            if (!sceneViewportController_ || !sceneEditorSession_ ||
                !sceneEditorSession_->active() || !viewport.valid)
                return;

            const float aspect =
                viewport.logicalHeight > 0.0f
                    ? viewport.logicalWidth / viewport.logicalHeight
                    : 1.0f;
            camera_.setAspect(aspect);
            SceneViewportCamera viewportCamera;
            viewportCamera.view = camera_.viewMatrix();
            viewportCamera.projection = camera_.projectionMatrix();
            viewportCamera.position = camera_.position();
            viewportCamera.forward = camera_.front();
            viewportCamera.cameraDragging =
                mode_ == InputMode::CameraDrag;
            if (sceneEditorSession_->cameraMode() ==
                EditorCameraMode::ActiveScene) {
                if (const auto active =
                        sceneEditorSession_->world()->activeCamera(aspect)) {
                    viewportCamera.view = active->view;
                    viewportCamera.projection = active->projection;
                    viewportCamera.position = active->position;
                    const glm::mat4 inverseView =
                        glm::inverse(active->view);
                    viewportCamera.forward = glm::normalize(
                        -glm::vec3(inverseView[2]));
                }
            }

            SceneViewportActions actions;
            actions.modelDisplayName = [this](const std::string &modelId) {
                const auto source = resolveModelSource(
                    catalog_, projectContext_, ModelAssetId(modelId));
                return source ? source->displayName : modelId;
            };
            actions.instantiateModel =
                [this](const std::string &modelId,
                       const glm::vec3 &position) {
                    try {
                        const auto source = resolveModelSource(
                            catalog_, projectContext_, ModelAssetId(modelId));
                        if (!source || !source->instanceable)
                            throw std::runtime_error(
                                "model_not_instanceable");
                        SceneEntityDocument entity;
                        entity.id = PersistentEntityId::generate();
                        entity.name = source->displayName;
                        entity.transform.translation = position;
                        entity.modelInstance = ModelInstanceDocument{
                            ModelAssetId(modelId)};
                        const PersistentEntityId created = entity.id;
                        if (sceneEditorSession_->execute(
                                "Add Model Entity",
                                [entity = std::move(entity)](
                                    RuntimeWorld &world) mutable {
                                    world.createEntity(std::move(entity));
                                    return true;
                                })) {
                            sceneEditorSession_->select(created);
                            editorUi_->sceneError.clear();
                        }
                    } catch (const std::exception &error) {
                        editorUi_->sceneError = error.what();
                    }
                };
            actions.reportError = [this](std::string error) {
                editorUi_->sceneError = std::move(error);
            };
            try {
                sceneViewportController_->drawOverlay(
                    viewport, viewportCamera, *sceneEditorSession_, actions);
            } catch (const std::exception &error) {
                editorUi_->sceneError = error.what();
                sceneViewportController_->cancelManipulation();
            }
        };
    }
    panels.scenes = [this, hasActiveLoad]() {
        drawScenePanel(false);
        if (hasActiveLoad) {
            ImGui::SeparatorText("Loading");
            drawSceneLoadingPanel();
        }
        if (!editorUi_->sceneStatus.empty()) {
            ImGui::Separator();
            editor::statusIndicator(editorUi_->sceneStatus.c_str(),
                                    editor::StatusTone::Success);
        }
        if (!editorUi_->sceneError.empty()) {
            ImGui::Separator();
            ImGui::TextColored(editor::statusColor(
                                   editor::StatusTone::Error),
                               "%s", editorUi_->sceneError.c_str());
        }
    };
    panels.assets = [this]() {
        if (!ImGui::BeginTabBar("AssetTabs"))
            return;
        if (ImGui::BeginTabItem("Models")) {
            ImGui::PushID("Models");
            drawScenePanel(true);
            ImGui::PopID();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Environments")) {
            ImGui::PushID("Environments");
            drawAssetsPanel(true);
            ImGui::PopID();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Jobs / Cache")) {
            ImGui::PushID("JobsCache");
            drawAssetsPanel(false);
            ImGui::PopID();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    };
    panels.render = [this]() {
        ImGui::PushItemWidth(-145.0f);
        if (ImGui::CollapsingHeader("Common",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushID("Common");
            drawRenderPanel();
            ImGui::PopID();
        }
        if (ImGui::CollapsingHeader("Post Processing",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushID("PostProcessing");
            drawPostProcessingPanel();
            ImGui::PopID();
        }
        if (ImGui::CollapsingHeader("Surface Data")) {
            ImGui::PushID("SurfaceData");
            drawSurfaceDataPanel();
            ImGui::PopID();
        }
        if (ImGui::CollapsingHeader("Lighting",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushID("Lighting");
            drawLightingPanel();
            ImGui::PopID();
        }
        if (ImGui::CollapsingHeader("Culling",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushID("Culling");
            drawCullingPanel();
            ImGui::PopID();
        }
        if (ImGui::CollapsingHeader("Camera & Clip")) {
            ImGui::PushID("Camera");
            drawCameraPanel();
            ImGui::PopID();
        }
        ImGui::PopItemWidth();
    };
    panels.materials = [this]() { drawMaterialsPanel(); };
    panels.diagnostics = [this]() {
        if (ImGui::BeginTabBar("DiagnosticsTabs")) {
            if (ImGui::BeginTabItem("Performance")) {
                ImGui::PushID("PerformanceTab");
                drawPerformancePanel();
                ImGui::PopID();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Load Stats")) {
                ImGui::PushID("LoadStatsTab");
                drawLoadStatsPanel();
                ImGui::PopID();
                ImGui::EndTabItem();
            }
#if VKL_ENABLE_CAPTURE
            if (ImGui::BeginTabItem("Capture")) {
                ImGui::PushID("CaptureTab");
                drawCapturePanel();
                ImGui::PopID();
                ImGui::EndTabItem();
            }
#endif
            ImGui::EndTabBar();
        }
    };

    handleEditorShortcuts();

    const VkExtent2D renderExtent = renderer_->viewportExtent();
    EditorViewportFrame viewportFrame{};
    viewportFrame.textureId =
        gui_->viewportTextureId(frameSync_->nextFrameIndex());
    viewportFrame.renderWidth = renderExtent.width;
    viewportFrame.renderHeight = renderExtent.height;
    viewportFrame.resizePending = viewportResize_.pending;
    editorDockWorkspace_->draw(status, viewportFrame, panels);
    drawSceneAuthoringDialogs();

    const EditorViewportState &viewport =
        editorDockWorkspace_->viewportState();
    viewportVisible_ = viewport.visible;
    viewportHovered_ = viewport.hovered;
    if ((!panels.sceneSessionActive || !viewport.visible ||
         !viewport.valid) &&
        sceneViewportController_)
        sceneViewportController_->cancelManipulation();
    if (viewport.valid) {
        viewportDisplayWidth_ = viewport.pixelWidth;
        viewportDisplayHeight_ = viewport.pixelHeight;
        camera_.setAspect(viewport.logicalWidth / viewport.logicalHeight);

        if (!reflectionProbeCapture_) {
            const bool desiredChanged =
                viewportResize_.desiredWidth != viewport.pixelWidth ||
                viewportResize_.desiredHeight != viewport.pixelHeight;
            if (desiredChanged) {
                viewportResize_.desiredWidth = viewport.pixelWidth;
                viewportResize_.desiredHeight = viewport.pixelHeight;
                viewportResize_.changedAt =
                    std::chrono::steady_clock::now();
                viewportResize_.pending = true;
                viewportResize_.immediate = !viewportResize_.measured;
                viewportResize_.measured = true;
            }
        }
    }
}
#else
void Application::drawGui() {}
#endif

#if VKL_ENABLE_EDITOR_UI
void Application::bindViewportTextures() {
    if (!gui_ || !renderer_)
        return;
    const RendererViewportOutput output = renderer_->viewportOutput();
    gui_->setViewportTextures(output.sampler, output.imageViews);
}

void Application::applyPendingViewportResize() {
    if (!viewportResize_.pending || !renderer_ || !gui_)
        return;
    const VkExtent2D current = renderer_->viewportExtent();
    if (current.width == viewportResize_.desiredWidth &&
        current.height == viewportResize_.desiredHeight) {
        viewportResize_.pending = false;
        viewportResize_.immediate = false;
        return;
    }

    constexpr auto kResizeDebounce = std::chrono::milliseconds(120);
    if (!viewportResize_.immediate &&
        std::chrono::steady_clock::now() - viewportResize_.changedAt <
            kResizeDebounce) {
        return;
    }

    frameSync_->waitForAllFrames();
    if (captureService_) {
        captureService_->update(frameSync_->completedSubmissionSerial());
    }
    gui_->clearViewportTextures();
    renderer_->resizeViewport(
        {viewportResize_.desiredWidth, viewportResize_.desiredHeight});
    bindViewportTextures();
    viewportResize_.pending = false;
    viewportResize_.immediate = false;
    VKR_LOG_DEBUG("Viewport", "Resized render target to {}x{}",
                  renderer_->viewportExtent().width,
                  renderer_->viewportExtent().height);
}
#endif

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
    const ShaderVariant *variant =
        shaderRegistry_.findVariant(currentShaderVariantId_);
    return variant ? *variant : shaderRegistry_.defaultVariant();
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
        if (window_->shouldClose() && hasUnsavedSceneChanges() &&
            editorUi_ && !editorUi_->quitConfirmed) {
            window_->setShouldClose(false);
            editorUi_->pendingAction = EditorPendingActionKind::Quit;
            editorUi_->requestDirtyModal = true;
        }
#endif
        if (window_->shouldClose())
            break;

        if (captureService_) {
            captureService_->update(
                frameSync_->completedSubmissionSerial());
        }

        updateAssetImports();
#if VKL_ENABLE_RUNTIME_CONTROL
        processRuntimeCommand();
        if (pendingQuitCommand_ &&
            pendingQuitCommand_->responseDelivered.load()) {
            window_->setShouldClose(true);
            pendingQuitCommand_.reset();
            break;
        }
#endif

        // 1. 帧外：场景切�?
        sceneRuntime_->pump();
#if VKL_ENABLE_EDITOR_UI
        updateReflectionProbeCapture();
#endif

        // 2. 时间
        const auto now = std::chrono::high_resolution_clock::now();
        const float dt = config_.diagnostics.fixedDeltaSeconds
                             ? *config_.diagnostics.fixedDeltaSeconds
                             : std::chrono::duration<float>(now - lastTime)
                                   .count();
        lastTime = now;

#if VKL_ENABLE_EDITOR_UI
        applyPendingViewportResize();
#endif

        // 3. ImGui 新帧
        if (gui_)
            gui_->beginFrame();
#if VKL_ENABLE_EDITOR_UI
        if (gui_ && sceneViewportController_)
            sceneViewportController_->beginFrame();
#endif

        // 4. 模式切换 + 输入
        if (!config_.diagnostics.automationMode) {
            updateInputMode();
            if (mode_ == InputMode::CameraDrag)
                processCameraInput(dt);
        }
        if (input_->isKeyPressed(Key::Escape)) {
#if VKL_ENABLE_EDITOR_UI
            if (sceneViewportController_ &&
                sceneViewportController_->manipulationActive()) {
                sceneViewportController_->cancelManipulation();
            } else if (hasUnsavedSceneChanges()) {
                editorUi_->pendingAction = EditorPendingActionKind::Quit;
                editorUi_->requestDirtyModal = true;
            } else {
                window_->setShouldClose(true);
            }
#else
            window_->setShouldClose(true);
#endif
        }
#if VKL_ENABLE_CAPTURE
        if (input_->isKeyPressed(Key::F12))
            requestManualCapture(captureIncludeGui_);
#endif

        // 5. 场景 tick
        simulationTime = config_.diagnostics.fixedDeltaSeconds
                             ? simulationTime + dt
                             : std::chrono::duration<float>(now - startTime)
                                   .count();
        if (sceneRuntime_->currentWorld())
            sceneRuntime_->currentWorld()->update(dt, simulationTime);

        // 6. UI
        if (gui_)
            drawGui();

        // 7. 渲染
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
        viewInput.settings = renderSettings_;
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
            useActiveSceneCamera =
                useActiveSceneCamera ||
                (sceneEditorSession_ && sceneEditorSession_->active() &&
                 sceneEditorSession_->cameraMode() ==
                     EditorCameraMode::ActiveScene);
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
        applyReflectionProbeCaptureView(viewInput, cameraHistoryIdentity);
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
        if (sceneEditorSession_ && sceneEditorSession_->active() &&
            sceneEditorSession_->selection()) {
            const auto world = sceneEditorSession_->world();
            const auto selected = world ? world->entity(world->find(
                                         *sceneEditorSession_->selection()))
                                        : std::nullopt;
            if (selected && selected->light)
                shadowInput.focusedLightEntity = selected->id;
        }
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

        // 8. 帧末：丢弃本帧鼠标增量
        input_->endFrame();
    }

    vkDeviceWaitIdle(device_->logicalDevice());
    frameSync_->markAllSubmissionsCompleted();
    sceneRuntime_->collectRetired();
    if (captureService_)
        captureService_->shutdown(frameSync_->completedSubmissionSerial());
}

} // namespace vkr
