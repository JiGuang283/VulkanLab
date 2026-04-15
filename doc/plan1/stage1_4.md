## 阶段 1.4 SwapChain RAII — 具体操作步骤

### 前置分析

**需要搬入 SwapChain 类的成员变量：**

| 资源类型 | 当前成员变量 | 所在文件 |
|---------|------------|---------|
| 交换链 | `swapChain` | app.h |
| 交换链图像 | `swapChainImages` | app.h |
| 图像格式 | `swapChainImageFormat` | app.h |
| 交换链范围 | `swapChainExtent` | app.h |
| 图像视图 | `swapChainImageViews` | app.h |

**需要搬入的方法：**

| 方法 | 当前位置 | 说明 |
|------|---------|------|
| `createSwapChain()` | vulkan_swapchain.cpp | 交换链创建主逻辑 |
| `createImageViews()` | vulkan_swapchain.cpp | 交换链 ImageView 创建 |
| `chooseSwapSurfaceFormat()` | vulkan_swapchain.cpp | 格式选择策略 |
| `chooseSwapPresentMode()` | vulkan_swapchain.cpp | 呈现模式选择策略 |
| `chooseSwapExtent()` | vulkan_swapchain.cpp | 范围选择（依赖 GLFW 窗口） |
| `createImageView()` | vulkan_texture.cpp | 通用 ImageView 创建辅助函数 |

**同时应搬入 SwapChain 的资源（与交换链生命周期一致）：**

`cleanupSwapChain()` 中一并销毁的还有 `renderFinishedSemaphores`，它们的数量等于交换链图像数，随交换链重建而重建。但同步对象在计划中预留给阶段 2（Renderer / RenderFrame），因此 **1.4 暂不搬移同步对象**，仅将 `renderFinishedSemaphores` 的创建/销毁保留在 App 中。

**不搬入 SwapChain 的资源（属于渲染层，等阶段 2）：**

- `swapChainFramebuffers` — Framebuffer 依赖 RenderPass，属于 Renderer 职责
- `colorImage_` / `depthImage_` — 附件资源，属于 Renderer 职责
- `renderFinishedSemaphores` — 同步对象，属于 RenderFrame 职责

**设计决策：**

1. `createImageView()` 是通用辅助函数，阶段 1.3 中保留在 App 仅为交换链 ImageView 服务。现在吸收进 SwapChain 内部作为私有方法，同时从 App 中删除。
2. `chooseSwapExtent()` 需要 `GLFWwindow*` 来获取 framebuffer 大小。SwapChain 构造时传入 `GLFWwindow*` 并存储，`recreate()` 时复用。
3. `recreateSwapChain()` 目前除了重建交换链本身，还重建 semaphores/colorResources/depthResources/framebuffers。拆分后 SwapChain 只负责自身 `recreate()`，其余由 App 协调。

---

### 步骤 1：创建 `src/core/SwapChain.h`

```cpp
#pragma once
#include <vector>
#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace vkr {

class Device;

class SwapChain {
public:
    SwapChain(Device& device, VkSurfaceKHR surface, GLFWwindow* window);
    ~SwapChain();

    SwapChain(const SwapChain&) = delete;
    SwapChain& operator=(const SwapChain&) = delete;

    // 重建交换链（窗口大小改变时调用）
    void recreate();

    VkSwapchainKHR               handle()      const { return swapChain_; }
    VkFormat                     imageFormat()  const { return imageFormat_; }
    VkExtent2D                   extent()       const { return extent_; }
    const std::vector<VkImage>&  images()       const { return images_; }
    const std::vector<VkImageView>& imageViews() const { return imageViews_; }
    uint32_t                     imageCount()   const { return static_cast<uint32_t>(images_.size()); }

private:
    void createSwapChain();
    void createImageViews();
    void cleanup();

    // 选择策略
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(
        const std::vector<VkSurfaceFormatKHR>& availableFormats);
    VkPresentModeKHR chooseSwapPresentMode(
        const std::vector<VkPresentModeKHR>& availablePresentModes);
    VkExtent2D chooseSwapExtent(
        const VkSurfaceCapabilitiesKHR& capabilities);

    // ImageView 辅助
    VkImageView createImageView(VkImage image, VkFormat format,
                                VkImageAspectFlags aspectFlags, uint32_t mipLevels);

    Device*      device_  = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;  // 非拥有，VulkanContext 管理
    GLFWwindow*  window_  = nullptr;         // 非拥有

    VkSwapchainKHR           swapChain_   = VK_NULL_HANDLE;
    VkFormat                 imageFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D               extent_      = {0, 0};
    std::vector<VkImage>     images_;       // Vulkan 拥有，不手动销毁
    std::vector<VkImageView> imageViews_;   // 需要手动销毁
};

} // namespace vkr
```

**要点：**
- `device_` 用裸指针（非拥有引用），与 Buffer/Image 保持一致的风格
- `surface_` 和 `window_` 均为非拥有指针，由 VulkanContext 和 App 分别管理
- `images_` 中的 `VkImage` 由 Vulkan 交换链拥有，`cleanup()` 中 **不** 销毁它们
- `imageViews_` 是我们创建的，需要在 `cleanup()` 中逐个销毁
- `recreate()` 先 cleanup 再重新创建，等价于当前 `cleanupSwapChain()` 中交换链相关部分 + `createSwapChain()` + `createImageViews()`

---

### 步骤 2：创建 `src/core/SwapChain.cpp`

逻辑直接搬自 `vulkan_swapchain.cpp` 中 `createSwapChain()` / `createImageViews()` 以及 `vulkan_texture.cpp` 中 `createImageView()`：

```cpp
#include "SwapChain.h"
#include "Device.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace vkr {

SwapChain::SwapChain(Device& device, VkSurfaceKHR surface, GLFWwindow* window)
    : device_(&device), surface_(surface), window_(window)
{
    createSwapChain();
    createImageViews();
}

SwapChain::~SwapChain() {
    cleanup();
}

void SwapChain::recreate() {
    cleanup();
    createSwapChain();
    createImageViews();
}

void SwapChain::createSwapChain() {
    // 直接搬自 HelloTriangleApplication::createSwapChain()
    // device->querySwapChainSupport()  →  device_->querySwapChainSupport()
    // context->surface()               →  surface_（已存储）
    // device->logicalDevice()          →  device_->logicalDevice()
    // device->queueFamilies()          →  device_->queueFamilies()
    // 最后把 swapChain/images/format/extent 存入成员变量
    ...
}

void SwapChain::createImageViews() {
    // 直接搬自 HelloTriangleApplication::createImageViews()
    // 用内部的 createImageView() 方法
    ...
}

VkImageView SwapChain::createImageView(VkImage image, VkFormat format,
                                       VkImageAspectFlags aspectFlags,
                                       uint32_t mipLevels) {
    // 直接搬自 HelloTriangleApplication::createImageView()（在 vulkan_texture.cpp 中）
    ...
}

// 三个 choose 方法原样搬入
VkSurfaceFormatKHR SwapChain::chooseSwapSurfaceFormat(...) { ... }
VkPresentModeKHR   SwapChain::chooseSwapPresentMode(...)   { ... }
VkExtent2D         SwapChain::chooseSwapExtent(...)         { ... }

void SwapChain::cleanup() {
    VkDevice d = device_->logicalDevice();
    for (auto view : imageViews_) {
        vkDestroyImageView(d, view, nullptr);
    }
    imageViews_.clear();
    images_.clear();

    if (swapChain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(d, swapChain_, nullptr);
        swapChain_ = VK_NULL_HANDLE;
    }
}

} // namespace vkr
```

**搬移规则（伪代码→实际代码对照）：**

| 原调用 | SwapChain 中替换为 |
|--------|-------------------|
| `device->logicalDevice()` | `device_->logicalDevice()` |
| `device->querySwapChainSupport()` | `device_->querySwapChainSupport()` |
| `device->queueFamilies()` | `device_->queueFamilies()` |
| `context->surface()` | `surface_` |
| `window` (GLFW 窗口指针) | `window_` |
| `swapChain` (成员) | `swapChain_` |
| `swapChainImages` | `images_` |
| `swapChainImageFormat` | `imageFormat_` |
| `swapChainExtent` | `extent_` |
| `swapChainImageViews` | `imageViews_` |

---

### 步骤 3：修改 `app.h` — 替换成员变量与方法声明

**添加 include：**
```cpp
#include "core/SwapChain.h"
```

**替换交换链成员变量：**

| 删除 | 替换为 |
|------|--------|
| `VkSwapchainKHR swapChain;` | `std::unique_ptr<vkr::SwapChain> swapChain_;` |
| `std::vector<VkImage> swapChainImages;` | （删除，使用 `swapChain_->images()`） |
| `VkFormat swapChainImageFormat;` | （删除，使用 `swapChain_->imageFormat()`） |
| `VkExtent2D swapChainExtent;` | （删除，使用 `swapChain_->extent()`） |
| `std::vector<VkImageView> swapChainImageViews;` | （删除，使用 `swapChain_->imageViews()`） |

> 用 `unique_ptr` 是因为 SwapChain 在 `initVulkan()` 中延迟构建（需要先有 Device 和 Surface）。

**删除已搬走的方法声明：**
```cpp
// 删除
void createSwapChain();
void createImageViews();
VkSurfaceFormatKHR chooseSwapSurfaceFormat(...);
VkPresentModeKHR chooseSwapPresentMode(...);
VkExtent2D chooseSwapExtent(...);
VkImageView createImageView(VkImage image, VkFormat format,
                            VkImageAspectFlags aspectFlags, uint32_t mipLevels);
```

**保留的方法（暂由 App 协调）：**
```cpp
void recreateSwapChain();  // 保留，但内部调用 swapChain_->recreate() + 重建其他依赖资源
void cleanupSwapChain();   // 保留，但精简为重建交换链时的附属资源清理
```

---

### 步骤 4：更新 `app.cpp` — `initVulkan()` 和 `cleanup()`

**`initVulkan()` 中：**
```cpp
// 替换
// createSwapChain();
// createImageViews();
// 为
swapChain_ = std::make_unique<vkr::SwapChain>(
    *device, context->surface(), window);
```

**`cleanup()` 中：**
```cpp
// cleanupSwapChain() 内部会 reset swapChain_
// 最后 cleanup 结尾不需要手动销毁交换链了
```

---

### 步骤 5：更新 `vulkan_swapchain.cpp` — 精简 / 删除已搬走的代码

**删除的方法（已搬入 SwapChain 类）：**
- `HelloTriangleApplication::createSwapChain()`
- `HelloTriangleApplication::createImageViews()`
- `HelloTriangleApplication::chooseSwapSurfaceFormat()`
- `HelloTriangleApplication::chooseSwapPresentMode()`
- `HelloTriangleApplication::chooseSwapExtent()`

**修改 `recreateSwapChain()`：**
```cpp
void HelloTriangleApplication::recreateSwapChain() {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(window, &width, &height);
        glfwWaitEvents();
    }

    vkDeviceWaitIdle(device->logicalDevice());

    cleanupSwapChain();

    swapChain_->recreate();
    createSwapChainSemaphores();
    createColorResources();
    createDepthResources();
    createFramebuffers();
}
```

**修改 `cleanupSwapChain()`：**
```cpp
void HelloTriangleApplication::cleanupSwapChain() {
    for (auto semaphore : renderFinishedSemaphores) {
        vkDestroySemaphore(device->logicalDevice(), semaphore, nullptr);
    }
    renderFinishedSemaphores.clear();

    for (auto framebuffer : swapChainFramebuffers) {
        vkDestroyFramebuffer(device->logicalDevice(), framebuffer, nullptr);
    }

    colorImage_.reset();
    depthImage_.reset();

    // 交换链本身由 swapChain_->recreate() 内部处理，这里不再销毁
    // 或者如果是最终 cleanup（析构），由 swapChain_.reset() / unique_ptr 析构处理
}
```

> **关键变化**：`cleanupSwapChain()` 不再销毁交换链相关的 ImageViews 和 SwapchainKHR。这些由 `SwapChain::cleanup()` 在 `recreate()` 内部或析构时处理。

---

### 步骤 6：更新 `vulkan_texture.cpp` — 删除 `createImageView()`

`createImageView()` 已搬入 `SwapChain` 类，此处删除：

```cpp
// 删除整个 HelloTriangleApplication::createImageView() 方法
```

确认当前代码中唯一使用 `createImageView()` 的地方是 `createImageViews()`（在 vulkan_swapchain.cpp 中），而 `createImageViews()` 已搬入 SwapChain 类。所以删除后不会有调用方缺失。

---

### 步骤 7：全局替换旧成员变量引用

以下地方引用了旧的交换链成员变量，需要改为通过 `swapChain_` 访问：

| 旧写法 | 新写法 | 涉及文件 |
|--------|--------|---------|
| `swapChain` | `swapChain_->handle()` | vulkan_drawframe.cpp (`drawFrame`) |
| `swapChainImages` | `swapChain_->images()` | vulkan_drawframe.cpp (`createSwapChainSemaphores`) |
| `swapChainImages.size()` | `swapChain_->imageCount()` | vulkan_drawframe.cpp |
| `swapChainImageFormat` | `swapChain_->imageFormat()` | vulkan_renderpass.cpp, vulkan_msaa.cpp |
| `swapChainExtent` | `swapChain_->extent()` | vulkan_drawframe.cpp, vulkan_commandpool.cpp, vulkan_framebuffers.cpp, vulkan_depth.cpp, vulkan_msaa.cpp, vulkan_uniform.cpp, vulkan_pipeline.cpp |
| `swapChainImageViews` | `swapChain_->imageViews()` | vulkan_framebuffers.cpp |
| `swapChainFramebuffers` | `swapChainFramebuffers`（不变，仍在 App 中） | — |

**详细涉及文件清单：**

#### 7.1 `vulkan_drawframe.cpp`
```
drawFrame():
  swapChain       → swapChain_->handle()
  
createSwapChainSemaphores():
  swapChainImages.size() → swapChain_->imageCount()
```

#### 7.2 `vulkan_commandpool.cpp` (recordCommandBuffer)
```
  swapChainExtent → swapChain_->extent()
```

#### 7.3 `vulkan_framebuffers.cpp`
```
  swapChainImageViews.size() → swapChain_->imageViews().size()
  swapChainImageViews[i]     → swapChain_->imageViews()[i]
  swapChainExtent            → swapChain_->extent()
```

#### 7.4 `vulkan_renderpass.cpp`
```
  swapChainImageFormat → swapChain_->imageFormat()
```

#### 7.5 `vulkan_pipeline.cpp`
```
  swapChainExtent → swapChain_->extent()
```
> 注意: pipeline 中可能只在注释或被注释掉的代码中引用了 extent（动态状态），需确认实际代码。

#### 7.6 `vulkan_msaa.cpp`
```
  swapChainImageFormat → swapChain_->imageFormat()
  swapChainExtent      → swapChain_->extent()
```

#### 7.7 `vulkan_depth.cpp`
```
  swapChainExtent → swapChain_->extent()
```

#### 7.8 `vulkan_uniform.cpp`
```
  swapChainExtent → swapChain_->extent()
```

---

### 步骤 8：处理 `createSurface()` 遗留声明

当前 `app.h` 中仍有 `void createSurface();` 声明，但 `vulkan_swapchain.cpp` 中该方法已被注释掉（surface 创建已在阶段 1.1 搬入 VulkanContext）。确认无实际调用后删除该声明。

---

### 步骤 9：编译验证

1. 重新运行 CMake configure（GLOB_RECURSE 自动发现新文件 `src/core/SwapChain.cpp`）
2. Debug / Release 编译通过
3. 运行渲染结果不变
4. 验证层无报错
5. 窗口大小调整（如果启用了 resize）仍正常工作

---

### 步骤 10：验收 checklist

- [ ] App 中不再直接持有 `VkSwapchainKHR`、`swapChainImages`、`swapChainImageFormat`、`swapChainExtent`、`swapChainImageViews`
- [ ] App 中不再有 `createSwapChain()`、`createImageViews()`、`chooseSwap*()` 方法
- [ ] App 中不再有 `createImageView()` 方法（已并入 SwapChain 内部）
- [ ] `cleanupSwapChain()` 中不再有 `vkDestroyImageView` / `vkDestroySwapchainKHR` 调用
- [ ] `cleanup()` 中交换链资源由 `swapChain_.reset()` 自动销毁（或更早由 `cleanupSwapChain()` 间接触发）
- [ ] 渲染结果与重构前完全一致

---

### 需要注意的坑

1. **`recreate()` 中 oldSwapchain 的优化（可选）**
   当前 `createSwapChain()` 设置 `createInfo.oldSwapchain = VK_NULL_HANDLE`。更好的做法是在 `recreate()` 时传入旧的 swapchain handle 到 `createInfo.oldSwapchain`，让驱动复用资源。但这是优化项，1.4 可以先不做（保持行为不变），在后续阶段或单独 PR 中优化。如果要做：
   ```cpp
   void SwapChain::recreate() {
       VkSwapchainKHR oldSwapChain = swapChain_;
       // 不销毁旧的，先创建新的
       createSwapChain(oldSwapChain);  // 传入 oldSwapchain
       // 新的创建成功后销毁旧的 imageViews 和 swapchain
       ...
   }
   ```
   复杂度较高，建议 1.4 不做。

2. **成员变量引用范围广泛**
   `swapChainExtent` 在至少 7 个文件中被引用。替换时必须逐文件确认，不能遗漏。建议用全局搜索 `swapChainExtent`、`swapChainImageFormat`、`swapChainImages`、`swapChainImageViews`、`swapChain` 逐一确认。

3. **`swapChain` 名称冲突**
   旧成员变量叫 `swapChain`，新的 unique_ptr 叫 `swapChain_`。在 `drawFrame()` 中有 `VkSwapchainKHR swapChains[] = {swapChain};`，替换为 `{swapChain_->handle()}`。注意局部变量名 `swapChains`（复数）和成员变量 `swapChain`（单数）不要混淆。

4. **`createImageView()` 的归属**
   当前在 `vulkan_texture.cpp` 中实现。搬入 SwapChain 后，如果未来其他地方需要创建独立的 ImageView（非交换链、非 `vkr::Image` 的），则需要把它做成 Device 的公共方法或自由函数。但根据当前分析，`createImageView()` 只剩交换链在用（纹理/深度/MSAA 的 ImageView 创建已在 1.3 中搬入 `vkr::Image::createView()`），因此放入 SwapChain 私有即可。

5. **头文件依赖** 
   `SwapChain.cpp` 需要 include `<GLFW/glfw3.h>`（用于 `glfwGetFramebufferSize`）。`SwapChain.h` 前向声明 `struct GLFWwindow;` 即可，不需要在头文件中引入 GLFW。这保持了头文件的轻量性。

6. **`cleanup()` 中的销毁顺序**
   最终 `cleanup()` 时调用链为：
   ```
   cleanupSwapChain()        → 销毁 semaphores, framebuffers, colorImage_, depthImage_
   ...其他资源...
   swapChain_.reset()        → ~SwapChain() → 销毁 imageViews + swapchainKHR  
   device.reset()            → ~Device()
   context.reset()           → ~VulkanContext() → 销毁 surface + instance
   ```
   `swapChain_` 必须在 `device` 之前销毁（swapchain 是 device 的子对象），在 `context` 之前销毁（swapchain 依赖 surface）。当前 `cleanup()` 中 `device.reset()` 和 `context.reset()` 在最后，只需确保 `swapChain_.reset()` 在它们前面即可。建议在 `cleanupSwapChain()` 之后、`device.reset()` 之前显式 `swapChain_.reset()`，或利用成员变量声明逆序析构（`swapChain_` 声明在 `device` 之后，析构会早于 `device`）。
