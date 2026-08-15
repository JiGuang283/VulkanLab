#include "Application.h"
#include "render/frame/RenderSettingsController.h"
#include "scene/SceneRuntimeCoordinator.h"
#include "workflows/SceneWorkflowController.h"

#include <BuildFeatures.h>

#include "assets/EnvironmentLoadManager.h"
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
#include "diagnostics/CaptureService.h"
#include "diagnostics/Profiling.h"
#include "diagnostics/TracyProfiler.h"
#if VKL_ENABLE_EDITOR_UI
#include "editor/EditorController.h"
#include "editor/GuiSystem.h"
#endif
#include "render/frame/FrameGpuData.h"
#include "render/material/MaterialSystem.h"
#include "render/pipeline/PipelineCache.h"
#include "render/frame/RenderView.h"
#include "render/Renderer.h"
#include "render/features/shadows_visibility/ShadowSystem.h"
#include "render/features/temporal_post_process/TemporalAA.h"
#include "render/features/shadows_visibility/Visibility.h"
#include "scene/Camera.h"
#include "scene/EnvironmentAssetHandle.h"
#include "render/IRenderWorld.h"
#include "scene/ModelPrepareFactory.h"
#include "scene/SceneEntry.h"
#include "scene/SceneLoadTask.h"
#include "scene_data/SceneIds.h"
#include "window/InputManager.h"
#include "window/Window.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

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

} // namespace

enum class InputMode {
    UI,
    CameraDrag,
};

struct Application::PlatformServices {
    std::unique_ptr<Window> window;
    std::unique_ptr<InputManager> input;
    std::unique_ptr<VulkanContext> context;
    std::unique_ptr<Device> device;
    std::unique_ptr<DescriptorAllocator> descriptorAllocator;
    std::unique_ptr<MaterialSystem> materialSystem;
    std::unique_ptr<SwapChain> swapChain;
    std::unique_ptr<FrameSync> frameSync;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<PipelineCache> pipelineCache;
};

struct Application::RuntimeServices {
    RuntimeServices(const ProjectContext &projectContext,
                    SceneCatalog catalogValue)
        : sceneWorkflow(std::make_unique<SceneWorkflowController>(
              projectContext, std::move(catalogValue))),
          catalog(sceneWorkflow->catalog()),
          sceneRegistry(sceneWorkflow->entries()),
          renderSettings(std::make_unique<RenderSettingsController>(
              projectContext.resolveRuntimePath("shader/manifest.json"))) {}

    std::unique_ptr<SceneWorkflowController> sceneWorkflow;
    const SceneCatalog &catalog;
    const std::vector<SceneEntry> &sceneRegistry;
    std::unique_ptr<RenderSettingsController> renderSettings;
    SceneLoadContext sceneLoadContext;
    std::unique_ptr<SceneRuntimeCoordinator> scene;
};

struct Application::OptionalTooling {
#if VKL_ENABLE_EDITOR_UI
    std::unique_ptr<GuiSystem> gui;
    std::unique_ptr<EditorController> editor;
#endif
    std::unique_ptr<CaptureService> capture;
#if VKL_ENABLE_RUNTIME_CONTROL
    std::unique_ptr<RuntimeControlAdapter> runtimeControl;
#endif
};

struct Application::FrameState {
    std::vector<RenderItem> renderItems;
    VisibilitySystem visibilitySystem;
    VisibilityFrame visibilityFrame;
    ShadowSystem shadowSystem;
    uint64_t presentedFrameCount = 0;
    InputMode inputMode = InputMode::UI;
    glm::dvec2 savedCursor{};
    Camera camera;
    glm::vec3 ambientColor{1.0f};
    float ambientIntensity = 0.08f;
    glm::vec3 defaultSunDirection{0.3f, 0.8f, 0.5f};
    glm::vec3 defaultSunColor{1.0f};
    float defaultSunIntensity = 3.0f;
    RenderViewLightStats lastLightStats{};
};

Application::Application(const Config &config, ProjectContext projectContext,
                         SceneCatalog catalog)
    : config_(config), projectContext_(std::move(projectContext)),
      platform_(std::make_unique<PlatformServices>()),
      runtime_(std::make_unique<RuntimeServices>(projectContext_,
                                                 std::move(catalog))),
      tooling_(std::make_unique<OptionalTooling>()),
      frame_(std::make_unique<FrameState>()) {
    if (config_.derivedTextureCachePath.empty())
        config_.derivedTextureCachePath = projectContext_.cacheRoot.u8string();
    const ShaderRegistry &shaderRegistry =
        runtime_->renderSettings->shaderRegistry();
    VKR_LOG_INFO(
        "ShaderRegistry", "Loaded {} programs and {} variants; default={}",
        shaderRegistry.programs().size(), shaderRegistry.variants().size(),
        runtime_->renderSettings->currentShaderVariant().id);
}

Application::~Application() { shutdown(); }

void Application::shutdown() noexcept {
    if (shutdown_)
        return;
    shutdown_ = true;

#if VKL_ENABLE_RUNTIME_CONTROL
    if (tooling_ && tooling_->runtimeControl) {
        tooling_->runtimeControl->stop();
        tooling_->runtimeControl.reset();
    }
#endif
    try {
        if (runtime_ && runtime_->sceneWorkflow)
            runtime_->sceneWorkflow->shutdown();
        if (runtime_ && runtime_->scene)
            runtime_->scene->shutdown();

        if (platform_ && platform_->device) {
            vkDeviceWaitIdle(platform_->device->logicalDevice());
            if (platform_->frameSync)
                platform_->frameSync->markAllSubmissionsCompleted();
            if (runtime_ && runtime_->scene)
                runtime_->scene->collectRetired();
            if (tooling_ && tooling_->capture && platform_->frameSync) {
                tooling_->capture->shutdown(
                    platform_->frameSync->completedSubmissionSerial());
            }
        }
    } catch (const std::exception &error) {
        VKR_LOG_ERROR("Application", "Shutdown failed: {}", error.what());
    } catch (...) {
        VKR_LOG_ERROR("Application", "Shutdown failed with an unknown error");
    }

#if VKL_ENABLE_EDITOR_UI
    if (tooling_)
        tooling_->editor.reset();
#endif
    if (runtime_)
        runtime_->scene.reset();
    tooling_.reset();
    runtime_.reset();
    frame_.reset();
    platform_.reset();
}

void Application::run() {
    profileSetThreadName("Main");
    {
        VKL_PROFILE_ZONE("Application Init");
        init();
    }
#if VKL_ENABLE_RUNTIME_CONTROL
    if (tooling_->runtimeControl)
        tooling_->runtimeControl->start();
    else
        VKR_LOG_INFO(
            "Control",
            "Runtime control disabled; pass --runtime-control to enable.");
#endif

    try {
        mainLoop();
    } catch (...) {
        shutdown();
        throw;
    }
    shutdown();
}

void Application::init() {
    initPlatformAndRenderer();
    initSceneRuntime();
    initOptionalTooling();
}

void Application::initPlatformAndRenderer() {
    platform_->window = std::make_unique<Window>(
        config_.windowWidth, config_.windowHeight, config_.windowTitle,
        config_.diagnostics.windowResizable());
    platform_->input = std::make_unique<InputManager>(*platform_->window);

    auto extensions = Window::getRequiredVulkanExtensions();
    VulkanContextOptions contextOptions;
    contextOptions.validationProfile = config_.validationProfile;
    contextOptions.validationAllowed =
        build::kValidation && !projectContext_.cookedPackage;
    contextOptions.debugUtilsRequested =
        build::kGpuDebugUtils && !projectContext_.cookedPackage;
    platform_->context = std::make_unique<VulkanContext>(
        [this](VkInstance inst) {
            return platform_->window->createSurface(inst);
        },
        std::move(extensions), contextOptions);
    platform_->device = std::make_unique<Device>(*platform_->context,
                                                 config_.materialBindingMode);
    platform_->descriptorAllocator =
        std::make_unique<DescriptorAllocator>(*platform_->device);
    platform_->materialSystem = std::make_unique<MaterialSystem>(
        *platform_->device, *platform_->descriptorAllocator,
        config_.materialBindingMode,
        runtime_->renderSettings->shaderRegistry().supportsBindlessMaterials());
    VKR_LOG_INFO(
        "Material",
        "Material binding: requested={} active={} textures={} materials={}{}",
        materialBindingModeName(config_.materialBindingMode),
        materialBindingModeName(platform_->materialSystem->activeMode()),
        platform_->materialSystem->status().textureCapacity,
        platform_->materialSystem->status().materialCapacity,
        platform_->materialSystem->status().fallbackReason.empty()
            ? std::string{}
            : " fallback=" +
                  platform_->materialSystem->status().fallbackReason);
    platform_->swapChain = std::make_unique<SwapChain>(
        *platform_->device, platform_->context->surface(),
        [this]() { return platform_->window->framebufferExtent(); });
    platform_->frameSync =
        std::make_unique<FrameSync>(*platform_->device, *platform_->swapChain);
    platform_->renderer = std::make_unique<Renderer>(
        *platform_->device, *platform_->swapChain, *platform_->frameSync,
        *platform_->descriptorAllocator, *platform_->materialSystem,
        runtime_->renderSettings->shaderRegistry(),
        platform_->materialSystem->activeMode());
    RenderSettingsCallbacks renderSettingsCallbacks;
    renderSettingsCallbacks.reconfigureCacao =
        [this](CacaoResolution resolution) {
            platform_->frameSync->waitForAllFrames();
            std::string error;
            if (!platform_->renderer->reconfigureCacao(resolution, error)) {
                throw RenderSettingsError(
                    "cacao_reconfigure_failed",
                    error.empty() ? "Failed to reconfigure FidelityFX CACAO."
                                  : error);
            }
        };
    runtime_->renderSettings->configure(platform_->renderer->featureSupport(),
                                        std::move(renderSettingsCallbacks));
    platform_->window->setResizeCallback(
        [this](int, int) { platform_->frameSync->notifyResize(); });

    frame_->camera.setAspect(
        static_cast<float>(platform_->swapChain->extent().width) /
        static_cast<float>(platform_->swapChain->extent().height));

    platform_->pipelineCache =
        std::make_unique<PipelineCache>(*platform_->device);
}

void Application::initSceneRuntime() {

    if (runtime_->sceneRegistry.empty())
        throw std::runtime_error(
            "No model previews or native scenes are registered in the "
            "scene catalog.");

    int start =
        std::clamp(config_.defaultSceneIndex, 0,
                   static_cast<int>(runtime_->sceneRegistry.size()) - 1);
    if (projectContext_.nativeScenePackage) {
        const auto found = std::find_if(
            runtime_->sceneRegistry.begin(), runtime_->sceneRegistry.end(),
            [this](const SceneEntry &entry) {
                return entry.isNativeScene() &&
                       entry.id == projectContext_.startupSceneId;
            });
        if (found == runtime_->sceneRegistry.end()) {
            throw std::runtime_error(
                "Cooked package startup scene is not registered: " +
                projectContext_.startupSceneId);
        }
        start = static_cast<int>(found - runtime_->sceneRegistry.begin());
    }
    runtime_->sceneLoadContext.maxTextureSize = config_.gltfMaxTextureSize;
    runtime_->sceneLoadContext.derivedTextureCachePath =
        config_.derivedTextureCachePath;
    runtime_->sceneLoadContext.projectId = runtime_->catalog.projectId;
    runtime_->sceneLoadContext.textureTranscodeTarget =
        platform_->device->textureTranscodeTarget();
    if (projectContext_.cookedPackage &&
        projectContext_.requiredTextureEncoder == "bc7" &&
        runtime_->sceneLoadContext.textureTranscodeTarget !=
            TextureTranscodeTarget::Bc7) {
        throw std::runtime_error(
            "bc7_required: this cooked package requires native BC7 texture "
            "support");
    }
    runtime_->sceneLoadContext.requireDerivedTextures =
        config_.assetImportMode == AssetImportMode::CookedOnly;
    SceneRuntimeCallbacks runtimeCallbacks;
    runtimeCallbacks.publicationBlockReason =
        [this]() -> std::optional<std::string> {
        return hasUnsavedSceneChanges()
                   ? std::optional<std::string>(
                         "Scene changed while another world was loading")
                   : std::nullopt;
    };
    runtimeCallbacks.worldPublished =
        [this](const SceneRuntimePublication &publication) {
            frame_->shadowSystem.reset();
#if VKL_ENABLE_EDITOR_UI
            if (tooling_->editor)
                tooling_->editor->onWorldPublished(publication);
#endif
            if (publication.document &&
                publication.document->document.environment) {
                const SceneEnvironmentDocument &environment =
                    *publication.document->document.environment;
                RenderSettingsPatch patch;
                patch.environmentIntensity = environment.intensity;
                patch.environmentRotationRadians = environment.rotationRadians;
                applyRenderSettings(patch);
            }
        };
    runtimeCallbacks.environmentPublished =
        [this](const EnvironmentAssetKey &key) {
            runtime_->sceneWorkflow->recordEnvironmentUse(key.environmentId,
                                                          key.profileId);
        };
    runtimeCallbacks.loadFinalized =
        [this](const std::shared_ptr<SceneLoadTask> &task, bool success) {
            if (!success || !task || task->sceneIndex < 0 ||
                task->sceneIndex >=
                    static_cast<int>(runtime_->sceneRegistry.size())) {
                return;
            }
            const SceneEntry &entry = runtime_->sceneRegistry[task->sceneIndex];
            if (!entry.isModelPreview())
                return;
            const std::string &profileId = task->profileId;
            runtime_->sceneWorkflow->recordModelUse(entry.id, profileId);
        };
    runtime_->scene = std::make_unique<SceneRuntimeCoordinator>(
        *platform_->device, *platform_->descriptorAllocator,
        *platform_->materialSystem, *platform_->renderer, *platform_->frameSync,
        frame_->camera, projectContext_, runtime_->catalog,
        runtime_->sceneRegistry, runtime_->sceneLoadContext,
        std::move(runtimeCallbacks));

    SceneWorkflowCallbacks workflowCallbacks;
    workflowCallbacks.requestSceneLoad = [this](int index, bool sourceFallback,
                                                bool reloadAsset) {
        return requestSceneLoad(index, sourceFallback, reloadAsset);
    };
    workflowCallbacks.hasUnsavedChanges = [this] {
        return hasUnsavedSceneChanges();
    };
    workflowCallbacks.invalidateModel =
        [this](const ModelAssetId &modelId,
               const std::optional<std::string> &profileId) {
            runtime_->scene->invalidateModel(modelId,
                                             profileId ? &*profileId : nullptr);
        };
    workflowCallbacks.invalidateEnvironment =
        [this](const std::string &environmentId,
               const std::optional<std::string> &profileId) {
            runtime_->scene->invalidateEnvironment(
                environmentId, profileId ? &*profileId : nullptr);
        };
    workflowCallbacks.catalogRefreshed = [this] {
        runtime_->scene->remapCurrentSceneIndex();
    };
    workflowCallbacks.environmentArtifactsReady =
        [this](const std::string &environmentId) {
            if (runtime_->scene->selectedEnvironmentId() != environmentId)
                return;
            try {
                setEnvironment(environmentId);
                runtime_->sceneWorkflow->clearEnvironmentUiError();
            } catch (const std::exception &error) {
                runtime_->sceneWorkflow->setEnvironmentUiError(error.what());
            }
        };
    workflowCallbacks.environmentWillBeRemoved =
        [this](const std::string &environmentId) {
            if (runtime_->scene->selectedEnvironmentId() == environmentId)
                setEnvironment("None");
        };
    workflowCallbacks.textureLimitChanged = [this](uint32_t value) {
        runtime_->sceneLoadContext.maxTextureSize = value;
        config_.gltfMaxTextureSize = value;
    };
    runtime_->sceneWorkflow->initialize(
        SceneWorkflowConfig{
            config_.assetImportMode,
            std::filesystem::u8path(config_.derivedTextureCachePath),
            std::filesystem::u8path(config_.assetToolPath),
            std::filesystem::u8path(config_.gltfValidatorPath),
            config_.assetImportWorkers, config_.assetImportMemoryBudgetMiB,
            build::kAssetAuthoring},
        std::move(workflowCallbacks));
    runtime_->sceneWorkflow->setTextureLimit(
        runtime_->sceneLoadContext.maxTextureSize);
    if (runtime_->catalog.defaultEnvironment &&
        runtime_->catalog.findEnvironment(
            *runtime_->catalog.defaultEnvironment)) {
        try {
            setEnvironment(*runtime_->catalog.defaultEnvironment);
        } catch (const std::exception &error) {
            VKR_LOG_WARN("Environment",
                         "Could not queue default environment '{}': {}",
                         *runtime_->catalog.defaultEnvironment, error.what());
        }
    }
    runtime_->sceneWorkflow->selectEntry(start);
    requestSceneOperation(start);
}

void Application::initOptionalTooling() {
#if VKL_ENABLE_CAPTURE
    if (!projectContext_.cookedPackage) {
        tooling_->capture = std::make_unique<CaptureService>(
            *platform_->device, projectContext_.captureRoot);
    }
#endif

#if VKL_ENABLE_EDITOR_UI
    if (config_.diagnostics.guiVisible) {
        tooling_->gui = std::make_unique<GuiSystem>(
            platform_->context->instance(), *platform_->device,
            platform_->swapChain->imageFormat(), platform_->window->handle(),
            platform_->swapChain->imageCount(),
            platform_->swapChain->imageCount());
        EditorControllerActions editorActions;
        editorActions.requestSceneOperation =
            [this](int index, bool sourceFallback, bool loadAfter,
                   SceneWorkflowRequestReason reason, bool forceReimport,
                   bool reloadAsset) {
                return requestSceneOperation(index, sourceFallback, loadAfter,
                                             reason, forceReimport,
                                             reloadAsset);
            };
        editorActions.setTextureLimit = [this](uint32_t limit) {
            return setTextureLimit(limit);
        };
        editorActions.setEnvironment = [this](const std::string &id) {
            return setEnvironment(id);
        };
        tooling_->editor =
            std::make_unique<EditorController>(EditorControllerServices{
                config_,
                projectContext_,
                runtime_->catalog,
                runtime_->sceneRegistry,
                runtime_->sceneLoadContext,
                *platform_->window,
                *platform_->device,
                *platform_->frameSync,
                *platform_->swapChain,
                *platform_->renderer,
                *tooling_->gui,
                *platform_->materialSystem,
                *runtime_->sceneWorkflow,
                *runtime_->scene,
                *runtime_->renderSettings,
                tooling_->capture.get(),
                frame_->camera,
                frame_->ambientColor,
                frame_->ambientIntensity,
                frame_->defaultSunDirection,
                frame_->defaultSunColor,
                frame_->defaultSunIntensity,
                frame_->visibilityFrame,
                frame_->shadowSystem,
                frame_->lastLightStats,
                [this]() { return frame_->inputMode == InputMode::CameraDrag; },
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
        controlActions.setTextureLimit = [this](uint32_t limit) {
            return setTextureLimit(limit);
        };
        controlActions.setEnvironment = [this](const std::string &id) {
            return setEnvironment(id);
        };
        controlActions.reloadEnvironment = [this]() {
            return reloadCurrentEnvironment();
        };
        controlActions.cancelLoadOperation = [this](uint64_t taskId) {
            return cancelLoadOperation(taskId);
        };
        controlActions.hasUnsavedChanges = [this]() {
            return hasUnsavedSceneChanges();
        };

        std::function<RuntimeViewportSnapshot()> viewportSnapshot;
#if VKL_ENABLE_EDITOR_UI
        if (tooling_->editor) {
            viewportSnapshot = [this]() {
                const EditorViewportDiagnostics source =
                    tooling_->editor->viewportDiagnostics();
                return RuntimeViewportSnapshot{
                    source.displayWidth, source.displayHeight, source.visible,
                    source.hovered, source.resizePending};
            };
        }
#endif
        bool editorAvailable = false;
#if VKL_ENABLE_EDITOR_UI
        editorAvailable = tooling_->gui != nullptr;
#endif
        tooling_->runtimeControl = std::make_unique<RuntimeControlAdapter>(
            RuntimeControlServices{config_,
                                   projectContext_,
                                   runtime_->catalog,
                                   runtime_->sceneRegistry,
                                   runtime_->sceneLoadContext,
                                   *platform_->window,
                                   *platform_->context,
                                   *platform_->device,
                                   *platform_->frameSync,
                                   *platform_->swapChain,
                                   *platform_->renderer,
                                   *platform_->materialSystem,
                                   *runtime_->sceneWorkflow,
                                   *runtime_->scene,
                                   *runtime_->renderSettings,
                                   tooling_->capture.get(),
                                   frame_->camera,
                                   frame_->visibilityFrame,
                                   frame_->lastLightStats,
                                   frame_->presentedFrameCount,
                                   editorAvailable,
                                   std::move(viewportSnapshot),
                                   std::move(controlActions)});
    }
#endif
}

uint64_t Application::reloadCurrentScene() {
    const int index = runtime_->scene->currentSceneIndex();
    if (index < 0 || index >= static_cast<int>(runtime_->sceneRegistry.size()))
        return 0;
    const uint64_t taskId = requestSceneOperation(
        index, false, true, SceneWorkflowRequestReason::SceneLoad, false, true);
    VKR_LOG_INFO(
        "Scene", "Requested reload of {} with glTF texture limit {}",
        runtime_->sceneRegistry[index].name,
        runtime_->sceneLoadContext.maxTextureSize == 0
            ? std::string("Full")
            : std::to_string(runtime_->sceneLoadContext.maxTextureSize));
    return taskId;
}

uint64_t Application::setTextureLimit(uint32_t limit) {
    if (limit != 0 && limit != 512 && limit != 1024 && limit != 2048)
        throw SceneWorkflowError(
            "invalid_texture_limit",
            "Texture limit must be 0, 512, 1024, or 2048.");
    if (runtime_->sceneLoadContext.maxTextureSize == limit)
        return 0;
    if (config_.assetImportMode == AssetImportMode::CookedOnly) {
        throw SceneWorkflowError(
            "texture_limit_locked",
            "Texture limit is fixed by the cooked package profile.");
    }
    runtime_->sceneLoadContext.maxTextureSize = limit;
    runtime_->sceneWorkflow->setTextureLimit(limit);
    VKR_LOG_INFO("Renderer", "glTF texture limit set to {}",
                 textureLimitLabel(limit));
    const int currentIndex = runtime_->scene->currentSceneIndex();
    if (currentIndex >= 0 &&
        currentIndex < static_cast<int>(runtime_->sceneRegistry.size()) &&
        runtime_->sceneRegistry[currentIndex].isNativeScene())
        return 0;
    const auto &latest = runtime_->scene->latestSceneLoadTask();
    return latest && !isTerminalSceneLoadState(latest->state.load())
               ? requestSceneOperation(latest->sceneIndex)
               : reloadCurrentScene();
}

uint64_t Application::requestSceneLoad(int index, bool sourceFallback,
                                       bool reloadAsset) {
    return runtime_->scene->requestSceneLoad(index, sourceFallback,
                                             reloadAsset);
}

bool Application::cancelSceneLoad(uint64_t taskId) {
    return runtime_->scene && runtime_->scene->cancelSceneLoad(taskId);
}

bool Application::cancelEnvironmentLoad(uint64_t taskId) {
    return runtime_->scene && runtime_->scene->cancelEnvironmentLoad(taskId);
}

uint64_t Application::setEnvironment(const std::string &id) {
    if (asciiEqualsIgnoreCase(id, "None") || id.empty()) {
        runtime_->scene->clearEnvironment();
        return 0;
    }
    if (!platform_->device->environmentIblSupported()) {
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

uint64_t
Application::queueEnvironmentLoad(const CatalogEnvironment &environment,
                                  bool reload) {
    return runtime_->scene->queueEnvironment(environment, reload);
}

uint64_t Application::reloadCurrentEnvironment() {
    return runtime_->scene->reloadEnvironment();
}

void Application::applyRenderSettings(const RenderSettingsPatch &patch) {
    runtime_->renderSettings->apply(patch);
}

const RenderSettings &Application::renderSettings() const {
    return runtime_->renderSettings->settings();
}

const CatalogEnvironment *
Application::findEnvironmentByName(const std::string &name) const {
    for (const CatalogEnvironment &environment :
         runtime_->catalog.environments) {
        if (asciiEqualsIgnoreCase(environment.id, name) ||
            asciiEqualsIgnoreCase(environment.displayName, name)) {
            return &environment;
        }
    }
    return nullptr;
}

uint64_t Application::requestSceneOperation(int index, bool sourceFallback,
                                            bool loadAfter,
                                            SceneWorkflowRequestReason reason,
                                            bool forceReimport,
                                            bool reloadAsset) {
    return runtime_->sceneWorkflow->requestEntry(
        index, sourceFallback, loadAfter, reason, forceReimport, reloadAsset);
}

bool Application::hasUnsavedSceneChanges() const {
#if VKL_ENABLE_EDITOR_UI
    return tooling_->editor && tooling_->editor->hasUnsavedChanges();
#else
    return false;
#endif
}

bool Application::cancelLoadOperation(uint64_t taskId) {
    if ((taskId & EnvironmentLoadManager::kTaskIdMask) != 0 &&
        !runtime_->sceneWorkflow->isAssetTaskId(taskId)) {
        return cancelEnvironmentLoad(taskId);
    }
    if (!runtime_->sceneWorkflow->isAssetTaskId(taskId))
        return cancelSceneLoad(taskId);
    const uint64_t linked =
        runtime_->sceneWorkflow->linkedSceneLoadTask(taskId);
    if (linked != 0)
        return cancelSceneLoad(linked);
    return runtime_->sceneWorkflow->cancelAssetTask(taskId);
}

void Application::updateInputMode() {
#if VKL_ENABLE_EDITOR_UI
    if (tooling_->editor && tooling_->editor->activeSceneCamera()) {
        if (frame_->inputMode == InputMode::CameraDrag) {
            platform_->input->setCursorCaptured(false);
            platform_->input->setCursorPos(frame_->savedCursor);
            tooling_->editor->setCameraDragActive(false);
            frame_->inputMode = InputMode::UI;
        }
        return;
    }
#endif

    if (frame_->inputMode == InputMode::UI) {
        const bool pressed =
            platform_->input->isMousePressed(MouseButton::Right);
        bool overUI = false;
        bool overSceneArea = true;
#if VKL_ENABLE_EDITOR_UI
        overUI = tooling_->gui && tooling_->gui->wantCaptureMouse();
        overSceneArea = !tooling_->editor || tooling_->editor->viewportHovered();
        if (overSceneArea)
            overUI = false;
        overUI =
            overUI || (tooling_->editor && tooling_->editor->anyItemActive());
        overUI = overUI ||
                 (tooling_->editor && tooling_->editor->blocksViewportInput());
#endif
        if (pressed && overSceneArea && !overUI) {
            frame_->savedCursor = platform_->input->cursorPos();
            platform_->input->setCursorCaptured(true);
#if VKL_ENABLE_EDITOR_UI
            if (tooling_->editor)
                tooling_->editor->setCameraDragActive(true);
#endif
            frame_->inputMode = InputMode::CameraDrag;
        }
    } else { // CameraDrag
        if (platform_->input->isMouseReleased(MouseButton::Right)) {
            platform_->input->setCursorCaptured(false);
            platform_->input->setCursorPos(frame_->savedCursor);
#if VKL_ENABLE_EDITOR_UI
            if (tooling_->editor)
                tooling_->editor->setCameraDragActive(false);
#endif
            frame_->inputMode = InputMode::UI;
        }
    }
}

void Application::processCameraInput(float dt) {
    glm::vec3 move{0.0f};
    if (platform_->input->isKeyDown(Key::W))
        move.z += config_.moveSpeed * dt;
    if (platform_->input->isKeyDown(Key::S))
        move.z -= config_.moveSpeed * dt;
    if (platform_->input->isKeyDown(Key::A))
        move.x -= config_.moveSpeed * dt;
    if (platform_->input->isKeyDown(Key::D))
        move.x += config_.moveSpeed * dt;
    if (platform_->input->isKeyDown(Key::Q))
        move.y -= config_.moveSpeed * dt;
    if (platform_->input->isKeyDown(Key::E))
        move.y += config_.moveSpeed * dt;
    frame_->camera.translate(move);

    const auto d = platform_->input->mouseDelta();
    frame_->camera.rotate(-d.x * config_.mouseSensitivity,
                          -d.y * config_.mouseSensitivity);
}

void Application::handleSwapChainRecreate() {
    VKL_PROFILE_ZONE("Swapchain Recreate");
    const VkExtent2D framebufferExtent = platform_->window->framebufferExtent();
    if (framebufferExtent.width == 0 || framebufferExtent.height == 0)
        return;

    platform_->renderer->recreateSwapChain();
    platform_->frameSync->markAllSubmissionsCompleted();
    if (tooling_->capture) {
        tooling_->capture->update(
            platform_->frameSync->completedSubmissionSerial());
        tooling_->capture->onSwapChainRecreated(
            platform_->frameSync->completedSubmissionSerial());
    }
    platform_->pipelineCache->clear();
    platform_->frameSync->onSwapChainRecreated();
    bool editorViewportActive = false;
#if VKL_ENABLE_EDITOR_UI
    editorViewportActive = tooling_->gui != nullptr;
    if (tooling_->gui)
        tooling_->gui->onSwapChainRecreated(platform_->swapChain->imageCount());
#endif
    if (!editorViewportActive) {
        platform_->renderer->resizeViewport(platform_->swapChain->extent());
        frame_->camera.setAspect(
            static_cast<float>(platform_->swapChain->extent().width) /
            static_cast<float>(platform_->swapChain->extent().height));
    }
}

const ShaderVariant &Application::currentShaderVariant() const {
    return runtime_->renderSettings->currentShaderVariant();
}

void Application::mainLoop() {
    auto startTime = std::chrono::high_resolution_clock::now();
    auto lastTime = startTime;
    auto lastProfilerMemorySample =
        std::chrono::steady_clock::now() - std::chrono::seconds(1);
    AllocatorMemorySnapshot profilerMemory{};
    profileConfigureMemoryPlot("SceneLoad/StagingBytes");
    profileConfigureMemoryPlot("SceneLoad/ProcessedBytes");
    profileConfigureMemoryPlot("VMA/AllocationBytes");
    profileConfigureMemoryPlot("VMA/BlockBytes");
    float simulationTime = 0.0f;

    while (true) {
        VKL_PROFILE_ZONE("Application Frame");
        platform_->window->pollEvents();
        platform_->input->update();

#if VKL_ENABLE_EDITOR_UI
        if (platform_->window->shouldClose() && tooling_->editor &&
            tooling_->editor->interceptCloseRequest()) {
            platform_->window->setShouldClose(false);
        }
#endif
        if (platform_->window->shouldClose())
            break;

        if (tooling_->capture) {
            tooling_->capture->update(
                platform_->frameSync->completedSubmissionSerial());
        }

        runtime_->sceneWorkflow->pump();
#if VKL_ENABLE_RUNTIME_CONTROL
        if (tooling_->runtimeControl &&
            tooling_->runtimeControl->processOne()) {
            platform_->window->setShouldClose(true);
            break;
        }
#endif

        // 1. Pump scene work outside the frame command buffer.
        runtime_->scene->pump();
#if VKL_ENABLE_EDITOR_UI
        if (tooling_->editor)
            tooling_->editor->update();
#endif

        // 2. Advance frame time.
        const auto now = std::chrono::high_resolution_clock::now();
        const float dt =
            config_.diagnostics.fixedDeltaSeconds
                ? *config_.diagnostics.fixedDeltaSeconds
                : std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

#if VKL_ENABLE_EDITOR_UI
        if (tooling_->editor)
            tooling_->editor->applyPendingViewportResize();
#endif

        // 3. Begin the editor frame.
#if VKL_ENABLE_EDITOR_UI
        if (tooling_->gui)
            tooling_->gui->beginFrame();
        if (tooling_->editor)
            tooling_->editor->beginFrame();
#endif

        // 4. Update input mode and camera controls.
        if (!config_.diagnostics.automationMode) {
            updateInputMode();
            if (frame_->inputMode == InputMode::CameraDrag)
                processCameraInput(dt);
        }
        if (platform_->input->isKeyPressed(Key::Escape)) {
#if VKL_ENABLE_EDITOR_UI
            if (!tooling_->editor || !tooling_->editor->handleEscape())
                platform_->window->setShouldClose(true);
#else
            platform_->window->setShouldClose(true);
#endif
        }
#if VKL_ENABLE_CAPTURE
        if (platform_->input->isKeyPressed(Key::F12)) {
#if VKL_ENABLE_EDITOR_UI
            if (tooling_->editor)
                tooling_->editor->requestManualCapture();
#endif
        }
#endif

        // 5. Tick the active world.
        simulationTime =
            config_.diagnostics.fixedDeltaSeconds
                ? simulationTime + dt
                : std::chrono::duration<float>(now - startTime).count();
        if (runtime_->scene->currentWorld())
            runtime_->scene->currentWorld()->update(dt, simulationTime);

        // 6. Build editor UI.
#if VKL_ENABLE_EDITOR_UI
        if (tooling_->editor)
            tooling_->editor->draw();
#endif

        // 7. Render the frame.
        std::optional<FrameSync::FrameContext> ctx;
        {
            VKL_PROFILE_ZONE("Begin Render Frame");
            ctx = platform_->frameSync->beginFrame();
        }
        runtime_->scene->collectRetired();
        if (!ctx) {
            if (platform_->frameSync->swapChainNeedsRecreation())
                handleSwapChainRecreate();
#if VKL_ENABLE_EDITOR_UI
            if (tooling_->gui)
                tooling_->gui->discardFrame();
#endif
            platform_->input->endFrame();
            const VkExtent2D framebufferExtent =
                platform_->window->framebufferExtent();
            if (framebufferExtent.width == 0 || framebufferExtent.height == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            continue;
        }

        std::optional<CaptureFrameSelection> captureSelection;
        if (tooling_->capture) {
            tooling_->capture->update(
                platform_->frameSync->completedSubmissionSerial());
            const RendererViewportOutput viewportOutput =
                platform_->renderer->viewportOutput();
            CaptureImageSource viewportSource{};
            viewportSource.kind = CaptureSourceKind::Viewport;
            viewportSource.image = viewportOutput.images[ctx->frameIndex];
            viewportSource.extent = viewportOutput.extent;
            viewportSource.format = viewportOutput.format;
            viewportSource.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            viewportSource.sourceStage =
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            viewportSource.sourceAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                          VK_ACCESS_SHADER_READ_BIT;
            viewportSource.restoreStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            viewportSource.restoreAccess = VK_ACCESS_SHADER_READ_BIT;
            viewportSource.supported =
                describeCaptureFormat(viewportSource.format).supported;
            if (!viewportSource.supported) {
                viewportSource.unsupportedReason =
                    "viewport format is not supported for PNG capture";
            }

            CaptureImageSource workspaceSource{};
            workspaceSource.kind = CaptureSourceKind::Workspace;
            workspaceSource.image =
                platform_->swapChain->image(ctx->imageIndex);
            workspaceSource.extent = platform_->swapChain->extent();
            workspaceSource.format = platform_->swapChain->imageFormat();
            workspaceSource.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            workspaceSource.sourceStage =
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            workspaceSource.sourceAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            workspaceSource.restoreStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            workspaceSource.restoreAccess = 0;
            workspaceSource.supported =
                platform_->swapChain->captureSupported();
            workspaceSource.unsupportedReason =
                platform_->swapChain->captureUnsupportedReason();

            const RendererHdrOutput hdrOutput =
                platform_->renderer->hdrOutput();
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
            hdrSource.sourceAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                     VK_ACCESS_SHADER_WRITE_BIT |
                                     VK_ACCESS_SHADER_READ_BIT;
            hdrSource.restoreStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            hdrSource.restoreAccess = VK_ACCESS_SHADER_READ_BIT;
            const CaptureFormatDescription hdrFormat =
                describeCaptureFormat(hdrSource.format);
            hdrSource.supported =
                hdrFormat.supported &&
                hdrFormat.encoding != CapturePixelEncoding::Unorm8;
            if (!hdrSource.supported)
                hdrSource.unsupportedReason =
                    "renderer HDR format is not supported for HDR capture";
            captureSelection = tooling_->capture->prepareFrame(
                viewportSource, workspaceSource, hdrSource);
        }

        RenderWorldFrameSnapshot worldSnapshot{};
        const bool hasWorldSnapshot =
            runtime_->scene->currentWorld() != nullptr;
        if (runtime_->scene->currentWorld()) {
            VKL_PROFILE_ZONE("RenderWorld Snapshot");
            worldSnapshot =
                runtime_->scene->currentWorld()->buildRenderSnapshot();
        }

        RenderViewInput viewInput{};
        viewInput.view = frame_->camera.viewMatrix();
        viewInput.projection = frame_->camera.projectionMatrix();
        viewInput.cameraPosition = frame_->camera.position();
        viewInput.cameraNearPlane = frame_->camera.nearPlane();
        viewInput.cameraFarPlane = frame_->camera.farPlane();
        viewInput.viewportExtent = platform_->renderer->viewportExtent();
        viewInput.atmosphereSupported =
            platform_->device->atmosphereSupport().available;
        viewInput.ddgiSupported = platform_->device->ddgiSupport().available;
        viewInput.ambientColor = frame_->ambientColor;
        viewInput.ambientIntensity = frame_->ambientIntensity;
        viewInput.defaultSun = {frame_->defaultSunDirection,
                                frame_->defaultSunColor,
                                frame_->defaultSunIntensity};
        viewInput.settings = runtime_->renderSettings->snapshot().active;
        viewInput.environmentReady = platform_->renderer->environmentReady();
        viewInput.maxSpecularLod =
            platform_->renderer->currentEnvironmentMaxSpecularLod();
        std::string cameraHistoryIdentity = "editor";
        if (runtime_->scene->currentWorld()) {
            viewInput.sceneBounds = worldSnapshot.bounds;
            viewInput.sceneLights = &worldSnapshot.lights;
            viewInput.reflectionProbes = &worldSnapshot.reflectionProbes;
            viewInput.fallbackSunEnabled = worldSnapshot.fallbackSunEnabled;
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
            bool useActiveSceneCamera = true;
#if VKL_ENABLE_EDITOR_UI
            useActiveSceneCamera = !tooling_->gui ||
                                   (tooling_->editor &&
                                    tooling_->editor->activeSceneCamera());
#endif
            if (useActiveSceneCamera) {
                const VkExtent2D extent = platform_->renderer->viewportExtent();
                const float aspect =
                    extent.height == 0 ? 1.0f
                                       : static_cast<float>(extent.width) /
                                             static_cast<float>(extent.height);
                if (const auto activeCamera =
                        runtime_->scene->currentWorld()->activeCamera(aspect)) {
                    viewInput.view = activeCamera->view;
                    viewInput.projection = activeCamera->projection;
                    viewInput.cameraPosition = activeCamera->position;
                    viewInput.cameraNearPlane = activeCamera->nearPlane;
                    viewInput.cameraFarPlane = activeCamera->farPlane;
                    cameraHistoryIdentity =
                        activeCamera->entityId.empty()
                            ? "active-camera"
                            : "active:" + activeCamera->entityId.toString();
                }
            }
        }
#if VKL_ENABLE_EDITOR_UI
        if (tooling_->editor)
            tooling_->editor->applyReflectionProbeCaptureView(
                viewInput, cameraHistoryIdentity);
#endif
        const bool taaJitterEnabled =
            platform_->device->screenSpaceEffectsSupport().taaAvailable &&
            taaPassRequested(viewInput.settings);
        const TemporalJitter jitter =
            temporalJitter(frame_->presentedFrameCount,
                           viewInput.viewportExtent, taaJitterEnabled);
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
        shadowInput.fallbackSunDirection = frame_->defaultSunDirection;
        shadowInput.fallbackSunColor = frame_->defaultSunColor;
        shadowInput.fallbackSunIntensity = frame_->defaultSunIntensity;
        shadowInput.fallbackSunEnabled =
            hasWorldSnapshot && viewInput.fallbackSunEnabled;
        shadowInput.settings = viewInput.settings;
#if VKL_ENABLE_EDITOR_UI
        if (tooling_->editor)
            shadowInput.focusedLightEntity =
                tooling_->editor->focusedLightEntity();
#endif
        const ShadowFramePlan shadowPlan =
            frame_->shadowSystem.build(shadowInput);
        const RenderView renderView = buildRenderView(viewInput, shadowPlan);
        if (renderView.lightStats.ignoredLights !=
            frame_->lastLightStats.ignoredLights) {
            if (renderView.lightStats.ignoredLights > 0) {
                VKR_LOG_WARN(
                    "Lighting",
                    "Ignored {} scene lights beyond the shared limit of {}.",
                    renderView.lightStats.ignoredLights, kMaxSceneLights);
            }
        }
        frame_->lastLightStats = renderView.lightStats;
        {
            VKL_PROFILE_ZONE("RenderItem Collect");
            frame_->renderItems = std::move(worldSnapshot.renderItems);
        }
        {
            VKL_PROFILE_ZONE("Visibility Build");
            VisibilityBuildInput visibilityInput{};
            visibilityInput.sceneGeneration =
                runtime_->scene->sceneGeneration();
            visibilityInput.cameraIdentity = std::move(cameraHistoryIdentity);
            visibilityInput.shaderIdentity = currentShaderVariant().id;
            visibilityInput.sceneBounds = viewInput.sceneBounds;
            frame_->visibilityFrame = frame_->visibilitySystem.build(
                std::move(frame_->renderItems), renderView,
                platform_->renderer->viewportExtent(),
                std::move(visibilityInput));
        }

        if constexpr (build::kTracy) {
            profilePlotNumber("Frame/DrawCount",
                              static_cast<int64_t>(
                                  frame_->visibilityFrame.cameraDrawCount()));
            profilePlotNumber("Frame/OpaqueDrawCount",
                              static_cast<int64_t>(
                                  frame_->visibilityFrame.cameraOpaque.size()));
            profilePlotNumber(
                "Frame/TransparentDrawCount",
                static_cast<int64_t>(
                    frame_->visibilityFrame.cameraTransparent.size()));
            profilePlotNumber(
                "Visibility/Source",
                static_cast<int64_t>(
                    frame_->visibilityFrame.cpuStats.sourceDraws));
            profilePlotNumber(
                "Visibility/CameraVisible",
                static_cast<int64_t>(
                    frame_->visibilityFrame.cpuStats.cameraVisible));
            profilePlotNumber(
                "Visibility/FrustumCulled",
                static_cast<int64_t>(
                    frame_->visibilityFrame.cpuStats.frustumCulled));
            profilePlotNumber(
                "Visibility/DistanceCulled",
                static_cast<int64_t>(
                    frame_->visibilityFrame.cpuStats.distanceCulled));
            profilePlotNumber(
                "Visibility/SmallObjectCulled",
                static_cast<int64_t>(
                    frame_->visibilityFrame.cpuStats.smallObjectCulled));
            profilePlotNumber(
                "Visibility/ShadowVisible",
                static_cast<int64_t>(
                    frame_->visibilityFrame.cpuStats.shadowVisible));
            profilePlotNumber(
                "Frame/GraphicsPipelines",
                static_cast<int64_t>(
                    platform_->pipelineCache->graphicsPipelineCount()));
            profilePlotNumber(
                "Lighting/Active",
                static_cast<int64_t>(frame_->lastLightStats.effectiveLights));
            profilePlotNumber(
                "Lighting/Uploaded",
                static_cast<int64_t>(frame_->lastLightStats.totalLights));
            profilePlotNumber(
                "Lighting/Ignored",
                static_cast<int64_t>(frame_->lastLightStats.ignoredLights));
            profilePlotNumber(
                "Frame/ComputePipelines",
                static_cast<int64_t>(
                    platform_->pipelineCache->computePipelineCount()));
            profilePlotMemory(
                "SceneLoad/StagingBytes",
                static_cast<int64_t>(runtime_->scene->stagingBytesInUse()));
            if (runtime_->scene->latestSceneLoadTask()) {
                profilePlotMemory(
                    "SceneLoad/ProcessedBytes",
                    static_cast<int64_t>(runtime_->scene->latestSceneLoadTask()
                                             ->progress.processedBytes.load()));
            }
            const auto profilerNow = std::chrono::steady_clock::now();
            if (profileConnected() && profilerNow - lastProfilerMemorySample >=
                                          std::chrono::seconds(1)) {
                profilerMemory = platform_->device->allocatorMemorySnapshot();
                lastProfilerMemorySample = profilerNow;
            }
            profilePlotMemory(
                "VMA/AllocationBytes",
                static_cast<int64_t>(profilerMemory.allocationBytes));
            profilePlotMemory("VMA/BlockBytes",
                              static_cast<int64_t>(profilerMemory.blockBytes));
        }

        std::function<void(VkCommandBuffer)> drawUi;
#if VKL_ENABLE_EDITOR_UI
        if (tooling_->gui) {
            drawUi = [gui = tooling_->gui.get()](VkCommandBuffer cmd) {
                gui->render(cmd);
            };
        }
#endif
        {
            ScopedGpuLabel frameLabel(
                platform_->device->debugUtils(), ctx->cmd,
                "Frame " + std::to_string(frame_->presentedFrameCount + 1));
            platform_->renderer->renderFrame(
                *ctx, frame_->visibilityFrame, *platform_->pipelineCache,
                std::move(drawUi), currentShaderVariant(), renderView,
                captureSelection
                    ? std::optional<
                          FrameCaptureSource>{captureSelection->source ==
                                                      CaptureSourceKind::
                                                          Workspace
                                                  ? FrameCaptureSource::
                                                        Workspace
                                              : captureSelection->source ==
                                                      CaptureSourceKind::Hdr
                                                  ? FrameCaptureSource::Hdr
                                                  : FrameCaptureSource::
                                                        Viewport}
                    : std::nullopt,
                captureSelection
                    ? std::function<void(
                          VkCommandBuffer)>{[this](VkCommandBuffer cmd) {
                          tooling_->capture->recordCopy(cmd);
                      }}
                    : std::function<void(VkCommandBuffer)>{});
            runtime_->renderSettings->updateRuntimeState(
                platform_->renderer->featureRuntimeState());
            if constexpr (build::kTracy) {
                profilePlotNumber(
                    "Visibility/GpuOccluded",
                    static_cast<int64_t>(
                        platform_->renderer->occlusionCullingStatus()
                            .completed.occluded));
            }
        }
        platform_->device->tracyProfiler().collect(ctx->cmd);
        const uint64_t submissionSerial = platform_->frameSync->endFrame(*ctx);
        frame_->visibilitySystem.commit(frame_->visibilityFrame);
        ++frame_->presentedFrameCount;
        profileFrameMark();
        if (captureSelection)
            tooling_->capture->frameSubmitted(submissionSerial);

        if (platform_->frameSync->swapChainNeedsRecreation())
            handleSwapChainRecreate();

        // 8. Finish per-frame input state.
        platform_->input->endFrame();
    }
}

} // namespace vkr
