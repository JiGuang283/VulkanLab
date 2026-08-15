#pragma once

#include "Config.h"

#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"
#include "workflows/SceneWorkflowTypes.h"
#include "control/RuntimeCommandDispatcher.h"
#include "render/RenderItem.h"
#include "render/Visibility.h"
#include "render/RenderSettings.h"
#include "render/RenderView.h"
#include "render/ShaderVariant.h"
#include "scene/Camera.h"
#include "scene/ModelPrepareFactory.h"
#include "scene/SceneEntry.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <chrono>
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace vkr {

class Window;
class InputManager;
class VulkanContext;
class Device;
class DescriptorAllocator;
class MaterialSystem;
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
class EnvironmentAssetHandle;
struct SceneLoadTask;
struct ModelAsset;
struct EnvironmentLoadTask;
class SceneRuntimeCoordinator;
class SceneWorkflowController;
class RenderSettingsController;
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
#if VKL_ENABLE_RUNTIME_CONTROL
    void processRuntimeCommand();
#endif
    void drawScenePanel(bool modelsOnly = false);
    void drawOutlinerPanel();
    void drawInspectorPanel();
    void drawSceneAuthoringDialogs();
    void updateEditorModelBindings();
    void updateEditorReflectionProbeBindings();
    void beginReflectionProbeCapture(PersistentEntityId entityId);
    void updateReflectionProbeCapture();
    void applyReflectionProbeCaptureView(RenderViewInput &input,
                                         std::string &cameraIdentity) const;
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
    void drawSurfaceDataPanel();
    void drawCullingPanel();
    void drawLightingPanel();
    void drawCameraPanel();
    void drawMaterialsPanel();
    void drawPerformancePanel();
    void drawLoadStatsPanel();
    void drawCapturePanel();
    void requestManualCapture(bool includeGui);
    void refreshSceneRegistry(const std::string &selectSceneId = {});

    uint64_t reloadCurrentScene();
    void switchScene(int index);
    uint64_t requestSceneLoad(int index, bool sourceFallback = false,
                              bool reloadAsset = false);
    uint64_t requestSceneOperation(int index, bool sourceFallback = false,
                                   bool loadAfter = true,
                                   SceneWorkflowRequestReason reason =
                                       SceneWorkflowRequestReason::SceneLoad,
                                   bool forceReimport = false,
                                   bool reloadAsset = false);
    bool cancelSceneLoad(uint64_t taskId);
    bool cancelLoadOperation(uint64_t taskId);
    bool cancelEnvironmentLoad(uint64_t taskId);
    uint64_t setTextureLimit(uint32_t limit);
    void setShaderVariant(const std::string &id);
    uint64_t setEnvironment(const std::string &id);
    uint64_t queueEnvironmentLoad(const CatalogEnvironment &environment,
                                  bool reload = false);
    EnvironmentAssetHandle requestEnvironmentAsset(
        const CatalogEnvironment &environment,
        bool reload = false, bool *repositoryHit = nullptr,
        bool *coalesced = nullptr);
    uint64_t reloadCurrentEnvironment();
    void applyRenderSettings(const RenderSettingsPatch &patch);
    const RenderSettings &renderSettings() const;
    int findSceneIndexByName(const std::string &name) const;
    const CatalogEnvironment *
    findEnvironmentByName(const std::string &name) const;
    std::string profileIdForTextureLimit(const SceneEntry &entry) const;

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
    ControlJson runtimeIndexedArtifactStatus(int index) const;
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
    std::unique_ptr<MaterialSystem>      materialSystem_;
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
    enum class ReflectionProbeCapturePhase {
        AwaitingResize,
        CapturingFaces,
        Baking,
        Loading,
    };
    struct ReflectionProbeCaptureState {
        PersistentEntityId entityId;
        ReflectionProbeCapturePhase phase =
            ReflectionProbeCapturePhase::AwaitingResize;
        std::string environmentId;
        std::string profileId;
        std::filesystem::path sourcePath;
        std::filesystem::path backupPath;
        std::filesystem::path temporaryDirectory;
        std::array<std::filesystem::path, 6> faceRelativePaths{};
        std::array<std::filesystem::path, 6> facePaths{};
        VkExtent2D previousExtent{};
        uint32_t faceSize = 256;
        uint32_t faceIndex = 0;
        uint64_t captureTaskId = 0;
        uint64_t bakeTaskId = 0;
        bool catalogEntryAdded = false;
        std::string status;
    };
    std::optional<ReflectionProbeCaptureState> reflectionProbeCapture_;
#endif
    std::unique_ptr<CaptureService>      captureService_;
    std::vector<RenderItem>              renderItems_;
    VisibilitySystem                    visibilitySystem_;
    VisibilityFrame                     visibilityFrame_;
    ShadowSystem                        shadowSystem_;
    std::unique_ptr<RenderSettingsController> renderSettingsController_;

    // 场景切换
    SceneLoadContext        sceneLoadContext_;
    std::unique_ptr<SceneRuntimeCoordinator> sceneRuntime_;

#if VKL_ENABLE_RUNTIME_CONTROL
    std::unique_ptr<RuntimeCommandQueue> runtimeCommandQueue_;
    std::unique_ptr<NamedPipeServerWin32> runtimeControlServer_;
    RuntimeCommandDispatcher runtimeCommandDispatcher_;
    std::shared_ptr<RuntimeCommand> pendingQuitCommand_;
    std::string runtimeControlPipeName_;
#endif
    std::unique_ptr<EditorUiState> editorUi_;
    uint64_t lastCaptureTaskId_ = 0;
    bool captureIncludeGui_ = false;
    std::string captureUiError_;
    uint64_t presentedFrameCount_ = 0;

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
};

} // namespace vkr
