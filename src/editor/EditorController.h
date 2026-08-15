#pragma once

#include "app/Config.h"
#include "render/RenderView.h"
#include "scene/EnvironmentAssetRepository.h"
#include "scene/SceneEntry.h"
#include "scene/ModelPrepareFactory.h"
#include "scene_data/SceneIds.h"
#include "workflows/SceneWorkflowTypes.h"

#include <glm/glm.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

class AssetsPanel;
class Camera;
class CaptureService;
class Device;
class EditorDockWorkspace;
class FrameSync;
class GuiSystem;
class InspectorPanel;
class MaterialSystem;
class OutlinerPanel;
class RenderSettingsController;
class Renderer;
class RuntimeWorld;
class SceneEditorSession;
class SceneRuntimeCoordinator;
class SceneWorkflowController;
class SceneViewportController;
class ScenesPanel;
class ShadowSystem;
class SwapChain;
class Window;
struct EditorUiState;
struct ProjectContext;
struct RenderSettings;
struct RenderSettingsPatch;
struct ShaderVariant;
class SceneCatalog;
struct SceneRuntimePublication;
struct VisibilityFrame;

struct EditorControllerActions {
    std::function<uint64_t(int, bool, bool, SceneWorkflowRequestReason, bool,
                           bool)>
        requestSceneOperation;
    std::function<uint64_t(uint32_t)> setTextureLimit;
    std::function<uint64_t(const std::string &)> setEnvironment;
};

struct EditorControllerServices {
    Config &config;
    ProjectContext &projectContext;
    SceneCatalog &catalog;
    std::vector<SceneEntry> &sceneRegistry;
    SceneLoadContext &sceneLoadContext;
    Window &window;
    Device &device;
    FrameSync &frameSync;
    SwapChain &swapChain;
    Renderer &renderer;
    GuiSystem &gui;
    MaterialSystem &materialSystem;
    SceneWorkflowController &sceneWorkflow;
    SceneRuntimeCoordinator &sceneRuntime;
    RenderSettingsController &renderSettings;
    CaptureService *captureService = nullptr;
    Camera &camera;
    glm::vec3 &ambientColor;
    float &ambientIntensity;
    glm::vec3 &defaultSunDirection;
    glm::vec3 &defaultSunColor;
    float &defaultSunIntensity;
    VisibilityFrame &visibilityFrame;
    ShadowSystem &shadowSystem;
    RenderViewLightStats &lastLightStats;
    std::function<bool()> cameraDragging;
    EditorControllerActions actions;
};

struct EditorViewportDiagnostics {
    uint32_t displayWidth = 0;
    uint32_t displayHeight = 0;
    bool visible = false;
    bool hovered = false;
    bool resizePending = false;
};

class EditorController final {
  public:
    explicit EditorController(EditorControllerServices services);
    ~EditorController();

    EditorController(const EditorController &) = delete;
    EditorController &operator=(const EditorController &) = delete;

    void onWorldPublished(const SceneRuntimePublication &publication);
    void update();
    void beginFrame();
    void draw();

    bool hasUnsavedChanges() const;
    bool interceptCloseRequest();
    bool handleEscape();
    void requestManualCapture();

    bool activeSceneCamera() const;
    bool viewportHovered() const;
    bool blocksViewportInput() const;
    bool anyItemActive() const;
    void setCameraDragActive(bool active);

    void bindViewportTextures();
    void applyPendingViewportResize();
    void applyReflectionProbeCaptureView(RenderViewInput &input,
                                         std::string &cameraIdentity) const;
    std::optional<PersistentEntityId> focusedLightEntity() const;
    EditorViewportDiagnostics viewportDiagnostics() const;

  private:
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

    struct ViewportResizeState {
        uint32_t desiredWidth = 0;
        uint32_t desiredHeight = 0;
        std::chrono::steady_clock::time_point changedAt{};
        bool pending = false;
        bool immediate = false;
        bool measured = false;
    };

    uint64_t requestSceneOperation(
        int index, bool sourceFallback = false, bool loadAfter = true,
        SceneWorkflowRequestReason reason =
            SceneWorkflowRequestReason::SceneLoad,
        bool forceReimport = false, bool reloadAsset = false);
    uint64_t setTextureLimit(uint32_t limit);
    uint64_t setEnvironment(const std::string &id);
    void setShaderVariant(const std::string &id);
    void applyRenderSettings(const RenderSettingsPatch &patch);
    const RenderSettings &renderSettings() const;
    const ShaderVariant &currentShaderVariant() const;

    void refreshSceneRegistry(const std::string &selectSceneId = {});
    void requestEditorSceneLoad(int index);
    void updateEditorModelBindings();
    void updateEditorReflectionProbeBindings();
    void beginReflectionProbeCapture(PersistentEntityId entityId);
    void updateReflectionProbeCapture();
    void requestManualCapture(bool includeGui);

    void drawScenePanel(bool modelsOnly = false);
    void drawOutlinerPanel();
    void drawInspectorPanel();
    void drawSceneAuthoringDialogs();
    void saveEditorScene();
    void executePendingEditorAction(bool saveFirst);
    void deleteSelectedEditorEntity();
    void duplicateSelectedEditorEntity();
    void handleEditorShortcuts();
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

    Config &config_;
    ProjectContext &projectContext_;
    SceneCatalog &catalog_;
    std::vector<SceneEntry> &sceneRegistry_;
    SceneLoadContext &sceneLoadContext_;
    Window *window_ = nullptr;
    Device *device_ = nullptr;
    FrameSync *frameSync_ = nullptr;
    SwapChain *swapChain_ = nullptr;
    Renderer *renderer_ = nullptr;
    GuiSystem *gui_ = nullptr;
    MaterialSystem *materialSystem_ = nullptr;
    SceneWorkflowController *sceneWorkflow_ = nullptr;
    SceneRuntimeCoordinator *sceneRuntime_ = nullptr;
    RenderSettingsController *renderSettingsController_ = nullptr;
    CaptureService *captureService_ = nullptr;
    Camera &camera_;
    glm::vec3 &ambientColor_;
    float &ambientIntensity_;
    glm::vec3 &defaultSunDirection_;
    glm::vec3 &defaultSunColor_;
    float &defaultSunIntensity_;
    VisibilityFrame &visibilityFrame_;
    ShadowSystem &shadowSystem_;
    RenderViewLightStats &lastLightStats_;
    std::function<bool()> cameraDragging_;
    EditorControllerActions actions_;

    std::unique_ptr<EditorDockWorkspace> editorDockWorkspace_;
    std::unique_ptr<AssetsPanel> assetsPanel_;
    std::unique_ptr<ScenesPanel> scenesPanel_;
    std::unique_ptr<OutlinerPanel> outlinerPanel_;
    std::unique_ptr<InspectorPanel> inspectorPanel_;
    std::unique_ptr<SceneEditorSession> sceneEditorSession_;
    std::unique_ptr<SceneViewportController> sceneViewportController_;
    std::unique_ptr<EditorUiState> editorUi_;
    ViewportResizeState viewportResize_{};
    uint32_t viewportDisplayWidth_ = 0;
    uint32_t viewportDisplayHeight_ = 0;
    bool viewportVisible_ = false;
    bool viewportHovered_ = false;
    std::optional<ReflectionProbeCaptureState> reflectionProbeCapture_;
    uint64_t lastCaptureTaskId_ = 0;
    bool captureIncludeGui_ = false;
    std::string captureUiError_;
};

} // namespace vkr
