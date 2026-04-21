#pragma once

#include "Config.h"

#include "scene/Camera.h"
#include "scene/Scene.h"

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
class Texture;
class Material;
class Mesh;

class Application {
  public:
    explicit Application(const Config &config = {});
    ~Application();

    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    void run();

  private:
    void init();
    void mainLoop();

    void processInput(float dt);
    void updateUniforms(uint32_t frameIndex);

    Config config_;

    // 按创建顺序声明 —— 析构自动逆序销毁，无需手动 cleanup()
    std::unique_ptr<Window>        window_;
    std::unique_ptr<InputManager>  input_;
    std::unique_ptr<VulkanContext> context_;
    std::unique_ptr<Device>        device_;
    std::unique_ptr<SwapChain>     swapChain_;
    std::unique_ptr<FrameSync>     frameSync_;
    std::unique_ptr<Renderer>      renderer_;

    std::shared_ptr<Texture>           texture_;
    std::shared_ptr<Material>          material_;
    std::shared_ptr<Mesh>              mesh_;       // OBJ path
    std::vector<std::shared_ptr<Mesh>> gltfMeshes_; // glTF path

    Scene  scene_;
    Camera camera_;
};

} // namespace vkr
