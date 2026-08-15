#pragma once

#include "Config.h"

#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"
#include "workflows/SceneWorkflowTypes.h"
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
class EditorController;
#endif
class CaptureService;
#if VKL_ENABLE_RUNTIME_CONTROL
class RuntimeControlAdapter;
#endif
class EnvironmentAssetHandle;
struct SceneLoadTask;
struct ModelAsset;
struct EnvironmentLoadTask;
class SceneRuntimeCoordinator;
class SceneWorkflowController;
class RenderSettingsController;

enum class InputMode {
    UI,         // Cursor visible; editor controls input.
    CameraDrag, // 按住右键，相机接管鼠标
};

class Application final {
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
    void handleSwapChainRecreate();
    const ShaderVariant &currentShaderVariant() const;
    bool hasUnsavedSceneChanges() const;

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
    std::unique_ptr<EditorController> editorController_;
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
    std::unique_ptr<RuntimeControlAdapter> runtimeControl_;
#endif
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
