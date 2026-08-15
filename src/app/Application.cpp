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
#include "control/RuntimeControlAdapter.h"
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
}

Application::~Application() {
#if VKL_ENABLE_RUNTIME_CONTROL
    runtimeControl_.reset();
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
    if (runtimeControl_) {
        runtimeControl_->start();
    } else {
        VKR_LOG_INFO(
            "Control",
            "Runtime control disabled; pass --runtime-control to enable.");
    }
    try {
        mainLoop();
    } catch (...) {
        if (runtimeControl_)
            runtimeControl_->stop();
        throw;
    }
    if (runtimeControl_)
        runtimeControl_->stop();
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

#if VKL_ENABLE_RUNTIME_CONTROL
    if (config_.enableRuntimeControl) {
        RuntimeControlActions controlActions;
        controlActions.requestSceneOperation =
            [this](int index, bool sourceFallback, bool loadAfter,
                   SceneWorkflowRequestReason reason, bool forceReimport,
                   bool reloadAsset) {
                return requestSceneOperation(index, sourceFallback, loadAfter,
                                             reason, forceReimport,
                                             reloadAsset);
            };
        controlActions.setTextureLimit =
            [this](uint32_t limit) { return setTextureLimit(limit); };
        controlActions.setEnvironment =
            [this](const std::string &id) { return setEnvironment(id); };
        controlActions.reloadEnvironment =
            [this]() { return reloadCurrentEnvironment(); };
        controlActions.cancelLoadOperation =
            [this](uint64_t taskId) { return cancelLoadOperation(taskId); };
        controlActions.hasUnsavedChanges =
            [this]() { return hasUnsavedSceneChanges(); };

        std::function<RuntimeViewportSnapshot()> viewportSnapshot;
#if VKL_ENABLE_EDITOR_UI
        if (editorController_) {
            viewportSnapshot = [this]() {
                const EditorViewportDiagnostics source =
                    editorController_->viewportDiagnostics();
                return RuntimeViewportSnapshot{
                    source.displayWidth, source.displayHeight,
                    source.visible, source.hovered,
                    source.resizePending};
            };
        }
#endif
        runtimeControl_ = std::make_unique<RuntimeControlAdapter>(
            RuntimeControlServices{
                config_, projectContext_, catalog_, sceneRegistry_,
                sceneLoadContext_, *window_, *context_, *device_,
                *frameSync_, *swapChain_, *renderer_, *materialSystem_,
                *sceneWorkflow_, *sceneRuntime_, *renderSettingsController_,
                captureService_.get(), camera_, visibilityFrame_,
                lastLightStats_, presentedFrameCount_, gui_ != nullptr,
                std::move(viewportSnapshot), std::move(controlActions)});
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
        throw SceneWorkflowError(
            "invalid_texture_limit",
            "Texture limit must be 0, 512, 1024, or 2048.");
    if (sceneLoadContext_.maxTextureSize == limit)
        return 0;
    if (config_.assetImportMode == AssetImportMode::CookedOnly) {
        throw SceneWorkflowError(
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
    return sceneRuntime_->requestSceneLoad(index, sourceFallback,
                                           reloadAsset);
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
        throw SceneRuntimeError(
            "environment_unsupported",
            "The selected Vulkan device does not support the required "
            "RGBA16F cubemap and RG16F filtering features.");
    }
    const CatalogEnvironment *environment = findEnvironmentByName(id);
    if (!environment) {
        throw SceneRuntimeError("unknown_environment",
                                "Unknown environment '" + id + "'.");
    }
    return queueEnvironmentLoad(*environment);
}

uint64_t Application::queueEnvironmentLoad(
    const CatalogEnvironment &environment, bool reload) {
    return sceneRuntime_->queueEnvironment(environment, reload);
}

EnvironmentAssetHandle Application::requestEnvironmentAsset(
    const CatalogEnvironment &environment, bool reload,
    bool *repositoryHit, bool *coalesced) {
    return sceneRuntime_->requestEnvironmentAsset(
        environment, reload, repositoryHit, coalesced);
}

uint64_t Application::reloadCurrentEnvironment() {
    return sceneRuntime_->reloadEnvironment();
}

void Application::setShaderVariant(const std::string &id) {
    const std::string previous = currentShaderVariant().id;
    renderSettingsController_->setShaderVariant(id);
    if (previous != id) {
        VKR_LOG_INFO("Renderer", "Shader variant switched to {}",
                     currentShaderVariant().displayName);
    }
}

void Application::applyRenderSettings(const RenderSettingsPatch &patch) {
    renderSettingsController_->apply(patch);
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
    return sceneWorkflow_->requestEntry(
        index, sourceFallback, loadAfter, reason, forceReimport,
        reloadAsset);
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
        if (runtimeControl_ && runtimeControl_->processOne()) {
            window_->setShouldClose(true);
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
