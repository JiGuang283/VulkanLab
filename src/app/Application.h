#pragma once

#include "Config.h"

#include "scene/Camera.h"
#include "scene/Scene.h"
#include "scene/SceneFactory.h"

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace vkr {

class Window;
class InputManager;
class VulkanContext;
class Device;
class SwapChain;
class FrameSync;
class Renderer;
class Pipeline;
class GuiSystem;

enum class InputMode {
    UI,         // 光标可见，ImGui 接管
    CameraDrag, // 按住右键，相机接管鼠标
};

class Application {
  public:
    explicit Application(const Config &config = {});
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

    void switchScene(int index);

    Config config_;

    // 基础设施（创建顺序 = 析构逆序）
    std::unique_ptr<Window>        window_;
    std::unique_ptr<InputManager>  input_;
    std::unique_ptr<VulkanContext> context_;
    std::unique_ptr<Device>        device_;
    std::unique_ptr<SwapChain>     swapChain_;
    std::unique_ptr<FrameSync>     frameSync_;
    std::unique_ptr<Renderer>      renderer_;
    std::unique_ptr<Pipeline>      opaquePipeline_;
    std::unique_ptr<GuiSystem>     gui_;

    // 场景切换
    std::vector<SceneEntry> sceneRegistry_;
    std::unique_ptr<Scene>  currentScene_;
    int                     currentSceneIndex_ = -1;
    int                     pendingSceneIndex_ = -1;

    // 输入模式
    InputMode  mode_ = InputMode::UI;
    glm::dvec2 savedCursor_{};

    Camera camera_;
};

} // namespace vkr
