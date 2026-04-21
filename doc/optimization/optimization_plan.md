# 后续优化与重构计划

> 阶段 0–4.2 已全部完成。本文档梳理当前代码库中 **仍存在的技术债务** 与 **可做的优化**，按优先级排列。

---

## 当前状态概览

| 维度 | 状态 |
|------|------|
| RAII 封装 | ✅ Buffer / Image / Pipeline / VulkanContext / Device / SwapChain / Renderer 均有 RAII |
| 场景管理 | ✅ Scene / SceneObject / Camera / Push Constants |
| 窗口输入 | ✅ Window / InputManager 封装 |
| VMA | ⚠️ `VmaAllocator` 已创建于 `Device`，但 **Buffer / Image 仍使用 `vkAllocateMemory`** |
| vulkan_utils.h | ❌ 仍是"万能头文件"：全局常量、GLFW include、Vertex 定义、UBO 定义、Debug 代理函数全集中于此 |
| GLFW 隔离 | ⚠️ Window/InputManager 已封装，但 `VulkanContext`、`SwapChain` 仍直接持有 `GLFWwindow*`；`Renderer.cpp` 残留无用 `#include <GLFW/glfw3.h>`；`app.cpp` 使用 `GLFW_KEY_*` 常量 |
| 同步对象 | ⚠️ Semaphore / Fence 功能正确，手动在 Renderer 析构中销毁，未做 RAII 封装 |
| 错误处理 | ⚠️ 每处 `if (vk... != VK_SUCCESS) throw`，无统一 VK_CHECK 宏 |
| 资源管理 | ❌ 纹理 / 模型 / Shader 无缓存，全部 ad-hoc 加载 |
| Descriptor 管理 | ⚠️ 每个 Material 独立创建 DescriptorPool，多材质时池碎片化 |

---

## 优化项目（按优先级排列）

### P0：VMA 真正启用（Buffer / Image 迁移）

**问题**：`Device::createAllocator()` 已创建 `VmaAllocator`，`Device::allocator()` 可访问，但 `Buffer` 和 `Image` 仍使用 `vkAllocateMemory` / `vkFreeMemory` / `vkBindBufferMemory`。VMA 形同虚设。

**改动范围**：

#### Buffer.h / Buffer.cpp
```
替换前：
  VkDeviceMemory memory_;
  vkAllocateMemory → vkBindBufferMemory

替换后：
  VmaAllocation allocation_;
  vmaCreateBuffer()  → 同时创建 buffer + 分配内存
  vmaDestroyBuffer() → 同时释放
  vmaMapMemory() / vmaUnmapMemory()
```

- `Buffer` 构造函数接收 `VmaAllocator` 或 `Device&`（调 `device.allocator()`）
- 移除 `memory()` 访问器（外部不再需要 `VkDeviceMemory`）
- `findMemoryType()` 不再需要暴露给 Buffer

#### Image.h / Image.cpp
```
替换前：
  VkDeviceMemory memory_;
  vkAllocateMemory → vkBindImageMemory

替换后：
  VmaAllocation allocation_;
  vmaCreateImage()  → 同时创建 image + 分配内存
  vmaDestroyImage() → 同时释放
```

- 同理移除 `memory()` 访问器
- Texture 内部的 Image 自然跟着迁移

#### 额外清理
- `Device::findMemoryType()` 在完全迁移后可标记 `[[deprecated]]` 或直接移除
- 确认 `vk_mem_alloc.cpp` 中的 `VMA_IMPLEMENTATION` 仍正确链接

#### 验收标准
- 全局搜索 `vkAllocateMemory` / `vkFreeMemory` 应返回 **零结果**
- 验证层无报错，渲染结果不变

---

### P1：拆分 vulkan_utils.h

**问题**：`vulkan_utils.h` 是整个项目最大的耦合源—— 几乎所有 .cpp 都 include 它，它又 include 了 GLFW、GLM、stb_image、tiny_obj_loader 等大量头文件。

**拆分方案**：

| 内容 | 目标位置 | 说明 |
|------|----------|------|
| `WIDTH`, `HEIGHT`, `MODEL_PATH`, `TEXTURE_PATH` | `Config.h`（新建，`src/` 根目录或 `src/app/`） | 集中管理可配置常量 |
| `MAX_FRAMES_IN_FLIGHT` | `Config.h` 或 `Renderer.h` 内部 | Renderer 独占使用 |
| `enableValidationLayers`, `validationLayers`, `deviceExtensions` | `VulkanContext.cpp` 内部（匿名命名空间） | 仅 VulkanContext 使用 |
| `QueueFamilyIndices`, `SwapChainSupportDetails` | `core/VulkanTypes.h`（新建） | Device / SwapChain 共用 |
| `Vertex` + hash 特化 | `render/Vertex.h`（新建） | Mesh 使用 |
| `UniformBufferObject` | `render/UniformData.h`（新建）或就近放 `app.h` | 仅 App 的 UBO 布局 |
| `CreateDebugUtilsMessengerEXT` / `DestroyDebugUtilsMessengerEXT` | `VulkanContext.cpp` 内部 | 仅 VulkanContext 使用 |
| GLFW / GLM / stb / tiny_obj includes | 各使用者自行 include | 不再由 vulkan_utils.h 间接传播 |

**最终目标**：`vulkan_utils.h` 文件 **删除**，所有依赖它的 .cpp/.h 改为 include 对应的细粒度头文件。

#### 验收标准
- `vulkan_utils.h` 不存在
- 每个 .cpp 仅 include 自己需要的头文件
- 编译通过，渲染结果不变

---

### P2：GLFW 依赖进一步隔离

**现状**：

| 文件 | GLFW 使用方式 | 是否合理 |
|------|--------------|---------|
| `Window.h/cpp` | GLFW 窗口管理 | ✅ 正确位置 |
| `InputManager.h/cpp` | `glfwGetKey`, `glfwSetCursorPosCallback` | ✅ 正确位置 |
| `VulkanContext.h/cpp` | 持有 `GLFWwindow*`，调 `glfwCreateWindowSurface` / `glfwGetRequiredInstanceExtensions` | ⚠️ 可优化 |
| `SwapChain.h/cpp` | 持有 `GLFWwindow*`，调 `glfwGetFramebufferSize` | ⚠️ 可优化 |
| `Renderer.cpp` | `#include <GLFW/glfw3.h>`（**未使用**） | ❌ 删除 |
| `app.cpp` | 使用 `GLFW_KEY_W` 等常量 | ⚠️ 可通过 InputManager 抽象 |

**改动方案**：

1. **Renderer.cpp**：直接删除 `#include <GLFW/glfw3.h>`（0 调用）
2. **VulkanContext**：构造参数改为接收 `VkSurfaceKHR`（由 Window 创建）+ `std::vector<const char*> extensions`（由 Window 提供），不再持有 `GLFWwindow*`
3. **SwapChain**：构造参数改为接收一个获取窗口尺寸的回调 `std::function<VkExtent2D()>`，或者由 Window 类提供 `framebufferSize()` 并在外部传入
4. **app.cpp**：InputManager 提供键名枚举或常量转发，使 app.cpp 不直接引用 `GLFW_KEY_*`

#### 验收标准
- `grep -r "GLFW\|glfw" src/` 仅命中 `src/window/` 目录下的文件
- 编译通过，渲染结果不变

---

### P3：统一错误处理（VK_CHECK 宏）

**问题**：代码中有 30+ 处 `if (vkXxx(...) != VK_SUCCESS) throw std::runtime_error("...");` 模式，冗长且错误信息不包含位置。

**方案**：新建 `src/core/VulkanCheck.h`：

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <stdexcept>
#include <string>

#define VK_CHECK(expr)                                              \
    do {                                                            \
        VkResult _result = (expr);                                  \
        if (_result != VK_SUCCESS) {                                \
            throw std::runtime_error(                               \
                std::string("Vulkan error ") +                      \
                std::to_string(static_cast<int>(_result)) +         \
                " at " __FILE__ ":" + std::to_string(__LINE__));    \
        }                                                           \
    } while (0)
```

**使用示例**：
```cpp
// 替换前
if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer_) != VK_SUCCESS)
    throw std::runtime_error("failed to create buffer!");

// 替换后
VK_CHECK(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer_));
```

#### 验收标准
- 所有 `vkCreate*` / `vkAllocate*` / `vkBegin*` 调用统一使用 `VK_CHECK`
- 异常消息包含文件名和行号

---

### P4：同步对象 RAII 封装（可选）

**现状**：`Renderer::FrameData` 中的 `VkSemaphore` / `VkFence` 在 `Renderer` 析构中手动逐个 `vkDestroy*`。功能正确，但违反"RAII 优先"原则。

**方案 A（轻量）**：FrameData 析构自动销毁
```cpp
struct FrameData {
    // ... 现有成员 ...
    Device* device = nullptr;
    ~FrameData() {
        if (device) {
            VkDevice d = device->logicalDevice();
            vkDestroySemaphore(d, imageAvailable, nullptr);
            vkDestroySemaphore(d, renderFinished, nullptr);
            vkDestroyFence(d, inFlight, nullptr);
        }
    }
};
```

**方案 B（完整 RAII）**：新建 `Semaphore` / `Fence` 类
```cpp
class Semaphore {
public:
    Semaphore(Device& device);
    ~Semaphore(); // vkDestroySemaphore
    VkSemaphore handle() const;
};
```

**推荐**：方案 A 即可，当前 FrameData 仅在 Renderer 内部使用，无需暴露独立类。

---

### P5：Config 系统

**问题**：窗口尺寸、模型路径、纹理路径、shader 路径均为 `vulkan_utils.h` 中的全局常量，无法运行时修改。

**方案**：新建 `src/Config.h`

```cpp
#pragma once
#include <string>
#include <cstdint>

namespace vkr {

struct Config {
    uint32_t    windowWidth     = 800;
    uint32_t    windowHeight    = 600;
    std::string windowTitle     = "Vulkan";
    std::string modelPath       = "models/viking_room.obj";
    std::string texturePath     = "textures/viking_room.png";
    std::string vertShaderPath  = "shader/vert.spv";
    std::string fragShaderPath  = "shader/frag.spv";
    bool        enableValidation = true;
};

} // namespace vkr
```

- `App::run()` 接受 `Config` 参数
- 未来可扩展为从 JSON / TOML 文件加载

---

### P6：InputManager 键名抽象

**问题**：`app.cpp` 直接使用 `GLFW_KEY_W` / `GLFW_KEY_ESCAPE` 等宏，这要求 app.cpp include GLFW 头文件（尽管当前通过 `vulkan_utils.h` 间接获得）。

**方案**：在 `InputManager.h` 中定义键名枚举或转发常量：

```cpp
namespace vkr {
    enum class Key : int {
        W = 87, A = 65, S = 83, D = 68,
        Space = 32, LeftShift = 340, Escape = 256,
        // ...
    };
}
```

- `InputManager::isKeyDown(Key key)` ，app.cpp 使用 `vkr::Key::W` 代替 `GLFW_KEY_W`
- 彻底消除 app 层对 GLFW 的依赖

---

### P7：单次命令提交优化

**问题**：`Renderer::endSingleTimeCommands()` 使用 `vkQueueWaitIdle()` 强制 GPU 全面停顿，在纹理加载、buffer 上传等密集操作时性能损失显著。

**方案**：使用 Fence 做精确等待：

```cpp
void Renderer::endSingleTimeCommands(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    vkCreateFence(device_->logicalDevice(), &fenceInfo, nullptr, &fence);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(device_->graphicsQueue(), 1, &submitInfo, fence);
    vkWaitForFences(device_->logicalDevice(), 1, &fence, VK_TRUE, UINT64_MAX);

    vkDestroyFence(device_->logicalDevice(), fence, nullptr);
    vkFreeCommandBuffers(device_->logicalDevice(), commandPool_, 1, &cmd);
}
```

更进一步可引入 **Transfer Queue**（如设备支持独立传输队列），实现异步上传。

---

### P8：Viewport / Scissor 封装

**问题**：`app.cpp` 的 `mainLoop()` 手动设置 viewport 和 scissor（约 10 行），这是渲染流程的通用环节，不该留在应用层。

**方案**：将其移入 `Renderer::beginRenderPass()` 末尾，或提供 `Renderer::setFullViewport(cmd)` 辅助方法。

---

### P9：Descriptor Pool 共享（长期）

**问题**：当前每个 `Material` 独立创建 `VkDescriptorPool`（maxSets = MAX_FRAMES_IN_FLIGHT）。多材质场景下，pool 数量线性增长。

**方案**：引入 `DescriptorAllocator` 类：
- 维护一组可扩展的 `VkDescriptorPool`
- 当前 pool 满时自动创建新 pool
- Material 从全局 allocator 分配 set，不再各自拥有 pool

**优先级较低**，当前仅一个材质时无性能问题。

---

### P10：资源管理器 / 缓存（长期）

**问题**：同一纹理 / 模型被多个对象引用时，当前会重复加载。

**方案**：
```cpp
class ResourceManager {
public:
    std::shared_ptr<Texture> loadTexture(const std::string& path);
    std::shared_ptr<Mesh>    loadMesh(const std::string& path);
private:
    std::unordered_map<std::string, std::weak_ptr<Texture>> textureCache_;
    std::unordered_map<std::string, std::weak_ptr<Mesh>>    meshCache_;
};
```

- 使用 `weak_ptr` 避免 ResourceManager 强持有已废弃资源
- 加载时先检查缓存

---

## 推荐执行顺序

```
P0 VMA 启用          ──── 最高优先级，消除技术债
  │
  ▼
P1 拆分 vulkan_utils.h ── 解耦头文件依赖
  │
  ├─► P3 VK_CHECK 宏     （拆分时顺便加入）
  └─► P5 Config 系统      （常量迁移时自然完成）
  │
  ▼
P2 GLFW 隔离          ── 配合 P1
  │
  ├─► P6 键名抽象        （顺便完成）
  └─► P4 FrameData RAII  （小改动）
  │
  ▼
P7 单次命令优化        ── 性能改善
  │
  ▼
P8 Viewport 封装       ── 清理 app 层
  │
  ▼
P9 Descriptor 池共享   ── 多材质前完成
  │
  ▼
P10 资源管理器         ── 多物体场景前完成
```

---

## 每项验收标准汇总

| 项目 | 验收标准 |
|------|----------|
| P0 | `vkAllocateMemory` / `vkFreeMemory` 全局零出现；VMA 承接所有分配 |
| P1 | `vulkan_utils.h` 文件删除；编译通过 |
| P2 | `grep -r "GLFW" src/` 仅命中 `src/window/` |
| P3 | 所有 Vulkan 调用使用 `VK_CHECK`；异常含文件名+行号 |
| P4 | Renderer 析构无手动 `vkDestroy` 同步对象 |
| P5 | 常量全部来自 `Config` 对象 |
| P6 | `app.cpp` 不含 `GLFW_KEY_*` 字符串 |
| P7 | 单次命令不再调用 `vkQueueWaitIdle` |
| P8 | `app.cpp` 无 `vkCmdSetViewport` / `vkCmdSetScissor` |
| P9 | 全局 DescriptorAllocator 管理所有 descriptor 分配 |
| P10 | 相同资源路径只加载一次 |
