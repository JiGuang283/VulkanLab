# 阶段 4.1：Window 封装 + InputManager + 键鼠控制相机

## 一、目标

将 `app.cpp` 中散落的 GLFW 调用（窗口创建、事件轮询、回调注册、销毁）抽象为独立的 `Window` 类，同时引入 `InputManager` 管理键鼠状态，并将 Camera 的 `rotate()`/`translate()` 接口与实际输入连接起来，实现 WASD + 鼠标 FPS 风格相机控制。

---

## 二、当前状态

`app.cpp` 中的 GLFW 使用点：

```cpp
// initWindow()
glfwInit();
glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
glfwWindowHint(GLFW_RESIZABLE, GL_FALSE);
window = glfwCreateWindow(WIDTH, HEIGHT, "Vulkan", nullptr, nullptr);
glfwSetWindowUserPointer(window, this);
glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);

// mainLoop()
while (!glfwWindowShouldClose(window)) { glfwPollEvents(); ... }

// cleanup()
glfwDestroyWindow(window);
glfwTerminate();

// framebufferResizeCallback() — static
glfwGetWindowUserPointer(window);
```

其他文件的 GLFW 依赖（**不在本阶段改动**）：
- `VulkanContext` — 接收 `GLFWwindow*` 创建 Surface、获取 Vulkan 扩展
- `SwapChain` — 接收 `GLFWwindow*` 获取 framebuffer 大小

这两处仍通过 `Window::handle()` 传递原始指针，保持不变。

---

## 三、类设计

### 3.1 Window

```cpp
// src/window/Window.h
#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct GLFWwindow;

namespace vkr {

class Window {
public:
    Window(uint32_t width, uint32_t height, const std::string& title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool shouldClose() const;
    void pollEvents();

    GLFWwindow* handle() const { return window_; }
    uint32_t    width()  const { return width_; }
    uint32_t    height() const { return height_; }

    // ---- 回调注册 ----
    using ResizeCallback = std::function<void(int width, int height)>;
    void setResizeCallback(ResizeCallback cb);

private:
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);

    GLFWwindow* window_ = nullptr;
    uint32_t    width_;
    uint32_t    height_;

    ResizeCallback resizeCallback_;
};

} // namespace vkr
```

### 设计要点

1. **构造函数接管 GLFW 初始化** — `glfwInit()`、`glfwWindowHint()`、`glfwCreateWindow()` 全部移入构造函数。
2. **析构函数接管销毁** — `glfwDestroyWindow()` + `glfwTerminate()`。
3. **回调通过 `std::function`** — `setResizeCallback()` 注册 resize 回调，内部通过 `glfwSetWindowUserPointer(this)` + static callback 转发。App 层不再直接处理 GLFW 回调。
4. **`handle()` 暴露原始指针** — VulkanContext / SwapChain 仍需 `GLFWwindow*`，本阶段不改动它们的构造签名。
5. **不包含任何输入处理** — 键鼠状态由 InputManager 负责。

### 3.2 InputManager

```cpp
// src/window/InputManager.h
#pragma once

#include <glm/glm.hpp>

struct GLFWwindow;

namespace vkr {

class InputManager {
public:
    explicit InputManager(GLFWwindow* window);
    ~InputManager();

    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    /// 每帧开始时调用，更新按键状态和鼠标 delta
    void update();

    // ---- 键盘 ----
    bool isKeyDown(int key) const;

    // ---- 鼠标 ----
    glm::vec2 mouseDelta() const { return mouseDelta_; }

    /// 启用/禁用鼠标捕获（FPS 模式）
    void setCursorCaptured(bool captured);
    bool isCursorCaptured() const { return cursorCaptured_; }

private:
    static void mouseCallback(GLFWwindow* window, double xpos, double ypos);

    GLFWwindow* window_ = nullptr;

    // 鼠标
    glm::vec2 mouseDelta_{0.0f};
    double    lastMouseX_ = 0.0;
    double    lastMouseY_ = 0.0;
    bool      firstMouse_ = true;
    bool      cursorCaptured_ = false;
};

} // namespace vkr
```

### 设计要点

1. **构造时注册鼠标回调** — `glfwSetCursorPosCallback()`，通过 `glfwSetWindowUserPointer` 需要与 Window 协调。采用方案：Window 设置 userPointer 为自己，Window 持有 InputManager 指针，或 InputManager 使用 static + map。**简洁方案**：让 Window 在 userPointer 中存储一个结构体指针，包含 Window* 和 InputManager*。

   **实际方案**：Window 的 `glfwSetWindowUserPointer` 存储 `Window*`，Window 提供 `setInputManager(InputManager*)` 让 InputManager 挂载自己。Window 内部的 static mouseCallback 转发给 InputManager。这样避免多个类争抢 userPointer。

2. **`update()` 每帧调用** — 重置 mouseDelta，通过 `glfwGetKey()` 轮询按键状态（比回调更适合持续按压检测）。
3. **`setCursorCaptured(true)`** — 调用 `glfwSetInputMode(GLFW_CURSOR, GLFW_CURSOR_DISABLED)`，进入 FPS 鼠标模式。按 ESC 时可退出。
4. **不依赖 Camera** — InputManager 只提供原始输入数据，Camera 控制逻辑在 App 层。

### 3.3 UserPointer 协调方案

由于 GLFW 只有一个 `glfwSetWindowUserPointer`，让 Window 独占它：

```cpp
// Window 内部
struct WindowUserData {
    Window*       window = nullptr;
    InputManager* input  = nullptr;
};
```

Window 构造时创建 `WindowUserData` 存入 userPointer，InputManager 构造时通过 Window 注册自己。所有 GLFW 回调从 userPointer 取 `WindowUserData`，分发给对应的处理者。

---

## 四、实施步骤

### 步骤 1：创建 Window.h / Window.cpp

文件位置：`src/window/Window.h`、`src/window/Window.cpp`

Window 构造函数：
```cpp
Window::Window(uint32_t width, uint32_t height, const std::string& title)
    : width_(width), height_(height) {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    window_ = glfwCreateWindow(width_, height_, title.c_str(), nullptr, nullptr);
    if (!window_) throw std::runtime_error("failed to create GLFW window");

    userData_.window = this;
    glfwSetWindowUserPointer(window_, &userData_);
    glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);
}
```

Window 析构函数：
```cpp
Window::~Window() {
    glfwDestroyWindow(window_);
    glfwTerminate();
}
```

静态回调转发：
```cpp
void Window::framebufferResizeCallback(GLFWwindow* w, int width, int height) {
    auto* data = static_cast<WindowUserData*>(glfwGetWindowUserPointer(w));
    if (data->window && data->window->resizeCallback_)
        data->window->resizeCallback_(width, height);
}
```

### 步骤 2：创建 InputManager.h / InputManager.cpp

文件位置：`src/window/InputManager.h`、`src/window/InputManager.cpp`

构造函数：
```cpp
InputManager::InputManager(Window& window) : window_(window.handle()) {
    // 注册到 Window 的 userData
    auto* data = static_cast<WindowUserData*>(glfwGetWindowUserPointer(window_));
    data->input = this;

    glfwSetCursorPosCallback(window_, mouseCallback);
}
```

`update()`：
```cpp
void InputManager::update() {
    mouseDelta_ = {0.0f, 0.0f};
    // mouseDelta 由 mouseCallback 在 pollEvents 期间累积
    // isKeyDown 使用 glfwGetKey 实时查询，无需在此更新
}
```

`isKeyDown()`：
```cpp
bool InputManager::isKeyDown(int key) const {
    return glfwGetKey(window_, key) == GLFW_PRESS;
}
```

鼠标回调：
```cpp
void InputManager::mouseCallback(GLFWwindow* w, double xpos, double ypos) {
    auto* data = static_cast<WindowUserData*>(glfwGetWindowUserPointer(w));
    if (!data->input || !data->input->cursorCaptured_) return;

    auto* self = data->input;
    if (self->firstMouse_) {
        self->lastMouseX_ = xpos;
        self->lastMouseY_ = ypos;
        self->firstMouse_ = false;
        return;
    }

    self->mouseDelta_.x += static_cast<float>(xpos - self->lastMouseX_);
    self->mouseDelta_.y += static_cast<float>(ypos - self->lastMouseY_);
    self->lastMouseX_ = xpos;
    self->lastMouseY_ = ypos;
}
```

`setCursorCaptured()`：
```cpp
void InputManager::setCursorCaptured(bool captured) {
    cursorCaptured_ = captured;
    glfwSetInputMode(window_, GLFW_CURSOR,
                     captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (captured) firstMouse_ = true;  // 防止切入时跳变
}
```

### 步骤 3：修改 app.h

```diff
- #include "vulkan_utils.h"
+ #include "vulkan_utils.h"
+ #include "window/Window.h"
+ #include "window/InputManager.h"

  class HelloTriangleApplication {
    private:
-     GLFWwindow *window;
+     std::unique_ptr<vkr::Window>       window_;
+     std::unique_ptr<vkr::InputManager> input_;

      // ... 其余不变
-     void        initWindow();
      // initWindow 删除，Window 在 run() 或 initVulkan 前创建
-     static void framebufferResizeCallback(...);
      // 回调逻辑移入 Window
  };
```

### 步骤 4：修改 app.cpp — 删除 initWindow / framebufferResizeCallback

替换 `initWindow()` 的逻辑为 `Window` 构造：
```cpp
void HelloTriangleApplication::run() {
    window_ = std::make_unique<vkr::Window>(WIDTH, HEIGHT, "Vulkan");
    input_  = std::make_unique<vkr::InputManager>(*window_);

    window_->setResizeCallback([this](int, int) {
        renderer_->notifyResize();
    });

    initVulkan();
    mainLoop();
    cleanup();
}
```

### 步骤 5：修改 app.cpp — initVulkan

将 `window` 替换为 `window_->handle()`：
```cpp
void HelloTriangleApplication::initVulkan() {
    context = std::make_unique<vkr::VulkanContext>(window_->handle());
    // ...
    swapChain_ = std::make_unique<vkr::SwapChain>(
        *device, context->surface(), window_->handle());
    // ...
}
```

### 步骤 6：修改 app.cpp — mainLoop（接入键鼠控制相机）

```cpp
void HelloTriangleApplication::mainLoop() {
    input_->setCursorCaptured(true);

    auto lastTime = std::chrono::high_resolution_clock::now();

    while (!window_->shouldClose()) {
        window_->pollEvents();

        // ---- deltaTime ----
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        // ---- 输入处理 ----
        input_->update();

        // ESC 退出鼠标捕获
        if (input_->isKeyDown(GLFW_KEY_ESCAPE))
            window_->setShouldClose(true);

        // WASD 移动
        const float speed = 2.0f;
        glm::vec3 move{0.0f};
        if (input_->isKeyDown(GLFW_KEY_W)) move.z += speed * dt;
        if (input_->isKeyDown(GLFW_KEY_S)) move.z -= speed * dt;
        if (input_->isKeyDown(GLFW_KEY_A)) move.x -= speed * dt;
        if (input_->isKeyDown(GLFW_KEY_D)) move.x += speed * dt;
        if (input_->isKeyDown(GLFW_KEY_SPACE))     move.y += speed * dt;
        if (input_->isKeyDown(GLFW_KEY_LEFT_SHIFT)) move.y -= speed * dt;
        camera_.translate(move);

        // 鼠标旋转
        const float sensitivity = 0.1f;
        auto delta = input_->mouseDelta();
        camera_.rotate(delta.x * sensitivity, -delta.y * sensitivity);

        // ---- 渲染 ----
        VkCommandBuffer cmd = renderer_->beginFrame();
        if (!cmd) continue;

        updateUniformBuffer(renderer_->frameIndex());

        // 场景物体动画（保留旋转演示）
        static auto startTime = std::chrono::high_resolution_clock::now();
        float time = std::chrono::duration<float>(now - startTime).count();
        scene_.objects()[0].transform = glm::rotate(
            glm::mat4(1.0f), time * glm::radians(90.0f),
            glm::vec3(0.0f, 0.0f, 1.0f));

        renderer_->beginRenderPass(cmd);
        // viewport / scissor ...
        scene_.render(cmd, renderer_->frameIndex());
        renderer_->endRenderPass(cmd);
        renderer_->endFrame();
    }

    vkDeviceWaitIdle(device->logicalDevice());
}
```

### 步骤 7：修改 app.cpp — cleanup

```cpp
void HelloTriangleApplication::cleanup() {
    renderer_.reset();
    material_.reset();
    texture_.reset();
    mesh_.reset();
    swapChain_.reset();
    device.reset();
    context.reset();

    input_.reset();
    window_.reset();   // ~Window() 处理 glfwDestroyWindow + glfwTerminate
}
```

删除原有的 `glfwDestroyWindow(window)` 和 `glfwTerminate()`。

### 步骤 8：构建验证

- 两个 build 目录执行 `cmake ..`（新增 `window/` 下的 .cpp）
- Release / Debug 编译通过
- 运行：
  - 旋转的 viking_room 正常渲染
  - WASD + 鼠标可控制相机移动和旋转
  - ESC 关闭窗口

---

## 五、变更文件清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `src/window/Window.h` | 新建 | Window 类声明 + WindowUserData 结构 |
| `src/window/Window.cpp` | 新建 | GLFW 窗口创建/销毁/回调转发 |
| `src/window/InputManager.h` | 新建 | InputManager 声明 |
| `src/window/InputManager.cpp` | 新建 | 键盘轮询 + 鼠标回调 + 捕获管理 |
| `src/app.h` | 修改 | `GLFWwindow*` → `unique_ptr<Window>` + `unique_ptr<InputManager>`，删除 `initWindow`/`framebufferResizeCallback` |
| `src/app.cpp` | 修改 | `run()` 创建 Window/InputManager，`mainLoop()` 接入输入控制相机，`cleanup()` 简化 |

---

## 六、依赖关系

```
Window (GLFW 独占封装)
  ├── 拥有 GLFWwindow*
  ├── 管理 WindowUserData (userPointer)
  └── 转发 resize 回调

InputManager (输入层)
  ├── 挂载到 Window 的 UserData
  ├── isKeyDown() → glfwGetKey
  ├── mouseDelta() → 鼠标回调累积
  └── setCursorCaptured() → glfwSetInputMode

App mainLoop
  ├── input_->update() → 重置 delta
  ├── input_->isKeyDown(W/A/S/D) → camera_.translate()
  ├── input_->mouseDelta() → camera_.rotate()
  └── scene_.render() → 绘制
```

---

## 七、注意事项

1. **VulkanContext / SwapChain 仍接收 `GLFWwindow*`** — 通过 `window_->handle()` 传入，本阶段不改动它们的签名。阶段 4 的目标是让 GLFW 调用**仅存在于 `window/` 目录下**的文件中（加上 VulkanContext/SwapChain 中必要的 `glfwGetFramebufferSize` / `glfwCreateWindowSurface` 等 Vulkan-GLFW 桥接调用）。
2. **`glfwSetWindowUserPointer` 只能存一个指针** — 使用 `WindowUserData` 结构体统一管理，避免 Window 和 InputManager 互相覆盖。
3. **鼠标 delta 累积时机** — `glfwPollEvents()` 会触发所有排队回调，所以 mouseCallback 在 `pollEvents()` 期间被调用。`input_->update()` 应在 `pollEvents()` 之前调用来重置上一帧的 delta，然后 `pollEvents()` 中的回调会写入新帧的 delta。
4. **首帧跳变** — `firstMouse_` 标记确保鼠标捕获后的第一个事件不产生巨大 delta。
5. **`vulkan_utils.h` 中的 `GLFW_INCLUDE_VULKAN` / `#include <GLFW/glfw3.h>`** — 本阶段暂时保留。后续可考虑将 GLFW include 限制到 `window/` 和 `core/` 中。
