#pragma once

#include "Config.h"

#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"
#include "diagnostics/SceneLoadStats.h"
#include "render/RenderQueue.h"
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
struct RuntimeCommand;
class RuntimeCommandQueue;
class NamedPipeServerWin32;
class SceneLoadManager;
class SceneGpuBuilder;
struct SceneLoadTask;

enum class InputMode {
    UI,         // 光标可见，ImGui 接管
    CameraDrag, // 按住右键，相机接管鼠标
};

class Application {
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
    void updateUniforms(uint32_t frameIndex);
    void drawGui();
    void handleSwapChainRecreate();
    const ShaderVariant &currentShaderVariant() const;
    void applySceneCameraDefaults();
    void processRuntimeCommand();
    void updateSceneLoading();

    void loadScene(int index, bool replaceCurrent = false);
    void reloadCurrentScene();
    void switchScene(int index);
    uint64_t requestSceneLoad(int index);
    bool cancelSceneLoad(uint64_t taskId);
    void finalizeSceneLoad(const std::shared_ptr<SceneLoadTask> &task,
                           bool success);
    void setTextureLimit(uint32_t limit);
    void setShaderVariant(int index);
    int findSceneIndexByName(const std::string &name) const;
    int findShaderVariantIndexByName(const std::string &name) const;
    std::string profileIdForTextureLimit(const SceneEntry &entry) const;

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

    std::unique_ptr<RuntimeCommandQueue> runtimeCommandQueue_;
    std::unique_ptr<NamedPipeServerWin32> runtimeControlServer_;
    std::shared_ptr<RuntimeCommand> pendingQuitCommand_;

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
};

} // namespace vkr
