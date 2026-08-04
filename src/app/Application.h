#pragma once

#include "Config.h"

#include "assets/AssetImportManager.h"
#include "assets/ArtifactIndex.h"
#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"
#include "control/RuntimeCommandDispatcher.h"
#include "diagnostics/SceneLoadStats.h"
#include "render/RenderQueue.h"
#include "render/Visibility.h"
#include "render/RenderSettings.h"
#include "render/RenderView.h"
#include "render/ShaderRegistry.h"
#include "render/ShaderVariant.h"
#include "scene/Camera.h"
#include "scene/IRenderWorld.h"
#include "scene/Scene.h"
#include "scene/SceneFactory.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace vkr {

class Window;
class InputManager;
class VulkanContext;
class Device;
class DescriptorAllocator;
class SwapChain;
class FrameSync;
class Renderer;
class PipelineCache;
class GuiSystem;
#if VKL_ENABLE_EDITOR_UI
class EditorDockWorkspace;
class AssetsPanel;
class ScenesPanel;
class OutlinerPanel;
class InspectorPanel;
class SceneEditorSession;
class SceneViewportController;
#endif
class CaptureService;
struct RuntimeCommand;
class RuntimeCommandQueue;
class NamedPipeServerWin32;
class SceneLoadManager;
class AssetRepository;
struct SceneLoadTask;
struct ModelAsset;
class EnvironmentLoadManager;
class EnvironmentGpuBuilder;
struct EnvironmentLoadTask;
struct EnvironmentGpuResources;
class SceneWorkflowController;
struct ModelImportUiState;
struct SceneAssetOperationState;
struct EditorUiState;

enum class InputMode {
    UI,         // 光标可见，ImGui 接管
    CameraDrag, // 按住右键，相机接管鼠标
};

class Application final
#if VKL_ENABLE_RUNTIME_CONTROL
    : public RuntimeControlHost
#endif
{
  public:
    Application(const Config &config, ProjectContext projectContext,
                SceneCatalog catalog);
    ~Application();

    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    void run();

    /// 注册一个场景条目。必须在 `run()` 之前调用。
    void registerScene(SceneEntry entry);

  private:
    void init();
    void mainLoop();

    void updateInputMode();
    void processCameraInput(float dt);
    void drawGui();
    void handleSwapChainRecreate();
#if VKL_ENABLE_EDITOR_UI
    void bindViewportTextures();
    void applyPendingViewportResize();
#endif
    const ShaderVariant &currentShaderVariant() const;
    void applySceneCameraDefaults();
#if VKL_ENABLE_RUNTIME_CONTROL
    void processRuntimeCommand();
#endif
    void updateSceneLoading();
    void updateNativeSceneLoading(
        const std::shared_ptr<SceneLoadTask> &task);
    void resolveNativeSceneModels(
        const std::shared_ptr<SceneLoadTask> &task);
    void publishNativeScene(const std::shared_ptr<SceneLoadTask> &task);
    void updateEnvironmentLoading();
    void updateAssetImports();
    void drawScenePanel(bool modelsOnly = false);
    void drawOutlinerPanel();
    void drawInspectorPanel();
    void drawSceneAuthoringDialogs();
    void updateEditorModelBindings();
    void requestEditorSceneLoad(int index);
    void saveEditorScene();
    void executePendingEditorAction(bool saveFirst);
    void deleteSelectedEditorEntity();
    void duplicateSelectedEditorEntity();
    void handleEditorShortcuts();
    bool hasUnsavedSceneChanges() const;
    void drawSceneLoadingPanel();
    void drawAssetsPanel(bool environmentsOnly = false);
    void drawRenderPanel();
    void drawPostProcessingPanel();
    void drawCullingPanel();
    void drawLightingPanel();
    void drawCameraPanel();
    void drawMaterialsPanel();
    void drawPerformancePanel();
    void drawLoadStatsPanel();
    void drawCapturePanel();
    void requestManualCapture(bool includeGui);
    void updateModelImport();
    void refreshSceneRegistry(const std::string &selectSceneId = {});
    void reloadArtifactIndex();
    void persistArtifactIndex();

    void loadScene(int index, bool replaceCurrent = false);
    uint64_t reloadCurrentScene();
    void switchScene(int index);
    uint64_t requestSceneLoad(int index, bool sourceFallback = false,
                              bool reloadAsset = false);
    uint64_t requestSceneOperation(int index, bool sourceFallback = false,
                                   bool loadAfter = true,
                                   ImportReason reason = ImportReason::SceneLoad,
                                   bool forceReimport = false,
                                   bool reloadAsset = false);
    bool cancelSceneLoad(uint64_t taskId);
    bool cancelLoadOperation(uint64_t taskId);
    bool cancelEnvironmentLoad(uint64_t taskId);
    void finalizeSceneLoad(const std::shared_ptr<SceneLoadTask> &task,
                           bool success);
    uint64_t setTextureLimit(uint32_t limit);
    void setShaderVariant(const std::string &id);
    uint64_t setEnvironment(const std::string &id);
    uint64_t queueEnvironmentLoad(const CatalogEnvironment &environment,
                                  bool stagedForNativeScene);
    uint64_t reloadCurrentEnvironment();
    void applyRenderSettings(const RenderSettingsPatch &patch);
    int findSceneIndexByName(const std::string &name) const;
    const CatalogEnvironment *
    findEnvironmentByName(const std::string &name) const;
    std::string profileIdForTextureLimit(const SceneEntry &entry) const;
    void refreshArtifactStatus(int sceneIndex, bool admission = false);
    void refreshAllArtifactStatuses();
    void refreshValidationStatus(int sceneIndex);
    void refreshAllValidationStatuses();
    void retireCurrentScene();
    void collectRetiredScenes();
    void runModelAssetSharingSmoke(
        const std::shared_ptr<SceneLoadTask> &task,
        const std::shared_ptr<const ModelAsset> &asset);

#if VKL_ENABLE_RUNTIME_CONTROL
    ControlJson runtimeSystemInfo() override;
    ControlJson runtimeSceneList() override;
    ControlJson runtimeSceneCurrent() override;
    ControlJson runtimeSceneLoad(const std::string &name) override;
    ControlJson runtimeSceneReload() override;
    ControlJson runtimeLoadStatus(std::optional<uint64_t> taskId) override;
    ControlJson runtimeLoadCancel(std::optional<uint64_t> taskId) override;
    ControlJson runtimeTextureLimitGet() override;
    ControlJson runtimeTextureLimitSet(uint32_t value) override;
    ControlJson runtimeAssetCatalog() override;
    ControlJson
    runtimeAssetStatus(const std::optional<std::string> &name) override;
    ControlJson runtimeAssetValidation(const std::string &name) override;
    ControlJson runtimeAssetImport(const std::string &name, bool force,
                                   bool loadAfter) override;
    ControlJson runtimeAssetCancel(std::optional<uint64_t> taskId) override;
    ControlJson runtimeAssetCacheInfo() override;
    ControlJson runtimeShaderList() override;
    ControlJson runtimeShaderCurrent() override;
    ControlJson runtimeShaderSet(const std::string &name) override;
    ControlJson runtimeCameraGet() override;
    ControlJson runtimeCameraSet(const RuntimeCameraPose &pose) override;
    ControlJson runtimeWindowResize(uint32_t width,
                                    uint32_t height) override;
    ControlJson runtimeRenderStatus() override;
    ControlJson runtimeRenderSettingsGet() override;
    ControlJson
    runtimeRenderSettingsSet(const RenderSettingsPatch &patch) override;
    ControlJson runtimeEnvironmentList() override;
    ControlJson runtimeEnvironmentCurrent() override;
    ControlJson runtimeEnvironmentSet(const std::string &name) override;
    ControlJson runtimeEnvironmentReload() override;
    ControlJson runtimeCaptureScreenshot(const std::string &path,
                                         bool includeGui) override;
    ControlJson runtimeCaptureStatus(uint64_t taskId) override;
    ControlJson runtimeCaptureCancel(uint64_t taskId) override;
    ControlJson runtimeLastLoadStats() override;
    ControlJson runtimeQuit() override;

    ControlJson runtimeSceneOperationResult(int index, uint64_t taskId);
    int runtimeAssetSceneIndex(const std::string &name) const;
    ControlJson runtimeIndexedArtifactStatus(int index,
                                             const std::string &profileId,
                                             const ArtifactStatus &status) const;
#endif

    Config config_;
    ProjectContext projectContext_;
    std::unique_ptr<SceneWorkflowController> sceneWorkflow_;
    SceneCatalog &catalog_;
    std::vector<SceneEntry> &sceneRegistry_;

    // 基础设施（创建顺序 = 析构逆序）
    std::unique_ptr<Window>              window_;
    std::unique_ptr<InputManager>        input_;
    std::unique_ptr<VulkanContext>       context_;
    std::unique_ptr<Device>              device_;
    std::unique_ptr<DescriptorAllocator> descriptorAllocator_;
    std::unique_ptr<SwapChain>           swapChain_;
    std::unique_ptr<FrameSync>           frameSync_;
    std::unique_ptr<Renderer>            renderer_;
    std::unique_ptr<PipelineCache>       pipelineCache_;
    std::unique_ptr<GuiSystem>           gui_;
#if VKL_ENABLE_EDITOR_UI
    std::unique_ptr<EditorDockWorkspace> editorDockWorkspace_;
    std::unique_ptr<AssetsPanel> assetsPanel_;
    std::unique_ptr<ScenesPanel> scenesPanel_;
    std::unique_ptr<OutlinerPanel> outlinerPanel_;
    std::unique_ptr<InspectorPanel> inspectorPanel_;
    std::unique_ptr<SceneEditorSession> sceneEditorSession_;
    std::unique_ptr<SceneViewportController> sceneViewportController_;
    struct ViewportResizeState {
        uint32_t desiredWidth = 0;
        uint32_t desiredHeight = 0;
        std::chrono::steady_clock::time_point changedAt{};
        bool pending = false;
        bool immediate = false;
        bool measured = false;
    } viewportResize_;
    uint32_t viewportDisplayWidth_ = 0;
    uint32_t viewportDisplayHeight_ = 0;
    bool viewportVisible_ = false;
    bool viewportHovered_ = false;
#endif
    std::unique_ptr<CaptureService>      captureService_;
    RenderQueue                          renderQueue_;
    VisibilitySystem                    visibilitySystem_;
    VisibilityFrame                     visibilityFrame_;
    ShaderRegistry                       shaderRegistry_;
    std::string                          currentShaderVariantId_;

    // 场景切换
    SceneLoadContext        sceneLoadContext_;
    std::unique_ptr<AssetRepository> assetRepository_;
    std::shared_ptr<IRenderWorld> currentScene_;
    struct RetiredScene {
        uint64_t retireAfterSerial = 0;
        std::shared_ptr<IRenderWorld> scene;
    };
    std::deque<RetiredScene> retiredScenes_;
    int                     currentSceneIndex_ = -1;
    int                     pendingSceneIndex_ = -1;
    std::optional<SceneLoadStats> lastSceneLoadStats_;
    std::unique_ptr<SceneLoadManager> sceneLoadManager_;
    std::shared_ptr<SceneLoadTask> latestSceneLoadTask_;
    std::unique_ptr<EnvironmentLoadManager> environmentLoadManager_;
    std::unique_ptr<EnvironmentGpuBuilder> environmentGpuBuilder_;
    std::shared_ptr<EnvironmentLoadTask> latestEnvironmentLoadTask_;
    uint64_t stagedEnvironmentTaskId_ = 0;
    std::shared_ptr<EnvironmentGpuResources> stagedEnvironmentResources_;
    std::string selectedEnvironmentId_;
    uint64_t lastFinalizedTaskId_ = 0;
    std::unique_ptr<AssetImportManager> assetImportManager_;
    std::unique_ptr<ArtifactIndex> artifactIndex_;
    std::optional<ArtifactIndexUsage> artifactUsage_;
    SceneAssetOperationState *sceneAssetOperations_ = nullptr;

#if VKL_ENABLE_RUNTIME_CONTROL
    std::unique_ptr<RuntimeCommandQueue> runtimeCommandQueue_;
    std::unique_ptr<NamedPipeServerWin32> runtimeControlServer_;
    RuntimeCommandDispatcher runtimeCommandDispatcher_;
    std::shared_ptr<RuntimeCommand> pendingQuitCommand_;
    std::string runtimeControlPipeName_;
#endif
    ModelImportUiState *modelImportUi_ = nullptr;
    std::unique_ptr<EditorUiState> editorUi_;
    uint64_t lastCaptureTaskId_ = 0;
    bool captureIncludeGui_ = false;
    std::string captureUiError_;
    uint64_t sceneGeneration_ = 0;
    uint64_t presentedFrameCount_ = 0;
    bool modelAssetSharingSmokeComplete_ = false;

    // 输入模式
    InputMode  mode_ = InputMode::UI;
    glm::dvec2 savedCursor_{};

    Camera camera_;

    glm::vec3 ambientColor_{1.0f};
    float     ambientIntensity_ = 0.08f;
    glm::vec3 defaultSunDirection_{0.3f, 0.8f, 0.5f};
    glm::vec3 defaultSunColor_{1.0f};
    float     defaultSunIntensity_ = 3.0f;
    RenderViewLightStats lastLightStats_{};
    RenderSettings renderSettings_{};
};

} // namespace vkr
