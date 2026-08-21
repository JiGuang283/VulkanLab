#include "EditorController.h"

#include <BuildFeatures.h>

#include "render/frame/RenderSettingsController.h"
#include "scene/SceneRuntimeCoordinator.h"
#include "workflows/SceneWorkflowController.h"
#include "assets/ArtifactStatus.h"
#include "assets/EnvironmentLoadManager.h"
#include "assets/ModelImportService.h"
#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"
#include "assets/SceneCatalogEditor.h"
#include "assets/SceneCatalogStore.h"
#include "core/Device.h"
#include "core/FrameSync.h"
#include "core/Log.h"
#include "core/SwapChain.h"
#include "diagnostics/CaptureService.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/SceneLoadStats.h"
#include "diagnostics/TracyProfiler.h"
#include "editor/EditorDockWorkspace.h"
#include "editor/EditorActionRegistry.h"
#include "editor/EditorIcons.h"
#include "editor/EditorNotificationService.h"
#include "editor/EditorUiState.h"
#include "editor/EditorWidgets.h"
#include "editor/ReflectionProbeCapture.h"
#include "editor/SceneEditorSession.h"
#include "editor/SceneViewportController.h"
#include "editor/panels/AssetsPanel.h"
#include "editor/panels/ContentBrowserPanel.h"
#include "editor/panels/DiagnosticsPanel.h"
#include "editor/panels/InspectorPanel.h"
#include "editor/panels/MaterialsPanel.h"
#include "editor/panels/OutlinerPanel.h"
#include "editor/panels/RenderSettingsPanel.h"
#include "editor/panels/ScenesPanel.h"
#include "render/features/shadows_visibility/DirectionalShadow.h"
#include "editor/GuiSystem.h"
#include "render/material/MaterialInstance.h"
#include "render/material/MaterialTemplate.h"
#include "render/material/MaterialSystem.h"
#include "render/material/MaterialTextureSlot.h"
#include "render/features/shadows_visibility/PunctualShadow.h"
#include "render/features/global_illumination/RayTracingScene.h"
#include "render/Renderer.h"
#include "render/features/global_illumination/DdgiPass.h"
#include "scene/AssetRepository.h"
#include "scene/Camera.h"
#include "scene/EnvironmentAssetRepository.h"
#include "scene/ModelSourceResolver.h"
#include "scene/RuntimeWorld.h"
#include "scene/SceneLoadTask.h"
#include "scene_data/PrimitiveModelDefinitions.h"
#include "scene_data/SceneDocument.h"
#include "window/Window.h"
#if VKL_ENABLE_ASSET_AUTHORING
#include "platform/FileDialogWin32.h"
#endif

#include <imgui.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace vkr {
namespace {
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

double bytesToMiB(uint64_t bytes) {
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
} // namespace

EditorController::EditorController(EditorControllerServices services)
    : config_(services.config), projectContext_(services.projectContext),
      catalog_(services.catalog), sceneRegistry_(services.sceneRegistry),
      sceneLoadContext_(services.sceneLoadContext), window_(&services.window),
      device_(&services.device), frameSync_(&services.frameSync),
      swapChain_(&services.swapChain), renderer_(&services.renderer),
      gui_(&services.gui), materialSystem_(&services.materialSystem),
      sceneWorkflow_(&services.sceneWorkflow),
      sceneRuntime_(&services.sceneRuntime),
      renderSettingsController_(&services.renderSettings),
      captureService_(services.captureService), camera_(services.camera),
      ambientColor_(services.ambientColor),
      ambientIntensity_(services.ambientIntensity),
      defaultSunDirection_(services.defaultSunDirection),
      defaultSunColor_(services.defaultSunColor),
      defaultSunIntensity_(services.defaultSunIntensity),
      visibilityFrame_(services.visibilityFrame),
      shadowSystem_(services.shadowSystem),
      lastLightStats_(services.lastLightStats),
      cameraDragging_(std::move(services.cameraDragging)),
      actions_(std::move(services.actions)),
      preferences_(std::make_unique<EditorPreferencesStore>(
          std::move(services.editorStorage))),
      editorDockWorkspace_(
          std::make_unique<EditorDockWorkspace>(*preferences_)),
      actionRegistry_(std::make_unique<EditorActionRegistry>()),
      commandPalette_(std::make_unique<EditorCommandPalette>()),
      notifications_(std::make_unique<EditorNotificationService>()),
      assetsPanel_(std::make_unique<AssetsPanel>()),
      contentBrowserPanel_(std::make_unique<ContentBrowserPanel>()),
      diagnosticsPanel_(std::make_unique<DiagnosticsPanel>()),
      materialsPanel_(std::make_unique<MaterialsPanel>()),
      scenesPanel_(std::make_unique<ScenesPanel>()),
      outlinerPanel_(std::make_unique<OutlinerPanel>()),
      renderSettingsPanel_(std::make_unique<RenderSettingsPanel>()),
      inspectorPanel_(std::make_unique<InspectorPanel>()),
      sceneEditorSession_(std::make_unique<SceneEditorSession>()),
      sceneViewportController_(std::make_unique<SceneViewportController>()),
      editorUi_(std::make_unique<EditorUiState>()) {
    config_.moveSpeed = preferences_->preferences().cameraMoveSpeed;
    sceneViewportController_->setOperation(
        preferences_->preferences().gizmoOperation);
    sceneViewportController_->setSpace(preferences_->preferences().gizmoSpace);
    assetWorkflowSnapshot_ = sceneWorkflow_->assetSnapshot();
    bindViewportTextures();
}

EditorController::~EditorController() {
    if (preferences_)
        preferences_->flush();
    if (gui_)
        gui_->clearViewportTextures();
}

uint64_t EditorController::requestSceneOperation(
    int index, bool sourceFallback, bool loadAfter,
    SceneWorkflowRequestReason reason, bool forceReimport,
    bool reloadAsset) {
    return actions_.requestSceneOperation(
        index, sourceFallback, loadAfter, reason, forceReimport, reloadAsset);
}

uint64_t EditorController::setTextureLimit(uint32_t limit) {
    return actions_.setTextureLimit(limit);
}

uint64_t EditorController::setEnvironment(const std::string &id) {
    return actions_.setEnvironment(id);
}

void EditorController::setViewMode(const std::string &id) {
    renderSettingsController_->setViewMode(id);
}

void EditorController::applyRenderSettings(const RenderSettingsPatch &patch) {
    renderSettingsController_->apply(patch);
}

const RenderSettings &EditorController::renderSettings() const {
    return renderSettingsController_->settings();
}

const ViewMode &EditorController::currentViewMode() const {
    return renderSettingsController_->currentViewMode();
}

bool EditorController::hasUnsavedChanges() const {
    return sceneEditorSession_ && sceneEditorSession_->active() &&
           sceneEditorSession_->dirty();
}

void EditorController::refreshSceneRegistry(
    const std::string &selectSceneId) {
    sceneWorkflow_->refresh(selectSceneId);
}
void EditorController::requestEditorSceneLoad(int index) {
    if (hasUnsavedChanges()) {
        editorUi_->pendingAction = EditorPendingActionKind::LoadScene;
        editorUi_->pendingSceneIndex = index;
        editorUi_->requestDirtyModal = true;
        return;
    }
    requestSceneOperation(index);
}

void EditorController::updateEditorModelBindings() {
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
            RenderSettingsPatch patch;
            patch.environmentIntensity = environment->intensity;
            patch.environmentRotationRadians =
                environment->rotationRadians;
            applyRenderSettings(patch);
        }
    } catch (const std::exception &error) {
        editorUi_->sceneError = error.what();
    }
}

void EditorController::updateEditorReflectionProbeBindings() {
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
                sceneRuntime_->requestEnvironmentAsset(*environment);
            world->bindReflectionProbe(
                entity.handle, entity.reflectionProbeBindingRevision,
                environment->environmentProfile, std::move(handle));
        } catch (const std::exception &error) {
            world->bindReflectionProbe(
                entity.handle, entity.reflectionProbeBindingRevision, {},
                {}, error.what());
        }
    }
}

void EditorController::beginReflectionProbeCapture(
    PersistentEntityId entityId) {
    if (reflectionProbeCapture_)
        throw std::runtime_error(
            "Another reflection probe capture is already active");
    if (!sceneEditorSession_ || !sceneEditorSession_->active())
        throw std::runtime_error("No native scene is open");
    if (!projectContext_.catalogWritable)
        throw std::runtime_error("The project Catalog is read-only");
    if (!captureService_ || !sceneWorkflow_->assetAuthoringAvailable())
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
}

void EditorController::updateReflectionProbeCapture() {
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

            state.bakeTaskId =
                sceneWorkflow_->buildEnvironment(state.environmentId, true);
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
            const std::optional<AssetTaskSnapshot> bake =
                sceneWorkflow_->assetTask(state.bakeTaskId);
            if (!bake || !bake->terminal)
                return;
            if (bake->state != "Completed") {
                fail(bake->error.empty() ?
                         "Reflection probe environment bake failed" :
                         bake->error);
                return;
            }
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
}

void EditorController::applyReflectionProbeCaptureView(
    RenderViewInput &input, std::string &cameraIdentity) const {
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
    input.settings.gBufferDebugView = GBufferDebugView::None;
    input.settings.deferredLightingDebugView =
        DeferredLightingDebugView::None;
    input.settings.screenSpaceDebugView = ScreenSpaceDebugView::None;
    cameraIdentity = "reflection-probe:" + state.entityId.toString() +
                     ":" + std::to_string(state.faceIndex);
}
void EditorController::drawScenePanel(bool modelsOnly,
                                      ContentBrowserViewMode viewMode) {
    if (scenesPanel_) {
        SceneWorkflowSnapshot snapshot = sceneWorkflow_->snapshot(
            {sceneLoadContext_.maxTextureSize,
             sceneRuntime_->currentSceneIndex(),
             sceneEditorSession_ && sceneEditorSession_->active()});
        snapshot.openImportDialog =
            modelsOnly && snapshot.openImportDialog;
        if (snapshot.openImportDialog)
            sceneWorkflow_->acknowledgeImportDialog();

        SceneWorkflowActions actions;
#if VKL_ENABLE_ASSET_AUTHORING
        actions.beginModelImport = [this] {
            try {
                const auto selected =
                    openGltfFileDialog(window_->nativeHandle());
                if (!selected)
                    return;
                sceneWorkflow_->beginModelImport(*selected);
            } catch (const std::exception &error) {
                sceneWorkflow_->reportError(error.what());
            }
        };
#endif
        actions.refresh = [this] {
            try {
                refreshSceneRegistry();
                sceneWorkflow_->reportStatus("Catalog refreshed");
            } catch (const std::exception &error) {
                sceneWorkflow_->reportError(error.what());
            }
        };
        actions.selectModel = [this](int index) {
            try {
                sceneWorkflow_->selectEntry(index);
            } catch (const std::exception &error) {
                sceneWorkflow_->reportError(error.what());
            }
        };
        actions.loadPreview = [this](int index) {
            try {
                requestEditorSceneLoad(index);
            } catch (const std::exception &error) {
                sceneWorkflow_->reportError(error.what());
            }
        };
        actions.loadSceneDocument = [this](int index) {
            try {
                requestEditorSceneLoad(index);
            } catch (const std::exception &error) {
                sceneWorkflow_->reportError(error.what());
            }
        };
        actions.reimportModel = [this](int index) {
            try {
                requestSceneOperation(
                    index, false, false,
                    SceneWorkflowRequestReason::ManualReimport, true);
            } catch (const std::exception &error) {
                sceneWorkflow_->reportError(error.what());
            }
        };
        actions.validateModel = [this](int index) {
            try {
                sceneWorkflow_->validateModel(index);
            } catch (const std::exception &error) {
                sceneWorkflow_->reportError(error.what());
            }
        };
        actions.loadSourceFallback = [this](int index) {
            try {
                requestSceneOperation(index, true);
            } catch (const std::exception &error) {
                sceneWorkflow_->reportError(error.what());
            }
        };
        actions.savePreviewCamera = [this](int index) {
            try {
                sceneWorkflow_->savePreviewCamera(
                    index,
                    CameraPose{camera_.position(), camera_.yaw(),
                               camera_.pitch()});
            } catch (const std::exception &error) {
                sceneWorkflow_->reportError(error.what());
            }
        };
        actions.removeModel = [this](int index) {
            try {
                sceneWorkflow_->removeModel(index);
            } catch (const std::exception &error) {
                sceneWorkflow_->reportError(error.what());
            }
        };
        actions.openReport = [](const std::filesystem::path &path) {
            if (!path.empty())
                ShellExecuteW(nullptr, L"open", path.c_str(), nullptr,
                              nullptr, SW_SHOWNORMAL);
        };
        actions.cancelImport = [this] { sceneWorkflow_->cancelModelImport(); };
#if VKL_ENABLE_ASSET_AUTHORING
        actions.confirmImport = [this](
                                    const ModelImportPanelSubmission &input) {
            try {
                sceneWorkflow_->confirmModelImport(input);
            } catch (const std::exception &error) {
                sceneWorkflow_->reportError(error.what());
            }
        };
#endif
        actions.dismissImport = [this] {
            sceneWorkflow_->dismissModelImport();
        };
        scenesPanel_->draw(snapshot, actions, modelsOnly, viewMode);
        return;
    }
}

void EditorController::drawAssetsPanel(bool environmentsOnly) {
    if (assetsPanel_) {
        const AssetWorkflowSnapshot &snapshot = assetWorkflowSnapshot_;

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
                for (const auto &profile :
                     sceneWorkflow_->catalog().environmentProfiles)
                    result.profileIds.push_back(profile.first);
                std::sort(result.profileIds.begin(), result.profileIds.end());
                sceneWorkflow_->clearEnvironmentUiError();
                return result;
            } catch (const std::exception &error) {
                sceneWorkflow_->setEnvironmentUiError(error.what());
                return std::nullopt;
            }
        };
        actions.importEnvironment =
            [this](const EnvironmentImportSubmission &input) {
                try {
                    sceneWorkflow_->importEnvironment(input);
                } catch (const std::exception &error) {
                    sceneWorkflow_->setEnvironmentUiError(error.what());
                }
            };
        actions.buildEnvironment =
            [this](const std::string &id, bool force) {
                try {
                    sceneWorkflow_->buildEnvironment(id, force);
                } catch (const std::exception &error) {
                    sceneWorkflow_->setEnvironmentUiError(error.what());
                }
            };
        actions.removeEnvironment = [this](const std::string &id) {
            try {
                sceneWorkflow_->removeEnvironment(id);
            } catch (const std::exception &error) {
                sceneWorkflow_->setEnvironmentUiError(error.what());
            }
        };
        actions.cancelTask = [this](uint64_t id) {
            sceneWorkflow_->cancelAssetTask(id);
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

void EditorController::requestManualCapture(bool includeGui) {
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

void EditorController::drawCapturePanel() {
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

void EditorController::drawSceneLoadingPanel() {
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
        sceneRuntime_->cancelSceneLoad(task->id);
    std::lock_guard<std::mutex> lock(task->mutex);
    if (!task->error.empty())
        ImGui::TextWrapped("Error: %s", task->error.c_str());
}

void EditorController::saveEditorScene() {
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

void EditorController::executePendingEditorAction(bool saveFirst) {
    if (saveFirst) {
        saveEditorScene();
        if (hasUnsavedChanges() ||
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

void EditorController::drawSceneAuthoringDialogs() {
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
            } else if (!hasUnsavedChanges()) {
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

void EditorController::deleteSelectedEditorEntity() {
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

void EditorController::duplicateSelectedEditorEntity() {
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

void EditorController::frameSelectedEditorEntity() {
    if (!sceneEditorSession_ || !sceneEditorSession_->active() ||
        sceneEditorSession_->cameraMode() != EditorCameraMode::Editor ||
        !sceneEditorSession_->selection()) {
        return;
    }
    const std::shared_ptr<RuntimeWorld> world = sceneEditorSession_->world();
    const auto selected = world->entity(
        world->find(*sceneEditorSession_->selection()));
    if (!selected)
        return;

    glm::vec3 target = glm::vec3(selected->world[3]);
    float radius = 1.0f;
    if (const std::shared_ptr<const ModelAsset> asset =
            world->modelAsset(selected->handle);
        asset && asset->localBounds.valid) {
        glm::vec3 minimum(std::numeric_limits<float>::max());
        glm::vec3 maximum(std::numeric_limits<float>::lowest());
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 local{
                (corner & 1) ? asset->localBounds.max.x
                             : asset->localBounds.min.x,
                (corner & 2) ? asset->localBounds.max.y
                             : asset->localBounds.min.y,
                (corner & 4) ? asset->localBounds.max.z
                             : asset->localBounds.min.z};
            const glm::vec3 point = glm::vec3(
                selected->world * glm::vec4(local, 1.0f));
            minimum = glm::min(minimum, point);
            maximum = glm::max(maximum, point);
        }
        target = (minimum + maximum) * 0.5f;
        radius = std::max(glm::length(maximum - minimum) * 0.5f, 0.25f);
    }
    const glm::vec3 direction = glm::dot(camera_.front(), camera_.front()) >
                                        1.0e-6f
                                    ? glm::normalize(camera_.front())
                                    : glm::vec3(0.0f, 1.0f, 0.0f);
    camera_.setPosition(target - direction * std::max(radius * 2.5f, 1.5f));
    camera_.lookAt(target);
}

void EditorController::drawOutlinerPanel() {
    if (!outlinerPanel_ || !sceneEditorSession_ ||
        !sceneEditorSession_->active()) {
        ImGui::TextDisabled("Open a native scene.");
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
             entity.light.has_value(), entity.camera.has_value(),
             world->activeCameraId() &&
                 *world->activeCameraId() == entity.id,
             entity.light && entity.light->atmosphereSunIndex.has_value(),
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

void EditorController::drawInspectorPanel() {
    if (!inspectorPanel_ || !sceneEditorSession_ ||
        !sceneEditorSession_->active()) {
        ImGui::TextDisabled("Open a native scene.");
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
        sceneWorkflow_->assetAuthoringAvailable();
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

void EditorController::handleEditorShortcuts() {
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
        if (hasUnsavedChanges()) {
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
    if (!io.KeyCtrl && !io.KeyShift && !io.KeyAlt && pressed(ImGuiKey_F)) {
        frameSelectedEditorEntity();
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

void EditorController::draw() {
    VKL_PROFILE_ZONE("Build Editor UI");
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
    status.errorCount = static_cast<uint32_t>(
        notifications_ ? notifications_->errorCount() : 0u);

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
    panels.sceneDirty = hasUnsavedChanges();
    panels.canUndo = panels.sceneSessionActive &&
                     sceneEditorSession_->canUndo();
    panels.canRedo = panels.sceneSessionActive &&
                     sceneEditorSession_->canRedo();
    if (panels.canUndo)
        panels.undoLabel = sceneEditorSession_->undoLabel();
    if (panels.canRedo)
        panels.redoLabel = sceneEditorSession_->redoLabel();
    panels.newScene = [this]() {
        if (hasUnsavedChanges()) {
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
        if (hasUnsavedChanges())
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
                const GizmoOperation beforeOperation =
                    sceneViewportController_->operation();
                const GizmoSpace beforeSpace = sceneViewportController_->space();
                sceneViewportController_->drawToolbar();
                if (beforeOperation != sceneViewportController_->operation() ||
                    beforeSpace != sceneViewportController_->space()) {
                    EditorPreferences &preferences = preferences_->edit();
                    preferences.gizmoOperation =
                        sceneViewportController_->operation();
                    preferences.gizmoSpace = sceneViewportController_->space();
                }
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
            ImGui::SameLine();
            EditorPreferences current = preferences_->preferences();
            bool changed = false;
            changed |= editor::toggleIconButton(
                "BoundsOverlay", icons::Box, "B", "Toggle bounds overlay",
                current.showBounds, ImVec2(28.0f, 0.0f));
            if (changed)
                current.showBounds = !current.showBounds;
            ImGui::SameLine();
            if (editor::toggleIconButton(
                    "LightsOverlay", icons::Light, "L",
                    "Toggle light overlay", current.showLights,
                    ImVec2(28.0f, 0.0f))) {
                current.showLights = !current.showLights;
                changed = true;
            }
            ImGui::SameLine();
            if (editor::toggleIconButton(
                    "ProbesOverlay", icons::Grid, "P",
                    "Toggle probe overlay", current.showProbes,
                    ImVec2(28.0f, 0.0f))) {
                current.showProbes = !current.showProbes;
                changed = true;
            }
            ImGui::SameLine();
            if (editor::iconButton("ViewportShading", icons::Tune, "S",
                                   "Viewport shading",
                                   ImVec2(28.0f, 0.0f)))
                ImGui::OpenPopup("ViewportShadingPopup");
            if (ImGui::BeginPopup("ViewportShadingPopup")) {
                ImGui::SeparatorText("Shading");
                for (const ViewMode &viewMode :
                     renderSettingsController_->shaderRegistry().viewModes()) {
                    const bool selected =
                        viewMode.id == currentViewMode().id;
                    if (ImGui::Selectable(viewMode.displayName.c_str(),
                                          selected))
                        setViewMode(viewMode.id);
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (editor::iconButton("ViewportDebug", icons::Bug, "D",
                                   "Viewport debug views",
                                   ImVec2(28.0f, 0.0f)))
                ImGui::OpenPopup("ViewportDebugPopup");
            if (ImGui::BeginPopup("ViewportDebugPopup")) {
                constexpr const char *surfaceLabels[] = {
                    "None", "Normal", "Roughness", "Motion",
                    "History Validity"};
                int surface = static_cast<int>(
                    renderSettings().surfaceDebugView);
                ImGui::SetNextItemWidth(180.0f);
                if (ImGui::Combo("Surface", &surface, surfaceLabels,
                                 static_cast<int>(
                                     std::size(surfaceLabels)))) {
                    RenderSettingsPatch patch;
                    patch.surfaceDebugView =
                        static_cast<SurfaceDebugView>(surface);
                    if (*patch.surfaceDebugView != SurfaceDebugView::None) {
                        patch.gBufferDebugView = GBufferDebugView::None;
                        patch.deferredLightingDebugView =
                            DeferredLightingDebugView::None;
                        patch.screenSpaceDebugView =
                            ScreenSpaceDebugView::None;
                    }
                    applyRenderSettings(patch);
                }
                constexpr const char *screenLabels[] = {
                    "None",          "Nearest Depth",  "Scene Color",
                    "SSAO Raw",      "SSAO Filtered",  "CACAO Output",
                    "GTAO Raw",      "GTAO Temporal",  "GTAO Filtered",
                    "GTAO Rejection", "GTAO History",  "TAA History",
                    "TAA Rejection", "TAA Weight",     "SSR Raw",
                    "SSR Temporal",  "SSR Filtered",   "SSR Confidence",
                    "SSR Rejection", "SSGI Raw",       "SSGI Temporal",
                    "SSGI Filtered", "SSGI Confidence", "SSGI Variance",
                    "SSGI Rejection"};
                int screen = static_cast<int>(
                    renderSettings().screenSpaceDebugView);
                ImGui::SetNextItemWidth(180.0f);
                if (ImGui::Combo("Screen Space", &screen, screenLabels,
                                 static_cast<int>(
                                     std::size(screenLabels)))) {
                    RenderSettingsPatch patch;
                    patch.screenSpaceDebugView =
                        static_cast<ScreenSpaceDebugView>(screen);
                    if (*patch.screenSpaceDebugView !=
                        ScreenSpaceDebugView::None) {
                        patch.surfaceDebugView = SurfaceDebugView::None;
                        patch.gBufferDebugView = GBufferDebugView::None;
                        patch.deferredLightingDebugView =
                            DeferredLightingDebugView::None;
                    }
                    applyRenderSettings(patch);
                }
                constexpr const char *deferredLabels[] = {
                    "None", "Final Color", "Baseline Diffuse",
                    "Baseline Specular", "Forward Difference"};
                int deferred = static_cast<int>(
                    renderSettings().deferredLightingDebugView);
                ImGui::SetNextItemWidth(180.0f);
                if (ImGui::Combo("Deferred", &deferred, deferredLabels,
                                 static_cast<int>(
                                     std::size(deferredLabels)))) {
                    RenderSettingsPatch patch;
                    patch.deferredLightingDebugView =
                        static_cast<DeferredLightingDebugView>(deferred);
                    if (*patch.deferredLightingDebugView !=
                        DeferredLightingDebugView::None) {
                        patch.surfaceDebugView = SurfaceDebugView::None;
                        patch.gBufferDebugView = GBufferDebugView::None;
                        patch.screenSpaceDebugView =
                            ScreenSpaceDebugView::None;
                    }
                    applyRenderSettings(patch);
                }
                ImGui::EndPopup();
            }
            if (changed) {
                EditorPreferences &preferences = preferences_->edit();
                preferences.showBounds = current.showBounds;
                preferences.showLights = current.showLights;
                preferences.showProbes = current.showProbes;
            }
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
                cameraDragging_ && cameraDragging_();
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
            actions.showBounds = preferences_->preferences().showBounds;
            actions.showLights = preferences_->preferences().showLights;
            actions.showProbes = preferences_->preferences().showProbes;
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
    panels.contentBrowser = [this]() {
        const ContentBrowserViewMode mode =
            preferences_->preferences().contentView;
        const SceneWorkflowSnapshot overview = sceneWorkflow_->snapshot(
            {sceneLoadContext_.maxTextureSize,
             sceneRuntime_->currentSceneIndex(),
             sceneEditorSession_ && sceneEditorSession_->active()});
        ContentBrowserActions actions;
        actions.setViewMode = [this](ContentBrowserViewMode value) {
            preferences_->edit().contentView = value;
        };
        actions.drawScenes = [this, mode]() {
            drawScenePanel(false, mode);
        };
        actions.drawModels = [this, mode]() {
            drawScenePanel(true, mode);
        };
        actions.drawEnvironments = [this]() { drawAssetsPanel(true); };
        actions.openScene = [this](int index) {
            requestEditorSceneLoad(index);
        };
        actions.previewModel = [this](int index) {
            requestEditorSceneLoad(index);
        };
        actions.assignEnvironment = [this](const std::string &id) {
            setEnvironment(id);
        };
        contentBrowserPanel_->draw(
            {mode, &overview, &assetWorkflowSnapshot_}, actions);
    };
    panels.render = [this]() {
        RenderSettingsPanelSnapshot snapshot{};
        snapshot.advanced = preferences_->preferences().renderAdvanced;
        snapshot.features = renderSettingsController_->snapshot();
        snapshot.viewModes =
            &renderSettingsController_->shaderRegistry().viewModes();
        const ViewMode &viewMode = currentViewMode();
        snapshot.currentViewModeId = viewMode.id;
        snapshot.currentViewModeDisplayName = viewMode.displayName;
        snapshot.viewModeSupportsBloom = viewMode.supportsBloom;
        snapshot.viewModeSupportsScreenSpace = viewMode.supportsScreenSpace;
        snapshot.viewModeSupportsDdgi = viewMode.supportsDdgi;
        snapshot.textureLimit = sceneLoadContext_.maxTextureSize;
        const bool nativeSceneActive =
            sceneRuntime_->currentSceneIndex() >= 0 &&
            sceneRuntime_->currentSceneIndex() <
                static_cast<int>(sceneRegistry_.size()) &&
            sceneRegistry_[sceneRuntime_->currentSceneIndex()]
                .isNativeScene();
        snapshot.textureLimitLocked =
            config_.assetImportMode == AssetImportMode::CookedOnly ||
            nativeSceneActive;
        snapshot.textureLimitHelp =
            nativeSceneActive
                ? "Preview only. Native scenes use each model's Catalog profile."
                : "Changing texture quality reloads the current model preview.";
        snapshot.screenSpace = renderer_->screenSpaceEffectsStatus();
        snapshot.ddgi = renderer_->ddgiStatus();
        snapshot.surfaceData = renderer_->surfaceDataStatus();
        snapshot.gBuffer = renderer_->gBufferStatus();
        snapshot.deferredLighting = renderer_->deferredLightingStatus();
        snapshot.occlusion = renderer_->occlusionCullingStatus();
        snapshot.visibilityStats = visibilityFrame_.cpuStats;
        snapshot.temporalHistory = visibilityFrame_.history;
        snapshot.renderItemCount =
            static_cast<uint32_t>(visibilityFrame_.items.size());
        snapshot.lightStats = lastLightStats_;
        snapshot.atmosphere = renderer_->atmosphereStatus();
        snapshot.reflectionProbes = renderer_->reflectionProbeStatus();
        snapshot.environmentRepository =
            sceneRuntime_->environmentRepositorySnapshot();
        snapshot.lightBuffer = renderer_->sceneLightBufferStatus();
        snapshot.clusteredLighting = renderer_->clusteredLightingStatus();
        if (sceneRuntime_->currentWorld()) {
            snapshot.sceneLights = sceneRuntime_->currentWorld()->lights();
            snapshot.camera.sceneBounds =
                sceneRuntime_->currentWorld()->bounds();
        }
        snapshot.environments.reserve(catalog_.environments.size());
        for (const CatalogEnvironment &environment : catalog_.environments) {
            snapshot.environments.push_back(
                {environment.id, environment.displayName});
        }
        snapshot.selectedEnvironmentId =
            sceneRuntime_->selectedEnvironmentId();
        if (const CatalogEnvironment *environment =
                snapshot.selectedEnvironmentId.empty()
                    ? nullptr
                    : catalog_.findEnvironment(
                          snapshot.selectedEnvironmentId)) {
            snapshot.selectedEnvironmentName = environment->displayName;
        }
        snapshot.environmentIblSupported =
            device_->environmentIblSupported();
        snapshot.environmentReady = renderer_->environmentReady();
        snapshot.environmentError = sceneWorkflow_->environmentError();
        snapshot.reflectionProbeCaptureStatus =
            reflectionProbeCapture_ ? reflectionProbeCapture_->status
                                    : std::string{};
        if (const auto task = sceneRuntime_->latestEnvironmentLoadTask()) {
            const EnvironmentLoadState state = task->state.load();
            snapshot.environmentLoad = RenderEnvironmentLoadSnapshot{
                task->id,
                environmentLoadStateName(state),
                task->uploadedImages.load(),
                task->totalImages,
                !isTerminalEnvironmentLoadState(state)};
        }
        snapshot.ambientColor = ambientColor_;
        snapshot.ambientIntensity = ambientIntensity_;
        snapshot.fallbackSunDirection = defaultSunDirection_;
        snapshot.fallbackSunColor = defaultSunColor_;
        snapshot.fallbackSunIntensity = defaultSunIntensity_;
        snapshot.camera.position = camera_.position();
        snapshot.camera.moveSpeed = config_.moveSpeed;
        snapshot.camera.nearPlane = camera_.nearPlane();
        snapshot.camera.farPlane = camera_.farPlane();

        RenderSettingsPanelActions actions;
        actions.setAdvanced = [this](bool value) {
            preferences_->edit().renderAdvanced = value;
        };
        actions.setViewMode = [this](const std::string &id) {
            setViewMode(id);
        };
        actions.setTextureLimit = [this](uint32_t limit) {
            setTextureLimit(limit);
        };
        actions.applySettings = [this](const RenderSettingsPatch &patch) {
            applyRenderSettings(patch);
        };
        actions.setEnvironment = [this](const std::string &id) {
            try {
                setEnvironment(id);
                sceneWorkflow_->clearEnvironmentUiError();
            } catch (const std::exception &error) {
                sceneWorkflow_->setEnvironmentUiError(error.what());
            }
        };
        actions.cancelEnvironmentLoad = [this](uint64_t taskId) {
            sceneRuntime_->cancelEnvironmentLoad(taskId);
        };
        actions.setAmbientColor = [this](glm::vec3 value) {
            ambientColor_ = value;
        };
        actions.setAmbientIntensity = [this](float value) {
            ambientIntensity_ = value;
        };
        actions.setFallbackSunDirection = [this](glm::vec3 value) {
            defaultSunDirection_ = value;
        };
        actions.setFallbackSunColor = [this](glm::vec3 value) {
            defaultSunColor_ = value;
        };
        actions.setFallbackSunIntensity = [this](float value) {
            defaultSunIntensity_ = value;
        };
        if (sceneEditorSession_ && sceneEditorSession_->active() &&
            !snapshot.ddgi.componentPresent) {
            actions.createDdgiProbeVolume = [this]() {
                try {
                    const std::shared_ptr<RuntimeWorld> world =
                        sceneEditorSession_->world();
                    const Bounds bounds = world->bounds();
                    SceneEntityDocument entity;
                    entity.id = PersistentEntityId::generate();
                    entity.name = "DDGI Probe Volume";
                    entity.ddgiProbeVolume =
                        DdgiProbeVolumeComponentDocument{};
                    if (bounds.valid) {
                        const glm::vec3 extent = glm::max(
                            bounds.max - bounds.min, glm::vec3(0.5f));
                        const float longest = std::max(
                            {extent.x, extent.y, extent.z, 0.5f});
                        auto probeCount = [longest](float axisExtent,
                                                   uint32_t minimum) {
                            return glm::clamp(
                                static_cast<uint32_t>(std::lround(
                                    12.0f * axisExtent / longest)),
                                minimum, 12u);
                        };
                        const glm::uvec3 counts{
                            probeCount(extent.x, 2u),
                            probeCount(extent.y, 2u),
                            probeCount(extent.z, 3u)};
                        const glm::vec3 spacing = glm::max(
                            extent * 1.05f /
                                glm::vec3(glm::max(
                                    counts - glm::uvec3(1u),
                                    glm::uvec3(1u))),
                            glm::vec3(0.25f));
                        entity.transform.translation = bounds.center;
                        entity.ddgiProbeVolume->probeCounts = counts;
                        entity.ddgiProbeVolume->probeSpacing = spacing;
                        entity.ddgiProbeVolume->maxRayDistance =
                            glm::clamp(
                                std::max({spacing.x, spacing.y, spacing.z}) *
                                    8.0f,
                                4.0f, 100.0f);
                        entity.ddgiProbeVolume->probesUpdatedPerFrame =
                            std::min(64u,
                                     counts.x * counts.y * counts.z);
                    }
                    const PersistentEntityId id = entity.id;
                    sceneEditorSession_->execute(
                        "Create Fitted DDGI Probe Volume",
                        [entity = std::move(entity)](
                            RuntimeWorld &runtimeWorld) mutable {
                            const EntityHandle created =
                                runtimeWorld.createEntity(std::move(entity));
                            return static_cast<bool>(created);
                        });
                    sceneEditorSession_->select(id);
                } catch (const std::exception &error) {
                    editorUi_->sceneError = error.what();
                }
            };
        }
        actions.setCameraMoveSpeed = [this](float value) {
            config_.moveSpeed = value;
            preferences_->edit().cameraMoveSpeed = value;
        };
        actions.setCameraClipPlanes = [this](float nearPlane,
                                              float farPlane) {
            camera_.setClipPlanes(nearPlane, farPlane);
        };
        renderSettingsPanel_->draw(snapshot, actions);
    };
    panels.materials = [this]() {
        MaterialsPanelSnapshot snapshot{};
        snapshot.sceneGeneration = sceneRuntime_->sceneGeneration();
        const MaterialBindingStatus &binding = materialSystem_->status();
        snapshot.bindingMode = binding.active;
        snapshot.activeMaterials = binding.activeMaterials;
        snapshot.materialCapacity = binding.materialCapacity;

        std::vector<std::shared_ptr<MaterialInstance>> selectedMaterials;
        const std::vector<std::shared_ptr<MaterialInstance>> *materials =
            sceneRuntime_->currentWorld()
                ? &sceneRuntime_->currentWorld()->materials()
                : nullptr;
        snapshot.sceneLoaded = materials != nullptr;
        if (sceneEditorSession_ && sceneEditorSession_->active()) {
            snapshot.sceneLoaded = true;
            materials = &selectedMaterials;
            if (sceneEditorSession_->selection()) {
                const std::shared_ptr<RuntimeWorld> world =
                    sceneEditorSession_->world();
                if (const auto asset = world->modelAsset(
                        world->find(*sceneEditorSession_->selection())))
                    selectedMaterials = asset->materials;
            }
        }
        if (materials) {
            snapshot.materials.reserve(materials->size());
            for (size_t index = 0; index < materials->size(); ++index) {
                const auto &material = (*materials)[index];
                if (!material)
                    continue;
                MaterialPanelItem item{};
                item.sourceIndex = static_cast<uint32_t>(index);
                item.params = material->params();
                item.gpuMaterialIndex = material->materialIndex();
                item.shaderFamily =
                    renderSettingsController_->shaderRegistry()
                        .materialShaderFamily(
                            material->materialTemplate().shaderFamily())
                        .displayName;
                const auto &textures = material->textures();
                const GpuMaterial *gpuMaterial =
                    materialSystem_->gpuMaterial(material->materialHandle());
                item.textureSlots = {
                    gpuMaterial ? gpuMaterial->textureIndices0.x : 0u,
                    gpuMaterial ? gpuMaterial->textureIndices0.y : 0u,
                    gpuMaterial ? gpuMaterial->textureIndices0.z : 0u,
                    gpuMaterial ? gpuMaterial->textureIndices0.w : 0u,
                    gpuMaterial ? gpuMaterial->textureIndices1.x : 0u};
                for (size_t slot = 0; slot < kMaterialTextureSlotCount;
                     ++slot)
                    item.texturesBound[slot] =
                        static_cast<bool>(textures[slot]);
                snapshot.materials.push_back(std::move(item));
            }
        }
        materialsPanel_->draw(snapshot);
    };
    panels.bottomDrawer = [this, hasActiveLoad]() {
        DiagnosticsPanelSnapshot snapshot{};
        snapshot.sceneLoadActive = hasActiveLoad;
        snapshot.captureAvailable = captureService_ != nullptr;
        snapshot.fps = ImGui::GetIO().Framerate;
        snapshot.gpuTimings = renderer_->gpuPassTimings();
        snapshot.gpuFrameMs = snapshot.gpuTimings.available
                                  ? static_cast<float>(
                                        snapshot.gpuTimings.totalMs)
                                  : 0.0f;
        snapshot.cameraDragging = cameraDragging_ && cameraDragging_();
        snapshot.renderableCount =
            sceneRuntime_->currentWorld()
                ? sceneRuntime_->currentWorld()->renderableCount()
                : 0;
        snapshot.tracyCompiled = build::kTracy;
        snapshot.tracyConnected = device_->tracyProfiler().connected();
        snapshot.tracyGpuAvailable =
            device_->tracyProfiler().gpuAvailable();
        snapshot.renderPath = renderer_->renderPathStatus();
        snapshot.gBuffer = renderer_->gBufferStatus();
        snapshot.deferredLighting = renderer_->deferredLightingStatus();
        snapshot.clusteredLighting = renderer_->clusteredLightingStatus();
        snapshot.renderGraph = renderer_->renderGraphDiagnostics();
        const auto &lastSceneLoad = sceneRuntime_->lastSceneLoadStats();
        snapshot.lastSceneLoad = lastSceneLoad ? &*lastSceneLoad : nullptr;
        snapshot.materialBinding = materialSystem_->status();
        snapshot.modelRepository = sceneRuntime_->modelRepositorySnapshot();

        DiagnosticsPanelActions actions;
        actions.drawSceneLoad = [this]() { drawSceneLoadingPanel(); };
        actions.drawTasks = [this]() { drawAssetsPanel(false); };
#if VKL_ENABLE_CAPTURE
        actions.drawCapture = [this]() { drawCapturePanel(); };
#endif
        actions.exportRenderGraphJson = [this]() {
            std::filesystem::create_directories("logs");
            std::ofstream("logs/render_graph.json", std::ios::binary)
                << renderer_->renderGraphJson();
        };
        actions.exportRenderGraphDot = [this]() {
            std::filesystem::create_directories("logs");
            std::ofstream("logs/render_graph.dot", std::ios::binary)
                << renderer_->renderGraphDot();
        };
        diagnosticsPanel_->draw(snapshot, actions);
    };

    panels.openCommandPalette = [this]() {
        if (commandPalette_)
            commandPalette_->requestOpen();
    };

    actionRegistry_->clear();
    const auto addAction = [this](std::string id, std::string label,
                                  const char *icon, std::string shortcut,
                                  std::string keywords,
                                  std::function<void()> callback,
                                  std::function<bool()> enabled = {}) {
        actionRegistry_->add(EditorAction{
            std::move(id), std::move(label), std::move(keywords),
            std::move(shortcut), icon, std::move(enabled),
            std::move(callback)});
    };
    addAction("file.new", "New Scene", icons::Plus, "Ctrl+N",
              "file create", panels.newScene);
    addAction("file.open", "Open Scene", icons::Folder, "Ctrl+O",
              "file scene", panels.openScene);
    addAction("file.save", "Save Scene", icons::Save, "Ctrl+S",
              "file", panels.saveScene,
              [active = panels.sceneSessionActive]() { return active; });
    addAction("file.save_as", "Save Scene As", icons::Save,
              "Ctrl+Shift+S", "file duplicate", panels.saveSceneAs,
              [active = panels.sceneSessionActive]() { return active; });
    addAction("file.close", "Close Scene", icons::Close, {}, "file",
              panels.closeScene,
              [active = panels.sceneSessionActive]() { return active; });
    addAction("file.convert", "Convert Model Preview", icons::Scene, {},
              "file scene model", panels.convertPreview,
              [active = panels.sceneSessionActive]() { return !active; });
    addAction("edit.undo", "Undo", icons::Undo, "Ctrl+Z", "edit",
              panels.undo, [enabled = panels.canUndo]() { return enabled; });
    addAction("edit.redo", "Redo", icons::Redo, "Ctrl+Y", "edit",
              panels.redo, [enabled = panels.canRedo]() { return enabled; });
    addAction("edit.duplicate", "Duplicate Entity", icons::Copy,
              "Ctrl+D", "edit entity", [this]() {
                  duplicateSelectedEditorEntity();
              },
              [this]() {
                  return sceneEditorSession_ &&
                         sceneEditorSession_->selection().has_value();
              });
    addAction("edit.delete", "Delete Entity", icons::Trash, "Delete",
              "edit entity", [this]() { deleteSelectedEditorEntity(); },
              [this]() {
                  return sceneEditorSession_ &&
                         sceneEditorSession_->selection().has_value();
              });
    addAction("view.maximize", "Maximize Viewport", icons::Maximize,
              "Ctrl+Space", "view focus viewport", [this]() {
                  editorDockWorkspace_->toggleViewportMaximized();
              });
    addAction("view.frame_selected", "Frame Selected", icons::Focus, "F",
              "view camera entity", [this]() {
                  frameSelectedEditorEntity();
              },
              [this]() {
                  return sceneEditorSession_ &&
                         sceneEditorSession_->selection().has_value() &&
                         sceneEditorSession_->cameraMode() ==
                             EditorCameraMode::Editor;
              });
    addAction("view.drawer", "Toggle Bottom Drawer", icons::DrawerOpen,
              {}, "tasks diagnostics", [this]() {
                  editorDockWorkspace_->toggleBottomDrawer();
              });

    if (ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_P, false))
        commandPalette_->requestOpen();
    if (ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_Space, false))
        actionRegistry_->invoke("view.maximize");

    handleEditorShortcuts();

    const VkExtent2D renderExtent = renderer_->viewportExtent();
    EditorViewportFrame viewportFrame{};
    viewportFrame.textureId =
        gui_->viewportTextureId(frameSync_->nextFrameIndex());
    viewportFrame.renderWidth = renderExtent.width;
    viewportFrame.renderHeight = renderExtent.height;
    viewportFrame.resizePending = viewportResize_.pending;
    editorDockWorkspace_->draw(status, viewportFrame, panels);
    commandPalette_->draw(*actionRegistry_);
    notifications_->draw(preferences_->preferences().bottomDrawerExpanded
                             ? preferences_->preferences().bottomDrawerHeight +
                                   12.0f
                             : 40.0f);
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
void EditorController::bindViewportTextures() {
    if (!gui_ || !renderer_)
        return;
    const RendererViewportOutput output = renderer_->viewportOutput();
    gui_->setViewportTextures(output.sampler, output.imageViews);
}

void EditorController::applyPendingViewportResize() {
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

void EditorController::onWorldPublished(
    const SceneRuntimePublication &publication) {
    if (!sceneEditorSession_)
        return;
    if (publication.kind == SceneLoadKind::NativeScene &&
        publication.document) {
        const std::shared_ptr<RuntimeWorld> runtimeWorld =
            std::dynamic_pointer_cast<RuntimeWorld>(publication.world);
        if (!runtimeWorld)
            throw std::logic_error(
                "Native scene publication did not contain a RuntimeWorld");
        sceneEditorSession_->attach(
            runtimeWorld, publication.document->path,
            publication.document->sourceStamp);
        sceneEditorSession_->setWorldChangedCallback([this]() {
            updateEditorModelBindings();
            updateEditorReflectionProbeBindings();
        });
    } else {
        sceneEditorSession_->detach();
    }
}

void EditorController::update() {
    updateReflectionProbeCapture();
    preferences_->update();
    notifications_->update();

    if (!editorUi_->sceneError.empty() &&
        editorUi_->sceneError != lastNotifiedSceneError_) {
        lastNotifiedSceneError_ = editorUi_->sceneError;
        notifications_->push(editor::StatusTone::Error, "Scene operation failed",
                             editorUi_->sceneError, "Open Tasks", [this]() {
                                 if (!preferences_->preferences()
                                          .bottomDrawerExpanded)
                                     editorDockWorkspace_->toggleBottomDrawer();
                             });
    } else if (editorUi_->sceneError.empty()) {
        lastNotifiedSceneError_.clear();
    }
    if (!captureUiError_.empty() &&
        captureUiError_ != lastNotifiedCaptureError_) {
        lastNotifiedCaptureError_ = captureUiError_;
        notifications_->push(editor::StatusTone::Error, "Capture failed",
                             captureUiError_);
    } else if (captureUiError_.empty()) {
        lastNotifiedCaptureError_.clear();
    }

    const auto openTasks = [this]() {
        if (!preferences_->preferences().bottomDrawerExpanded)
            editorDockWorkspace_->toggleBottomDrawer();
    };
    if (const std::shared_ptr<SceneLoadTask> task =
            sceneRuntime_->latestSceneLoadTask();
        task && isTerminalSceneLoadState(task->state.load()) &&
        notifiedSceneTasks_.insert(task->id).second) {
        const SceneLoadState state = task->state.load();
        std::string message = task->sceneName;
        if (state == SceneLoadState::Failed) {
            std::scoped_lock lock(task->mutex);
            if (!task->error.empty())
                message = task->error;
        }
        notifications_->push(
            state == SceneLoadState::Completed
                ? editor::StatusTone::Success
                : (state == SceneLoadState::Failed
                       ? editor::StatusTone::Error
                       : editor::StatusTone::Warning),
            state == SceneLoadState::Completed
                ? "Scene loaded"
                : (state == SceneLoadState::Failed ? "Scene load failed"
                                                   : "Scene load cancelled"),
            std::move(message), "Open Tasks", openTasks);
    }

    assetWorkflowSnapshot_ = sceneWorkflow_->assetSnapshot();
    const auto notifyAssetTask = [&](const AssetTaskSnapshot &task) {
        if (!task.terminal || !notifiedAssetTasks_.insert(task.id).second)
            return;
        const bool failed = !task.error.empty() || task.failed > 0;
        notifications_->push(
            failed ? editor::StatusTone::Error
                   : editor::StatusTone::Success,
            failed ? "Asset task failed" : "Asset task completed",
            failed ? task.error : task.kind + ": " + task.assetId,
            "Open Tasks", openTasks);
    };
    if (assetWorkflowSnapshot_.activeTask)
        notifyAssetTask(*assetWorkflowSnapshot_.activeTask);
    for (const AssetTaskSnapshot &task : assetWorkflowSnapshot_.recentTasks)
        notifyAssetTask(task);

    if (captureService_) {
        for (const CaptureTaskSnapshot &task : captureService_->tasks()) {
            if (!isTerminalCaptureTaskState(task.state) ||
                !notifiedCaptureTasks_.insert(task.request.taskId).second) {
                continue;
            }
            const bool failed = task.state == CaptureTaskState::Failed;
            const bool completed =
                task.state == CaptureTaskState::Completed;
            notifications_->push(
                failed ? editor::StatusTone::Error
                       : (completed ? editor::StatusTone::Success
                                    : editor::StatusTone::Warning),
                failed ? "Capture failed"
                       : (completed ? "Capture saved"
                                    : "Capture cancelled"),
                failed ? task.result.error
                       : task.result.outputPath.string(),
                "Open Tasks", openTasks);
        }
    }
}

void EditorController::beginFrame() {
    sceneViewportController_->beginFrame();
}

bool EditorController::interceptCloseRequest() {
    if (!hasUnsavedChanges() || editorUi_->quitConfirmed)
        return false;
    editorUi_->pendingAction = EditorPendingActionKind::Quit;
    editorUi_->requestDirtyModal = true;
    return true;
}

bool EditorController::handleEscape() {
    if (sceneViewportController_->manipulationActive()) {
        sceneViewportController_->cancelManipulation();
        return true;
    }
    if (hasUnsavedChanges()) {
        editorUi_->pendingAction = EditorPendingActionKind::Quit;
        editorUi_->requestDirtyModal = true;
        return true;
    }
    return false;
}

void EditorController::requestManualCapture() {
    requestManualCapture(captureIncludeGui_);
}

bool EditorController::activeSceneCamera() const {
    return sceneEditorSession_ && sceneEditorSession_->active() &&
           sceneEditorSession_->cameraMode() ==
               EditorCameraMode::ActiveScene;
}

bool EditorController::viewportHovered() const {
    return editorDockWorkspace_ &&
           editorDockWorkspace_->viewportState().hovered;
}

bool EditorController::blocksViewportInput() const {
    return sceneViewportController_ &&
           sceneViewportController_->blocksViewportInput();
}

bool EditorController::anyItemActive() const {
    return ImGui::IsAnyItemActive();
}

void EditorController::setCameraDragActive(bool active) {
    ImGuiIO &io = ImGui::GetIO();
    if (active)
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    else
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
}

std::optional<PersistentEntityId>
EditorController::focusedLightEntity() const {
    if (!sceneEditorSession_ || !sceneEditorSession_->active() ||
        !sceneEditorSession_->selection())
        return std::nullopt;
    const auto world = sceneEditorSession_->world();
    const auto selected =
        world ? world->entity(world->find(*sceneEditorSession_->selection()))
              : std::nullopt;
    if (!selected || !selected->light)
        return std::nullopt;
    return selected->id;
}

EditorViewportDiagnostics EditorController::viewportDiagnostics() const {
    return {viewportDisplayWidth_, viewportDisplayHeight_, viewportVisible_,
            viewportHovered_, viewportResize_.pending};
}

} // namespace vkr
