#pragma once

#include "Config.h"

#include "assets/AssetImportManager.h"
#include "assets/ArtifactIndex.h"
#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"
#include "control/RuntimeCommandDispatcher.h"
#include "diagnostics/SceneLoadStats.h"
#include "render/RenderQueue.h"
#include "render/RenderSettings.h"
#include "render/ShaderVariant.h"
#include "scene/Camera.h"
#include "scene/Scene.h"
#include "scene/SceneFactory.h"

#include <glm/glm.hpp>

#include <cstdint>
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
class CaptureService;
struct RuntimeCommand;
class RuntimeCommandQueue;
class NamedPipeServerWin32;
class SceneLoadManager;
class SceneGpuBuilder;
struct SceneLoadTask;
struct SceneImportUiState;
struct SceneAssetOperationState;

enum class InputMode {
    UI,         // 光标可见，ImGui 接管
    CameraDrag, // 按住右键，相机接管鼠标
};

class Application final : public RuntimeControlHost {
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
    const ShaderVariant &currentShaderVariant() const;
    void applySceneCameraDefaults();
    void processRuntimeCommand();
    void updateSceneLoading();
    void updateAssetImports();
    void drawScenePanel();
    void drawAssetsPanel();
    void drawCapturePanel();
    void requestManualCapture(bool includeGui);
    void updateSceneImport();
    void refreshSceneRegistry(const std::string &selectSceneId = {});
    void reloadArtifactIndex();
    void persistArtifactIndex();

    void loadScene(int index, bool replaceCurrent = false);
    uint64_t reloadCurrentScene();
    void switchScene(int index);
    uint64_t requestSceneLoad(int index, bool sourceFallback = false);
    uint64_t requestSceneOperation(int index, bool sourceFallback = false,
                                   bool loadAfter = true,
                                   ImportReason reason = ImportReason::SceneLoad,
                                   bool forceReimport = false);
    bool cancelSceneLoad(uint64_t taskId);
    bool cancelLoadOperation(uint64_t taskId);
    void finalizeSceneLoad(const std::shared_ptr<SceneLoadTask> &task,
                           bool success);
    uint64_t setTextureLimit(uint32_t limit);
    void setShaderVariant(int index);
    void applyRenderSettings(const RenderSettingsPatch &patch);
    int findSceneIndexByName(const std::string &name) const;
    int findShaderVariantIndexByName(const std::string &name) const;
    std::string profileIdForTextureLimit(const SceneEntry &entry) const;
    void refreshArtifactStatus(int sceneIndex, bool admission = false);
    void refreshAllArtifactStatuses();

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
    ControlJson runtimeAssetImport(const std::string &name, bool force,
                                   bool loadAfter) override;
    ControlJson runtimeAssetCancel(std::optional<uint64_t> taskId) override;
    ControlJson runtimeAssetCacheInfo() override;
    ControlJson runtimeShaderList() override;
    ControlJson runtimeShaderCurrent() override;
    ControlJson runtimeShaderSet(const std::string &name) override;
    ControlJson runtimeCameraGet() override;
    ControlJson runtimeCameraSet(const RuntimeCameraPose &pose) override;
    ControlJson runtimeRenderStatus() override;
    ControlJson runtimeRenderSettingsGet() override;
    ControlJson
    runtimeRenderSettingsSet(const RenderSettingsPatch &patch) override;
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

    Config config_;
    ProjectContext projectContext_;
    SceneCatalog catalog_;

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
    std::unique_ptr<CaptureService>      captureService_;
    RenderQueue                          renderQueue_;
    std::vector<ShaderVariant>           shaderVariants_;
    int                                  currentShaderVariantIndex_ = 0;

    // 场景切换
    std::vector<SceneEntry> sceneRegistry_;
    SceneLoadContext        sceneLoadContext_;
    std::unique_ptr<Scene>  currentScene_;
    int                     currentSceneIndex_ = -1;
    int                     pendingSceneIndex_ = -1;
    std::optional<SceneLoadStats> lastSceneLoadStats_;
    std::unique_ptr<SceneLoadManager> sceneLoadManager_;
    std::unique_ptr<SceneGpuBuilder> sceneGpuBuilder_;
    std::shared_ptr<SceneLoadTask> latestSceneLoadTask_;
    uint64_t lastFinalizedTaskId_ = 0;
    std::unique_ptr<AssetImportManager> assetImportManager_;
    std::unique_ptr<ArtifactIndex> artifactIndex_;
    std::optional<ArtifactIndexUsage> artifactUsage_;
    std::unique_ptr<SceneAssetOperationState> sceneAssetOperations_;

    std::unique_ptr<RuntimeCommandQueue> runtimeCommandQueue_;
    std::unique_ptr<NamedPipeServerWin32> runtimeControlServer_;
    RuntimeCommandDispatcher runtimeCommandDispatcher_;
    std::shared_ptr<RuntimeCommand> pendingQuitCommand_;
    std::string runtimeControlPipeName_;
    std::unique_ptr<SceneImportUiState> sceneImportUi_;
    uint64_t lastCaptureTaskId_ = 0;
    bool captureIncludeGui_ = false;
    std::string captureUiError_;
    uint64_t sceneGeneration_ = 0;
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
    uint32_t  lastUploadedDirectionalLights_ = 0;
    uint32_t  lastUploadedPunctualLights_ = 0;
    uint32_t  lastIgnoredLights_ = 0;
    RenderSettings renderSettings_{};
};

} // namespace vkr
