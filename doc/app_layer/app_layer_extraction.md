# 应用层拆分详细方案

> 本文档基于 `optimization_plan.md` 中的 P1（拆分 vulkan_utils.h）、P2（GLFW 隔离）、P3（VK_CHECK）、P5（Config 系统）、P8（Viewport 封装）等优化项，给出统一的"应用层拆分"实施方案。
> 目标：将当前 `HelloTriangleApplication` 这个"准 God Object"演化为一个清晰分层的 `Application`，使应用层仅包含初始化编排、主循环调度和输入响应逻辑。

---

## 一、当前 app 层问题分析

### 1.1 `HelloTriangleApplication` 现状

```
HelloTriangleApplication
├── run()                   — 创建 Window、InputManager，调用 initVulkan → mainLoop → cleanup
├── initVulkan()            — 按顺序创建所有 Vulkan 对象和场景资源
├── mainLoop()              — 帧循环：输入 → Camera → Renderer → Scene::render
├── cleanup()               — 手动 reset 所有 unique_ptr（实际上可以依赖析构顺序）
└── updateUniformBuffer()   — 填写 Camera 的 view/proj 到 UBO
```

**问题清单：**

| # | 问题 | 涉及优化项 |
|---|------|-----------|
| 1 | 依赖 `vulkan_utils.h` 万能头文件，间接引入 GLFW/stb/tinyobj/GLM 所有符号 | P1 |
| 2 | `mainLoop()` 直接使用 `GLFW_KEY_W` 等宏常量 | P2, P6 |
| 3 | `mainLoop()` 手动设置 Viewport / Scissor（约 10 行模板代码） | P8 |
| 4 | `WIDTH`, `HEIGHT`, `MODEL_PATH`, `TEXTURE_PATH` 是全局常量，无法运行时配置 | P5 |
| 5 | `UniformBufferObject` 定义在 `vulkan_utils.h` 中，它是应用层的 UBO 布局 | P1 |
| 6 | `cleanup()` 手动 reset 各 unique_ptr，顺序需小心维护 | 代码质量 |
| 7 | `HelloTriangleApplication` 类名沿用教程命名，不符合项目定位 | 命名 |

### 1.2 `vulkan_utils.h` 的内容归属

| 内容 | 当前使用者 | 目标归属 |
|------|-----------|---------|
| `WIDTH`, `HEIGHT` | app.cpp | → `Config.h` |
| `MODEL_PATH`, `TEXTURE_PATH` | app.cpp | → `Config.h` |
| `MAX_FRAMES_IN_FLIGHT` | Renderer.h | → `Renderer.h` 内部常量 |
| `enableValidationLayers`, `validationLayers`, `deviceExtensions` | VulkanContext.cpp | → `VulkanContext.cpp` 匿名命名空间 |
| `QueueFamilyIndices`, `SwapChainSupportDetails` | Device, SwapChain | → `core/VulkanTypes.h`（新建） |
| `Vertex` + `std::hash<Vertex>` | Mesh.cpp | → `render/Vertex.h`（新建） |
| `UniformBufferObject` | app.cpp | → `app/UniformData.h`（新建，或就近 `app.h`） |
| `CreateDebugUtilsMessengerEXT` / `DestroyDebugUtilsMessengerEXT` | VulkanContext.cpp | → `VulkanContext.cpp` 内部 |
| GLFW / GLM / stb / tinyobj includes | 各模块 | → 各使用者自行 include |

---

## 二、目标架构

### 2.1 目录结构变更

```
src/
├── main.cpp                      # 仅创建 Application 并 run()
├── app/                          # [NEW] 应用层目录
│   ├── Application.h             # 替代 HelloTriangleApplication
│   ├── Application.cpp
│   ├── Config.h                  # 运行时配置结构体 (P5)
│   └── UniformData.h             # 应用层 UBO 布局定义
│
├── core/
│   ├── VulkanCheck.h             # [NEW] VK_CHECK 宏 (P3)
│   ├── VulkanTypes.h             # [NEW] QueueFamilyIndices, SwapChainSupportDetails
│   ├── VulkanContext.h/cpp       # 不再持有 GLFWwindow* (P2)
│   ├── Device.h/cpp
│   ├── SwapChain.h/cpp           # 不再持有 GLFWwindow* (P2)
│   ├── Buffer.h/cpp
│   ├── Image.h/cpp
│   └── Pipeline.h/cpp
│
├── render/
│   ├── Vertex.h                  # [NEW] Vertex 定义 + hash
│   ├── Renderer.h/cpp            # beginRenderPass 内含 Viewport/Scissor 设置 (P8)
│   ├── Material.h/cpp
│   ├── Mesh.h/cpp
│   └── Texture.h/cpp
│
├── scene/
│   ├── Camera.h/cpp
│   ├── Scene.h/cpp
│   └── SceneObject.h
│
├── window/
│   ├── Window.h/cpp
│   └── InputManager.h/cpp        # 提供 vkr::Key 枚举 (P6)
│
├── stb_image.cpp                 # 实现文件保持原位
├── tiny_obj_loader.cpp
└── vk_mem_alloc.cpp
```

**删除文件：**
- `src/vulkan_utils.h` — 拆分完成后删除
- `src/app.h` / `src/app.cpp` — 由 `src/app/Application.h/.cpp` 替代

### 2.2 Application 类设计

```cpp
// src/app/Application.h
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

    // 按创建顺序声明，析构自动逆序销毁 —— 无需手动 cleanup()
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

**关键改进：**
1. 成员声明顺序 = 依赖顺序，析构自动逆序释放，**不需要手动 `cleanup()`**。
2. 接收 `Config` 结构体，所有路径/尺寸从外部传入。
3. `processInput()` 分离输入处理逻辑，使用 `vkr::Key` 枚举而非 GLFW 宏。
4. 类放入 `vkr` 命名空间。

### 2.3 Config 结构体

```cpp
// src/app/Config.h
#pragma once

#include <string>
#include <cstdint>

namespace vkr {

struct Config {
    uint32_t    windowWidth      = 800;
    uint32_t    windowHeight     = 600;
    std::string windowTitle      = "Vulkan Renderer";
    std::string modelPath        = "models/viking_room.obj";
    std::string texturePath      = "textures/viking_room.png";
    std::string vertShaderPath   = "shader/vert.spv";
    std::string fragShaderPath   = "shader/frag.spv";
    bool        enableValidation = true;
    float       moveSpeed        = 2.0f;
    float       mouseSensitivity = 0.1f;
};

} // namespace vkr
```

### 2.4 UniformData 定义

```cpp
// src/app/UniformData.h
#pragma once

#include <glm/glm.hpp>

namespace vkr {

struct GlobalUBO {
    glm::mat4 view;
    glm::mat4 proj;
};

} // namespace vkr
```

---

## 三、分步实施计划

整个拆分拆成 **6 步**，每步可独立编译验证。

### Step 1：新建基础头文件（不改已有文件）

新建以下文件（内容见上方及下方详述），**不修改**任何现有代码，确保编译不受影响：

| 新文件 | 内容 |
|--------|------|
| `src/core/VulkanCheck.h` | `VK_CHECK` 宏定义 |
| `src/core/VulkanTypes.h` | `QueueFamilyIndices` + `SwapChainSupportDetails`（从 vulkan_utils.h 复制） |
| `src/render/Vertex.h` | `Vertex` 结构体 + `std::hash<Vertex>` 特化（从 vulkan_utils.h 复制） |
| `src/app/Config.h` | `vkr::Config` 结构体 |
| `src/app/UniformData.h` | `vkr::GlobalUBO` 结构体 |

**验收**：现有代码编译通过，新文件能被 include。

---

### Step 2：迁移 vulkan_utils.h 中的符号到新头文件

逐个模块修改 include：

#### 2.1 core 模块
- `Device.h` / `Device.cpp`：`#include "vulkan_utils.h"` → `#include "VulkanTypes.h"` + 必要的 `<vulkan/vulkan.h>` 等
- `SwapChain.h` / `SwapChain.cpp`：同上
- `VulkanContext.cpp`：将 `enableValidationLayers`、`validationLayers`、`deviceExtensions`、`CreateDebugUtilsMessengerEXT`、`DestroyDebugUtilsMessengerEXT` 移入 `.cpp` 匿名命名空间

#### 2.2 render 模块
- `Mesh.cpp`：`#include "Vertex.h"` 替代对 `vulkan_utils.h` 中 `Vertex` 的依赖
- `Renderer.h` / `Renderer.cpp`：移除 `#include "vulkan_utils.h"`，按需 include 具体头
  - `MAX_FRAMES_IN_FLIGHT` 定义为 `Renderer.h` 内的 `static constexpr` 或匿名命名空间常量
- `Material.cpp`：移除 `#include "vulkan_utils.h"`

#### 2.3 逐文件操作
对每个 `.cpp` / `.h` 文件：
1. 移除 `#include "vulkan_utils.h"`
2. 添加原本间接依赖的头文件（`<vulkan/vulkan.h>`, `<glm/glm.hpp>`, `<vector>`, `<optional>` 等）
3. 编译验证

**验收**：`vulkan_utils.h` 在所有文件中的 `#include` 被移除；该文件可删除；编译通过。

---

### Step 3：VK_CHECK 宏替换（P3）

在 Step 2 进行的同时或之后：

1. 每个含 `if (vkXxx(...) != VK_SUCCESS) throw ...` 模式的文件中 `#include "core/VulkanCheck.h"`
2. 将所有此类模式替换为 `VK_CHECK(vkXxx(...))`

涉及文件（预估）：
- `VulkanContext.cpp`（createInstance, setupDebugMessenger, createSurface）
- `Device.cpp`（createLogicalDevice）
- `SwapChain.cpp`（createSwapChain）
- `Buffer.cpp`（createBuffer, allocateMemory）
- `Image.cpp`（createImage）
- `Pipeline.cpp`（createPipelineLayout, createGraphicsPipelines）
- `Renderer.cpp`（createRenderPass, createFramebuffers, createCommandPool, allocateCommandBuffers, createSyncObjects, beginFrame, endFrame）
- `Material.cpp`（createDescriptorPool, allocateDescriptorSets）
- `Texture.cpp`（createSampler）

**验收**：`grep -r "!= VK_SUCCESS" src/` 返回零结果。

---

### Step 4：GLFW 隔离 + InputManager 键名抽象（P2 + P6）

#### 4.1 VulkanContext 去 GLFW 化
**改前：**
```cpp
VulkanContext(GLFWwindow* window);
// 内部调用 glfwCreateWindowSurface, glfwGetRequiredInstanceExtensions
```

**改后：**
```cpp
VulkanContext(VkSurfaceKHR surface,
              const std::vector<const char*>& requiredExtensions);
// surface 由 Window 创建并传入
~VulkanContext();  // surface 的销毁仍由 VulkanContext 负责（因为 instance 在此）
```

需要在 `Window` 类中增加方法：
```cpp
// Window.h
VkSurfaceKHR createSurface(VkInstance instance) const;
static std::vector<const char*> getRequiredExtensions();
```

创建流程变化：
```cpp
// 旧：VulkanContext 内部包办一切
context_ = std::make_unique<VulkanContext>(window_->handle());

// 新：Window 提供信息，Application 组装
auto extensions = Window::getRequiredExtensions();
context_ = std::make_unique<VulkanContext>(extensions);  // 此时只创建 instance
VkSurfaceKHR surface = window_->createSurface(context_->instance());
context_->setSurface(surface);  // 或者 context 构造后再设置
```

> **备选方案（更简单）**：VulkanContext 构造拆为两步不太优雅。可以改为让 VulkanContext 接收一个 `SurfaceProvider` 回调：
> ```cpp
> using SurfaceCreator = std::function<VkSurfaceKHR(VkInstance)>;
> VulkanContext(SurfaceCreator createSurface);
> ```
> 在 Application 中传入 `[&](VkInstance inst) { return window_->createSurface(inst); }`。
> 此方案保持 VulkanContext 构造的原子性，且不显式依赖 GLFW。

#### 4.2 SwapChain 去 GLFW 化
**改前：**
```cpp
SwapChain(Device& device, VkSurfaceKHR surface, GLFWwindow* window);
// 内部调用 glfwGetFramebufferSize
```

**改后：**
```cpp
using ExtentProvider = std::function<VkExtent2D()>;
SwapChain(Device& device, VkSurfaceKHR surface, ExtentProvider getExtent);
```

Application 层传入：
```cpp
swapChain_ = std::make_unique<SwapChain>(
    *device_, context_->surface(),
    [this]() -> VkExtent2D {
        return { window_->width(), window_->height() };
    });
```

#### 4.3 InputManager 键名枚举

```cpp
// src/window/InputManager.h
namespace vkr {

enum class Key : int {
    W          = 87,   // GLFW_KEY_W
    A          = 65,   // GLFW_KEY_A
    S          = 83,   // GLFW_KEY_S
    D          = 68,   // GLFW_KEY_D
    Space      = 32,   // GLFW_KEY_SPACE
    LeftShift  = 340,  // GLFW_KEY_LEFT_SHIFT
    Escape     = 256,  // GLFW_KEY_ESCAPE
};

class InputManager {
public:
    // ...
    bool isKeyDown(Key key) const;  // 替代 isKeyDown(int key)
};

} // namespace vkr
```

Application 中：
```cpp
if (input_->isKeyDown(vkr::Key::Escape))
    window_->setShouldClose(true);
if (input_->isKeyDown(vkr::Key::W))
    move.z += speed * dt;
// ...
```

#### 4.4 删除 Renderer.cpp 中无用的 GLFW include

**验收**：`grep -rn "GLFW\|glfw" src/` 仅命中 `src/window/` 目录。

---

### Step 5：Viewport/Scissor 封装（P8）+ 创建 Application 类

#### 5.1 Viewport/Scissor 移入 Renderer

将 `app.cpp` 中 `mainLoop()` 里的 viewport/scissor 设置代码移入 `Renderer::beginRenderPass()`：

```cpp
void Renderer::beginRenderPass(VkCommandBuffer cmd) {
    // ... 已有的 vkCmdBeginRenderPass ...

    // [新增] 自动设置全屏 viewport & scissor
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

#### 5.2 创建 Application 类

新建 `src/app/Application.h` 和 `src/app/Application.cpp`：

```cpp
// src/app/Application.cpp
#include "Application.h"
#include "UniformData.h"

#include "core/VulkanContext.h"
#include "core/Device.h"
#include "core/SwapChain.h"
#include "render/Renderer.h"
#include "render/Texture.h"
#include "render/Material.h"
#include "render/Mesh.h"
#include "scene/Camera.h"
#include "scene/Scene.h"
#include "window/Window.h"
#include "window/InputManager.h"

#include <chrono>

namespace vkr {

Application::Application(const Config& config) : config_(config) {}

Application::~Application() = default;  // unique_ptr 按声明逆序自动销毁

void Application::run() {
    init();
    mainLoop();
    // 无需手动 cleanup — 析构自动处理
}

void Application::init() {
    window_ = std::make_unique<Window>(
        config_.windowWidth, config_.windowHeight, config_.windowTitle);
    input_ = std::make_unique<InputManager>(*window_);

    context_ = std::make_unique<VulkanContext>(/* ... */);
    device_  = std::make_unique<Device>(*context_);
    swapChain_ = std::make_unique<SwapChain>(
        *device_, context_->surface(), /* ExtentProvider */);
    renderer_ = std::make_unique<Renderer>(
        *device_, *swapChain_, sizeof(GlobalUBO));

    window_->setResizeCallback([this](int, int) { renderer_->notifyResize(); });

    texture_  = std::make_shared<Texture>(*device_, *renderer_, config_.texturePath);
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
    if (input_->isKeyDown(Key::W))     move.z += config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::S))     move.z -= config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::A))     move.x -= config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::D))     move.x += config_.moveSpeed * dt;
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
    memcpy(renderer_->mappedUniformBuffer(renderer_->frameIndex()),
           &ubo, sizeof(ubo));
}

void Application::mainLoop() {
    input_->setCursorCaptured(true);
    auto lastTime  = std::chrono::high_resolution_clock::now();
    auto startTime = lastTime;

    while (!window_->shouldClose()) {
        window_->pollEvents();

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        processInput(dt);

        VkCommandBuffer cmd = renderer_->beginFrame();
        if (!cmd) continue;

        updateUniforms();

        // 示例：旋转第一个物体
        float time = std::chrono::duration<float>(now - startTime).count();
        scene_.objects()[0].transform =
            glm::rotate(glm::mat4(1.0f), time * glm::radians(90.0f),
                        glm::vec3(0.0f, 0.0f, 1.0f));

        renderer_->beginRenderPass(cmd);  // 已包含 viewport/scissor
        scene_.render(cmd, renderer_->frameIndex());
        renderer_->endRenderPass(cmd);
        renderer_->endFrame();
    }

    vkDeviceWaitIdle(device_->logicalDevice());
}

} // namespace vkr
```

#### 5.3 更新 main.cpp

```cpp
// src/main.cpp
#include "app/Application.h"
#include "app/Config.h"

#include <iostream>
#include <cstdlib>

int main() {
    vkr::Config config;
    // 可从命令行参数或配置文件加载

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

**验收**：旧 `app.h` / `app.cpp` 删除，`main.cpp` 仅依赖 `app/Application.h` + `app/Config.h`，编译运行结果不变。

---

### Step 6：清理与最终验证

1. 删除 `src/vulkan_utils.h`
2. 删除旧 `src/app.h` / `src/app.cpp`
3. 更新 `CMakeLists.txt`（添加新文件，移除旧文件）
4. 全量编译 + 运行验证

---

## 四、文件变更总览

### 新增文件（7 个）
| 文件 | 说明 |
|------|------|
| `src/app/Config.h` | 运行时配置结构体 |
| `src/app/Application.h` | 应用类声明 |
| `src/app/Application.cpp` | 应用类实现 |
| `src/app/UniformData.h` | 全局 UBO 布局定义 |
| `src/core/VulkanCheck.h` | `VK_CHECK` 宏 |
| `src/core/VulkanTypes.h` | `QueueFamilyIndices`, `SwapChainSupportDetails` |
| `src/render/Vertex.h` | `Vertex` 结构体 + hash |

### 删除文件（3 个）
| 文件 | 原因 |
|------|------|
| `src/vulkan_utils.h` | 内容已拆分到各细粒度头文件 |
| `src/app.h` | 被 `src/app/Application.h` 替代 |
| `src/app.cpp` | 被 `src/app/Application.cpp` 替代 |

### 修改文件（约 15 个）
| 文件 | 改动摘要 |
|------|---------|
| `src/main.cpp` | 改用 `vkr::Application` |
| `src/core/VulkanContext.h/cpp` | 去 GLFW 化，接收回调创建 surface |
| `src/core/SwapChain.h/cpp` | 去 GLFW 化，接收 `ExtentProvider` |
| `src/core/Device.h/cpp` | `#include "VulkanTypes.h"` 替代 vulkan_utils |
| `src/core/Buffer.cpp` | `VK_CHECK` 替代手动 if-throw |
| `src/core/Image.cpp` | 同上 |
| `src/core/Pipeline.cpp` | 同上 |
| `src/render/Renderer.h/cpp` | `MAX_FRAMES_IN_FLIGHT` 内聚；viewport/scissor 移入 beginRenderPass；移除 GLFW include |
| `src/render/Material.cpp` | `VK_CHECK`；移除 vulkan_utils include |
| `src/render/Mesh.cpp` | `#include "Vertex.h"` |
| `src/render/Texture.cpp` | `VK_CHECK` |
| `src/window/InputManager.h/cpp` | 新增 `vkr::Key` 枚举 |
| `src/window/Window.h/cpp` | 新增 `createSurface()`, `getRequiredExtensions()` |
| `CMakeLists.txt` | 更新源文件列表 |

---

## 五、依赖关系梳理（改后）

```
main.cpp
  └─► app/Application
        ├─► app/Config               (纯数据，无依赖)
        ├─► app/UniformData           (仅依赖 GLM)
        ├─► window/Window             (仅依赖 GLFW)
        ├─► window/InputManager       (仅依赖 GLFW)
        ├─► core/VulkanContext        (仅依赖 Vulkan)
        ├─► core/Device               (依赖 VulkanContext)
        ├─► core/SwapChain            (依赖 Device, 回调获取尺寸)
        ├─► render/Renderer           (依赖 Device, SwapChain)
        ├─► render/Texture            (依赖 Device, Renderer)
        ├─► render/Material           (依赖 Device, Renderer, Texture, Pipeline)
        ├─► render/Mesh               (依赖 Device, Renderer)
        ├─► scene/Scene               (依赖 SceneObject)
        └─► scene/Camera              (仅依赖 GLM)
```

**GLFW 依赖边界**：仅 `window/Window` 和 `window/InputManager` 包含 `<GLFW/glfw3.h>`，其余模块完全无感知。

---

## 六、验收标准汇总

| 检查项 | 预期结果 |
|--------|---------|
| `vulkan_utils.h` | 已删除 |
| `app.h` / `app.cpp` | 已删除 |
| `grep -r "vulkan_utils" src/` | 零命中 |
| `grep -r "GLFW\|glfw" src/` | 仅命中 `src/window/` |
| `grep -r "GLFW_KEY_" src/` | 零命中（已被 `vkr::Key` 替代） |
| `grep -r "!= VK_SUCCESS" src/` | 零命中（已被 `VK_CHECK` 替代） |
| `grep -r "HelloTriangleApplication" src/` | 零命中 |
| `app.cpp` 中无 `vkCmdSetViewport` / `vkCmdSetScissor` | ✅ 移入 Renderer |
| 常量来源 | 全部来自 `Config` 或模块内部常量 |
| 编译 Debug + Release | 通过 |
| 渲染结果 | 与重构前一致 |

---

## 七、风险与注意事项

1. **VulkanContext 的 surface 销毁顺序**：surface 必须在 instance 之前销毁。改用回调创建方案后，surface 仍归 VulkanContext 在析构中销毁（紧接在 debugMessenger 之后、instance 之前），不改变当前逻辑。

2. **SwapChain recreate 时需获取最新窗口尺寸**：使用 `ExtentProvider` 回调方式天然支持，每次 `recreate()` 内部调用回调获取最新尺寸。

3. **CMakeLists.txt 中 `aux_source_directory` 或 `file(GLOB)`**：新增 `src/app/` 子目录后需要更新。建议使用显式文件列表或 `file(GLOB_RECURSE)` 以自动发现新文件。

4. **头文件搜索路径**：各模块之间 `#include` 路径形如 `"core/VulkanTypes.h"`、`"app/Config.h"` 等，需确保 CMake 中 `include_directories` 包含 `src/` 目录（当前应已配置）。

5. **渐进式迁移**：可以先让新旧文件并存一段时间——`Application` 和 `HelloTriangleApplication` 都能编译，各自的 `main.cpp` 条件编译切换——验证功能后再移除旧文件。
