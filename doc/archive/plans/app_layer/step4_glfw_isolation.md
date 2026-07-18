# Step 4：GLFW 隔离 + InputManager 键名抽象（P2 + P6）

> 对应 `app_layer_extraction.md` 第四步。
> 目标：将 GLFW 依赖严格限制在 `src/window/` 目录内，`core/` 和 `render/` 模块不再出现任何 GLFW 符号。

---

## 一、当前 GLFW 泄漏点

| # | 文件 | GLFW 用法 | 处理方式 |
|---|------|----------|---------|
| 1 | `VulkanContext.h` | 构造接收 `GLFWwindow*`；成员 `window_` | 改为接收 `SurfaceCreator` 回调 |
| 2 | `VulkanContext.cpp` | `#include <GLFW/glfw3.h>`；`glfwCreateWindowSurface`；`glfwGetRequiredInstanceExtensions` | 全部通过回调/参数注入 |
| 3 | `SwapChain.h` | 构造接收 `GLFWwindow*`；成员 `window_` | 改为接收 `ExtentProvider` 回调 |
| 4 | `SwapChain.cpp` | `#include <GLFW/glfw3.h>`；`glfwGetFramebufferSize` | 通过 `ExtentProvider` 回调 |
| 5 | `app.cpp` | `GLFW_KEY_W/S/A/D/SPACE/LEFT_SHIFT/ESCAPE` | 改用 `vkr::Key` 枚举 |
| 6 | `app.h` | 间接依赖（通过 `vulkan_utils.h` 引入 GLFW） | Step 4 不改 app.h（留到 Step 5） |

> `Window.h/cpp` 和 `InputManager.h/cpp` 内部的 GLFW 使用是**合理的**，不在本步改动范围。

---

## 二、改动计划

### 批次 A：VulkanContext 去 GLFW 化（3 个文件）

#### A-1. `Window.h` — 新增两个方法声明

在 `Window` 类 public 区域添加：

```cpp
// 创建 Vulkan surface（需要 VkInstance）
VkSurfaceKHR createSurface(VkInstance instance) const;

// 获取 GLFW 所需的 Vulkan 实例扩展
static std::vector<const char*> getRequiredVulkanExtensions();
```

需要新增 `#include <vulkan/vulkan.h>` 和 `#include <vector>` 到 `Window.h`。

#### A-2. `Window.cpp` — 实现两个新方法

```cpp
VkSurfaceKHR Window::createSurface(VkInstance instance) const {
    VkSurfaceKHR surface;
    if (glfwCreateWindowSurface(instance, window_, nullptr, &surface) != VK_SUCCESS) {
        throw std::runtime_error("failed to create window surface!");
    }
    return surface;
}

std::vector<const char*> Window::getRequiredVulkanExtensions() {
    uint32_t     glfwExtensionCount = 0;
    const char **glfwExtensions =
        glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    return {glfwExtensions, glfwExtensions + glfwExtensionCount};
}
```

> Surface 创建的错误检查保留 if-throw 模式（Window 层不引入 VulkanCheck.h）。

#### A-3. `VulkanContext.h` — 改构造签名，去掉 GLFWwindow

**改前：**
```cpp
struct GLFWwindow;                       // 删除

VulkanContext(GLFWwindow* window);       // 改签名
GLFWwindow* window_;                     // 删除成员
```

**改后：**
```cpp
#include <functional>                    // 新增

using SurfaceCreator = std::function<VkSurfaceKHR(VkInstance)>;

VulkanContext(SurfaceCreator createSurface,
              std::vector<const char*> requiredExtensions);
```

- 去掉 `createSurface()` 私有方法声明
- 去掉 `getRequiredExtensions()` 私有方法声明
- 去掉 `GLFWwindow* window_` 成员
- 新增 `SurfaceCreator surfaceCreator_` 成员（构造时暂存，用于创建 surface 后可丢弃）

> 也可以不存储 `surfaceCreator_`，改为在构造函数体内直接调用。因为只在构造时用一次，推荐**不存储**，直接在构造函数体内调用。

完整的新头文件声明：

```cpp
#pragma once

#include <vulkan/vulkan.h>
#include <functional>
#include <vector>

namespace vkr {

using SurfaceCreator = std::function<VkSurfaceKHR(VkInstance)>;

class VulkanContext {
public:
    VulkanContext(SurfaceCreator createSurface,
                  std::vector<const char*> requiredExtensions);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    VkInstance   instance() const { return instance_; }
    VkSurfaceKHR surface()  const { return surface_; }

private:
    void createInstance(std::vector<const char*> requiredExtensions);
    void setupDebugMessenger();

    bool checkValidationLayerSupport();
    void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo);

    VkInstance               instance_       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR             surface_        = VK_NULL_HANDLE;
};

} // namespace vkr
```

#### A-4. `VulkanContext.cpp` — 重写构造 + 删除 GLFW 依赖

**改动要点：**

1. **删除** `#include <GLFW/glfw3.h>`
2. **构造函数改为：**
   ```cpp
   VulkanContext::VulkanContext(SurfaceCreator createSurface,
                                std::vector<const char*> requiredExtensions) {
       createInstance(std::move(requiredExtensions));
       setupDebugMessenger();
       surface_ = createSurface(instance_);  // 通过回调创建 surface
   }
   ```
3. **`createInstance()` 签名改为接收 `requiredExtensions` 参数**：
   ```cpp
   void VulkanContext::createInstance(std::vector<const char*> requiredExtensions) {
       // ... 原有逻辑 ...
       if (enableValidationLayers) {
           requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
       }
       createInfo.enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size());
       createInfo.ppEnabledExtensionNames = requiredExtensions.data();
       // ... 其余不变 ...
   }
   ```
4. **删除** `createSurface()` 方法实现（原来的 `glfwCreateWindowSurface` 调用）
5. **删除** `getRequiredExtensions()` 方法实现（已由外部传入）

验证：此时 `VulkanContext.cpp` 中不再有任何 `glfw` / `GLFW` 字样。

---

### 批次 B：SwapChain 去 GLFW 化（2 个文件）

#### B-1. `SwapChain.h` — 改构造签名

**改前：**
```cpp
struct GLFWwindow;                                            // 删除

SwapChain(Device &device, VkSurfaceKHR surface, GLFWwindow *window);
GLFWwindow *window_ = nullptr;                                // 删除
```

**改后：**
```cpp
#include <functional>                                          // 新增

using ExtentProvider = std::function<VkExtent2D()>;

SwapChain(Device &device, VkSurfaceKHR surface, ExtentProvider getExtent);
ExtentProvider getExtent_;                                     // 替代 window_
```

完整改动点：
- 删除 `struct GLFWwindow;` 前向声明
- 删除 `GLFWwindow *window_` 成员
- 新增 `#include <functional>`
- 新增 `using ExtentProvider = std::function<VkExtent2D()>;`（放在 namespace vkr 内，SwapChain 类外）
- 构造参数 `GLFWwindow *window` → `ExtentProvider getExtent`
- 新增成员 `ExtentProvider getExtent_;`

#### B-2. `SwapChain.cpp` — 使用回调替代 glfwGetFramebufferSize

**改动要点：**

1. **删除** `#include <GLFW/glfw3.h>`
2. **构造函数**：`window_(window)` → `getExtent_(std::move(getExtent))`
3. **`chooseSwapExtent()`** 中：
   ```cpp
   // 改前：
   int width, height;
   glfwGetFramebufferSize(window_, &width, &height);
   VkExtent2D actualExtent = {static_cast<uint32_t>(width),
                               static_cast<uint32_t>(height)};

   // 改后：
   VkExtent2D actualExtent = getExtent_();
   ```

验证：此时 `SwapChain.cpp` 中不再有任何 `glfw` / `GLFW` 字样。

---

### 批次 C：InputManager 键名枚举 + app.cpp 去 GLFW 宏（3 个文件）

#### C-1. `InputManager.h` — 新增 `vkr::Key` 枚举 + 重载 `isKeyDown`

在 `InputManager` 类声明之前添加枚举：

```cpp
enum class Key : int {
    W          = 87,    // GLFW_KEY_W
    A          = 65,    // GLFW_KEY_A
    S          = 83,    // GLFW_KEY_S
    D          = 68,    // GLFW_KEY_D
    Space      = 32,    // GLFW_KEY_SPACE
    LeftShift  = 340,   // GLFW_KEY_LEFT_SHIFT
    Escape     = 256,   // GLFW_KEY_ESCAPE
};
```

在 `InputManager` 类中添加重载：

```cpp
bool isKeyDown(Key key) const;   // 新增
```

> 保留原有 `bool isKeyDown(int key) const;` 不删除，以免影响内部实现。后续 Step 5 中新 Application 类只使用 `Key` 版本。

#### C-2. `InputManager.cpp` — 实现 Key 重载

```cpp
bool InputManager::isKeyDown(Key key) const {
    return isKeyDown(static_cast<int>(key));
}
```

#### C-3. `app.cpp` — 替换 GLFW_KEY 宏

将 `app.cpp` 的 `mainLoop()` 中所有 `GLFW_KEY_*` 替换为 `vkr::Key::*`，并去掉对 `vulkan_utils.h` 中 GLFW 的间接依赖：

| 改前 | 改后 |
|------|------|
| `input_->isKeyDown(GLFW_KEY_ESCAPE)` | `input_->isKeyDown(vkr::Key::Escape)` |
| `input_->isKeyDown(GLFW_KEY_W)` | `input_->isKeyDown(vkr::Key::W)` |
| `input_->isKeyDown(GLFW_KEY_S)` | `input_->isKeyDown(vkr::Key::S)` |
| `input_->isKeyDown(GLFW_KEY_A)` | `input_->isKeyDown(vkr::Key::A)` |
| `input_->isKeyDown(GLFW_KEY_D)` | `input_->isKeyDown(vkr::Key::D)` |
| `input_->isKeyDown(GLFW_KEY_SPACE)` | `input_->isKeyDown(vkr::Key::Space)` |
| `input_->isKeyDown(GLFW_KEY_LEFT_SHIFT)` | `input_->isKeyDown(vkr::Key::LeftShift)` |

共 7 处替换。

---

### 批次 D：app.cpp 调用点适配（1 个文件）

由于 VulkanContext 和 SwapChain 构造签名改变，`app.cpp` 的 `initVulkan()` 需要同步更新：

**改前：**
```cpp
context = std::make_unique<vkr::VulkanContext>(window_->handle());
// ...
swapChain_ = std::make_unique<vkr::SwapChain>(*device, context->surface(),
                                               window_->handle());
```

**改后：**
```cpp
auto extensions = vkr::Window::getRequiredVulkanExtensions();
context = std::make_unique<vkr::VulkanContext>(
    [this](VkInstance inst) { return window_->createSurface(inst); },
    std::move(extensions));
// ...
swapChain_ = std::make_unique<vkr::SwapChain>(
    *device, context->surface(),
    [this]() -> VkExtent2D {
        return {window_->width(), window_->height()};
    });
```

> 注意：`window_->width()` / `window_->height()` 返回的是创建时的逻辑尺寸。`chooseSwapExtent` 中只有在 `currentExtent.width == UINT32_MAX` 时才使用此回调，Vulkan 驱动通常返回实际 framebuffer 尺寸，所以大多数情况下不会走到回调。
>
> **更精确的做法**：在 `Window` 中增加 `framebufferSize()` 方法返回实际像素尺寸：
> ```cpp
> // Window.h
> VkExtent2D framebufferExtent() const;
>
> // Window.cpp
> VkExtent2D Window::framebufferExtent() const {
>     int w, h;
>     glfwGetFramebufferSize(window_, &w, &h);
>     return {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
> }
> ```
> 然后 app.cpp 中的 SwapChain 构造改为：
> ```cpp
> swapChain_ = std::make_unique<vkr::SwapChain>(
>     *device, context->surface(),
>     [this]() { return window_->framebufferExtent(); });
> ```
> **推荐采用此方案**，确保高 DPI 显示器下尺寸正确。

---

## 三、执行顺序

```
批次 A (VulkanContext 去 GLFW)    批次 B (SwapChain 去 GLFW)
   A-1  Window.h                    B-1  SwapChain.h
   A-2  Window.cpp                  B-2  SwapChain.cpp
   A-3  VulkanContext.h              |
   A-4  VulkanContext.cpp            |
         |                           |
         └── 批次 D (app.cpp 调用点适配) ──┘
                    |
              编译验证 ①
                    |
              批次 C (Key 枚举 + app.cpp 去 GLFW_KEY)
                C-1  InputManager.h
                C-2  InputManager.cpp
                C-3  app.cpp GLFW_KEY → vkr::Key
                    |
              编译验证 ②
```

- **编译验证 ①**：批次 A + B + D 完成后编译，确保 VulkanContext/SwapChain 构造逻辑正确
- **编译验证 ②**：批次 C 完成后编译，确保 Key 枚举可用

---

## 四、文件变更总览

| 文件 | 操作 | 改动摘要 |
|------|------|---------|
| `src/window/Window.h` | 修改 | 新增 `createSurface()`、`getRequiredVulkanExtensions()`、`framebufferExtent()` 声明；新增 `<vulkan/vulkan.h>` + `<vector>` |
| `src/window/Window.cpp` | 修改 | 实现 3 个新方法 |
| `src/core/VulkanContext.h` | 修改 | 构造改为 `(SurfaceCreator, vector<const char*>)`；去掉 GLFWwindow 前向声明和成员 |
| `src/core/VulkanContext.cpp` | 修改 | 删除 `<GLFW/glfw3.h>`；构造通过回调创建 surface；`createInstance` 接收扩展参数；删除 `createSurface()`、`getRequiredExtensions()` |
| `src/core/SwapChain.h` | 修改 | 构造改为 `(Device&, VkSurfaceKHR, ExtentProvider)`；去掉 GLFWwindow 前向声明和成员 |
| `src/core/SwapChain.cpp` | 修改 | 删除 `<GLFW/glfw3.h>`；构造存储 `getExtent_`；`chooseSwapExtent` 使用回调 |
| `src/window/InputManager.h` | 修改 | 新增 `vkr::Key` 枚举；新增 `isKeyDown(Key)` 重载 |
| `src/window/InputManager.cpp` | 修改 | 实现 `isKeyDown(Key)` |
| `src/app.cpp` | 修改 | VulkanContext/SwapChain 构造调用适配；`GLFW_KEY_*` → `vkr::Key::*`（7 处） |

共 **9 个文件**修改，**0 个新建**，**0 个删除**。

---

## 五、验收标准

| 检查项 | 预期结果 |
|--------|---------|
| `grep -rn "GLFW\|glfw" src/core/` | **零命中** |
| `grep -rn "GLFW\|glfw" src/render/` | **零命中** |
| `grep -rn "GLFW\|glfw" src/app.cpp` | **零命中**（不再直接引用 GLFW 宏） |
| `grep -rn "GLFW\|glfw" src/window/` | 仅 Window.h/cpp 和 InputManager.h/cpp（合理） |
| `grep -rn "GLFW_KEY_" src/` | **零命中** |
| Debug 编译通过 | ✅ |
| 运行结果与重构前一致 | ✅ |
