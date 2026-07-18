# Step 5：Viewport/Scissor 封装 + 创建 Application 类（P8 + P5）

> 对应 `app_layer_extraction.md` 第五步。
> 目标：将 viewport/scissor 模板代码移入 Renderer；创建 `vkr::Application` 类替代 `HelloTriangleApplication`；更新 `main.cpp`。

---

## 一、当前状态（Step 4 完成后）

| 文件 | 状态 |
|------|------|
| `vulkan_utils.h` | 仅剩 WIDTH/HEIGHT/MODEL_PATH/TEXTURE_PATH/UniformBufferObject + GLFW/GLM includes |
| `app.h` | `HelloTriangleApplication` 类，`#include "vulkan_utils.h"`，非 namespace |
| `app.cpp` | 使用 `vkr::Key` 枚举、`SurfaceCreator`/`ExtentProvider` 回调，但仍引用 vulkan_utils.h 常量 |
| `app.cpp` mainLoop | 手动设置 viewport/scissor（约 12 行模板代码） |
| `app/Config.h` | 已创建，`vkr::Config` 结构体 |
| `app/UniformData.h` | 已创建，`vkr::GlobalUBO` 结构体 |

---

## 二、改动计划

### 批次 A：Viewport/Scissor 移入 Renderer（1 个文件）

#### A-1. `Renderer.cpp` — `beginRenderPass()` 末尾追加 viewport/scissor 设置

**改前：**
```cpp
void Renderer::beginRenderPass(VkCommandBuffer cmd) {
    // ... vkCmdBeginRenderPass ...
    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
}
```

**改后：**
```cpp
void Renderer::beginRenderPass(VkCommandBuffer cmd) {
    // ... vkCmdBeginRenderPass ...
    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // 自动设置全屏 viewport & scissor
    VkViewport viewport{};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = static_cast<float>(swapChain_->extent().width);
    viewport.height   = static_cast<float>(swapChain_->extent().height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapChain_->extent();
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}
```

> 不改 Renderer.h —— 接口签名不变。

---

### 批次 B：创建 Application 类（2 个新文件）

#### B-1. `src/app/Application.h` — 新建

```cpp
#pragma once

#include "Config.h"

#include <memory>

namespace vkr {

class Window;
class InputManager;
class VulkanContext;
class Device;
class SwapChain;
class Renderer;
class Texture;
class Material;
class Mesh;
class Scene;
class Camera;

class Application {
public:
    explicit Application(const Config& config = {});
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void run();

private:
    void init();
    void mainLoop();

    void processInput(float dt);
    void updateUniforms();

    Config config_;

    // 按创建顺序声明 —— 析构自动逆序销毁，无需手动 cleanup()
    std::unique_ptr<Window>        window_;
    std::unique_ptr<InputManager>  input_;
    std::unique_ptr<VulkanContext>  context_;
    std::unique_ptr<Device>        device_;
    std::unique_ptr<SwapChain>     swapChain_;
    std::unique_ptr<Renderer>      renderer_;

    std::shared_ptr<Texture>       texture_;
    std::shared_ptr<Material>      material_;
    std::shared_ptr<Mesh>          mesh_;

    Scene                          scene_;
    Camera                         camera_;
};

} // namespace vkr
```

注意：
- `Scene` 和 `Camera` 是值语义成员，需要 include 头文件而非前向声明。
- 因为析构器需要看到 `unique_ptr` 管理的完整类型，`~Application()` 在 `.cpp` 中定义（= default）。

#### B-2. `src/app/Application.cpp` — 新建

从 `app.cpp` 迁移逻辑，做以下改动：

| 改动点 | 旧 app.cpp | 新 Application.cpp |
|--------|-----------|-------------------|
| 类名 | `HelloTriangleApplication` | `vkr::Application` |
| 命名空间 | 无 | `namespace vkr { ... }` |
| `#include` | `"app.h"` → `"vulkan_utils.h"` 链 | 直接 include 各精确头文件 |
| 常量 | `WIDTH`, `HEIGHT`, `MODEL_PATH`, `TEXTURE_PATH` | `config_.windowWidth` 等 |
| UBO 类型 | `UniformBufferObject` | `vkr::GlobalUBO` |
| UBO 大小 | `sizeof(UniformBufferObject)` | `sizeof(GlobalUBO)` |
| 输入速度/灵敏度 | 硬编码 `2.0f` / `0.1f` | `config_.moveSpeed` / `config_.mouseSensitivity` |
| viewport/scissor | 手动 12 行代码 | 已移入 Renderer，删除 |
| `cleanup()` | 手动 reset | 删除，依赖析构顺序 |
| `run()` | 先创建 window，再 initVulkan | 合并为 `init()` |
| `vkDeviceWaitIdle` | `device->logicalDevice()` | `device_->logicalDevice()` |
| 成员命名 | `context`(无后缀), `device`(无后缀) | 统一 `context_`, `device_` |

完整内容：

```cpp
#include "Application.h"
#include "UniformData.h"

#include "core/Device.h"
#include "core/SwapChain.h"
#include "core/VulkanContext.h"
#include "render/Material.h"
#include "render/Mesh.h"
#include "render/Renderer.h"
#include "render/Texture.h"
#include "scene/Camera.h"
#include "scene/Scene.h"
#include "window/InputManager.h"
#include "window/Window.h"

#include <chrono>
#include <cstring>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace vkr {

Application::Application(const Config& config) : config_(config) {}

Application::~Application() {
    if (device_)
        vkDeviceWaitIdle(device_->logicalDevice());
}

void Application::run() {
    init();
    mainLoop();
}

void Application::init() {
    window_ = std::make_unique<Window>(
        config_.windowWidth, config_.windowHeight, config_.windowTitle);
    input_ = std::make_unique<InputManager>(*window_);

    auto extensions = Window::getRequiredVulkanExtensions();
    context_ = std::make_unique<VulkanContext>(
        [this](VkInstance inst) { return window_->createSurface(inst); },
        std::move(extensions));
    device_ = std::make_unique<Device>(*context_);
    swapChain_ = std::make_unique<SwapChain>(
        *device_, context_->surface(),
        [this]() { return window_->framebufferExtent(); });
    renderer_ = std::make_unique<Renderer>(
        *device_, *swapChain_, sizeof(GlobalUBO));

    window_->setResizeCallback([this](int, int) { renderer_->notifyResize(); });

    texture_ = std::make_shared<Texture>(
        *device_, *renderer_, config_.texturePath);
    material_ = std::make_shared<Material>(
        *device_, *renderer_, *texture_,
        config_.vertShaderPath, config_.fragShaderPath);
    mesh_ = Mesh::fromOBJ(*device_, *renderer_, config_.modelPath);

    scene_.addObject({mesh_, material_, glm::mat4(1.0f)});
    camera_.setAspect(static_cast<float>(swapChain_->extent().width) /
                      static_cast<float>(swapChain_->extent().height));
}

void Application::processInput(float dt) {
    input_->update();

    if (input_->isKeyDown(Key::Escape))
        window_->setShouldClose(true);

    glm::vec3 move{0.0f};
    if (input_->isKeyDown(Key::W))         move.z += config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::S))         move.z -= config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::A))         move.x -= config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::D))         move.x += config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::Space))     move.y += config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::LeftShift)) move.y -= config_.moveSpeed * dt;
    camera_.translate(move);

    auto delta = input_->mouseDelta();
    camera_.rotate(delta.x * config_.mouseSensitivity,
                  -delta.y * config_.mouseSensitivity);
}

void Application::updateUniforms() {
    GlobalUBO ubo{};
    ubo.view = camera_.viewMatrix();
    ubo.proj = camera_.projectionMatrix();
    std::memcpy(renderer_->mappedUniformBuffer(renderer_->frameIndex()),
                &ubo, sizeof(ubo));
}

void Application::mainLoop() {
    input_->setCursorCaptured(true);

    auto lastTime  = std::chrono::high_resolution_clock::now();
    auto startTime = lastTime;

    while (!window_->shouldClose()) {
        window_->pollEvents();

        auto  now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        processInput(dt);

        VkCommandBuffer cmd = renderer_->beginFrame();
        if (!cmd) continue;

        updateUniforms();

        float time = std::chrono::duration<float>(now - startTime).count();
        scene_.objects()[0].transform =
            glm::rotate(glm::mat4(1.0f), time * glm::radians(90.0f),
                        glm::vec3(0.0f, 0.0f, 1.0f));

        renderer_->beginRenderPass(cmd);  // viewport/scissor 已包含
        scene_.render(cmd, renderer_->frameIndex());
        renderer_->endRenderPass(cmd);
        renderer_->endFrame();
    }

    vkDeviceWaitIdle(device_->logicalDevice());
}

} // namespace vkr
```

> **关于析构器**：析构中先调用 `vkDeviceWaitIdle` 确保 GPU 空闲，再让 `unique_ptr` 按逆序析构。旧代码 `cleanup()` 的手动 `reset()` 序列不再需要。

---

### 批次 C：更新 main.cpp（1 个文件）

**改前：**
```cpp
#include "app.h"
#include <iostream>

int main() {
    HelloTriangleApplication app;
    try {
        app.run();
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
```

**改后：**
```cpp
#include "app/Application.h"
#include "app/Config.h"

#include <cstdlib>
#include <iostream>

int main() {
    vkr::Config config;

    try {
        vkr::Application app(config);
        app.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
```

---

### 批次 D：删除旧文件（3 个文件）

| 删除文件 | 原因 |
|---------|------|
| `src/app.h` | 被 `src/app/Application.h` 替代 |
| `src/app.cpp` | 被 `src/app/Application.cpp` 替代 |
| `src/vulkan_utils.h` | 所有符号已迁移完毕，无引用者 |

> 删除前先确认 `grep -r "vulkan_utils\|app\.h" src/` 除了旧 app.cpp 和 main.cpp 外无其他引用。

---

## 三、执行顺序

```
批次 A (Renderer viewport/scissor)
   A-1  Renderer.cpp
         |
批次 B (新建 Application)
   B-1  app/Application.h     (新建)
   B-2  app/Application.cpp   (新建)
         |
批次 C (更新 main.cpp)
   C-1  main.cpp
         |
   编译验证 ①  ← 此时旧 app.h/app.cpp 仍在，但 main.cpp 已不引用它们
                  CMake GLOB_RECURSE 会同时编译新旧文件
                  旧文件仍能独立编译（只是没人调用）
         |
批次 D (删除旧文件)
   D-1  删除 app.h
   D-2  删除 app.cpp
   D-3  删除 vulkan_utils.h
         |
   编译验证 ②  ← clean build，确保无残留引用
         |
   grep 验收
```

**关于编译验证 ①**：因为 CMake 使用 `file(GLOB_RECURSE)`，旧 `app.cpp` 仍会被编译。它 include 了 `vulkan_utils.h`，所以此时 `vulkan_utils.h` 还不能删。但旧代码能独立编译不报错，只是 `main.cpp` 不再调用 `HelloTriangleApplication`。

**关于编译验证 ②**：删除旧文件后，做 clean build 确保 `vulkan_utils.h` 不被任何文件引用。

---

## 四、Application.h 中关于 Scene/Camera include 的说明

`Application.h` 中 `Scene scene_;` 和 `Camera camera_;` 是值成员，编译器需要看到完整类型。有两种处理方式：

**方案 1（推荐）—— 直接 include**：
```cpp
#include "scene/Camera.h"
#include "scene/Scene.h"
```
简洁直接，Application 本身就是顶层编排类，允许知道具体类型。

**方案 2 —— unique_ptr 包装**：
将 `Scene scene_` / `Camera camera_` 改为 `unique_ptr`，头文件中用前向声明。
增加了间接层和堆分配开销，对这两个轻量对象不值得。

**本方案采用方案 1**。

---

## 五、文件变更总览

| 文件 | 操作 | 改动摘要 |
|------|------|---------|
| `src/render/Renderer.cpp` | 修改 | `beginRenderPass()` 末尾追加 viewport/scissor 设置 |
| `src/app/Application.h` | **新建** | `vkr::Application` 类声明 |
| `src/app/Application.cpp` | **新建** | `vkr::Application` 实现（从 app.cpp 迁移 + 改进） |
| `src/main.cpp` | 修改 | `#include "app/Application.h"` + 使用 `vkr::Application` |
| `src/app.h` | **删除** | 被 Application.h 替代 |
| `src/app.cpp` | **删除** | 被 Application.cpp 替代 |
| `src/vulkan_utils.h` | **删除** | 所有符号已迁移完毕 |

共 **2 个新建**，**2 个修改**，**3 个删除**。

---

## 六、验收标准

| 检查项 | 预期结果 |
|--------|---------|
| `grep -rn "vulkan_utils" src/` | **零命中** |
| `grep -rn "HelloTriangleApplication" src/` | **零命中** |
| `grep -rn "app\.h" src/` | **零命中**（旧头文件已删除） |
| `grep -rn "vkCmdSetViewport\|vkCmdSetScissor" src/` | 仅命中 `Renderer.cpp` |
| `grep -rn "WIDTH\b\|HEIGHT\b\|MODEL_PATH\|TEXTURE_PATH" src/` | **零命中**（由 Config 替代） |
| `src/vulkan_utils.h` | 已删除 |
| `src/app.h` / `src/app.cpp` | 已删除 |
| Debug clean build 通过 | ✅ |
| 运行结果与重构前一致 | ✅ |
