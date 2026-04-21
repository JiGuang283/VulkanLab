# Step 2：迁移 vulkan_utils.h 中的符号到新头文件

> **原则**：逐文件替换 `#include "vulkan_utils.h"`，改为 include 对应的细粒度头文件。每修改一个文件后立即编译验证。最终删除 `vulkan_utils.h`。

---

## 概述

`vulkan_utils.h` 当前被 **8 个文件** 直接 include：

| # | 文件 | 使用的符号 | 替换为 |
|---|------|-----------|--------|
| 1 | `core/VulkanContext.cpp` | `enableValidationLayers`, `validationLayers`, `CreateDebugUtilsMessengerEXT`, `DestroyDebugUtilsMessengerEXT`, GLFW 头 | 全部移入本文件匿名命名空间 |
| 2 | `core/Device.h` | `QueueFamilyIndices`, `SwapChainSupportDetails` | `#include "VulkanTypes.h"` |
| 3 | `core/Pipeline.cpp` | `Vertex::getBindingDescription()`, `Vertex::getAttributeDescriptions()` | `#include "render/Vertex.h"` |
| 4 | `render/Renderer.h` | `MAX_FRAMES_IN_FLIGHT` | 改为类内 `static constexpr` |
| 5 | `render/Renderer.cpp` | `MAX_FRAMES_IN_FLIGHT`, GLFW 头（**未使用**） | 通过 Renderer.h 获取；删除 GLFW include |
| 6 | `render/Mesh.cpp` | `Vertex` 类型 + hash | `#include "Vertex.h"` |
| 7 | `render/Material.cpp` | `MAX_FRAMES_IN_FLIGHT`, `UniformBufferObject`(`sizeof` 用) | 通过 Renderer.h 获取 `MAX_FRAMES_IN_FLIGHT`；`sizeof(UniformBufferObject)` 改为运行时从 Renderer 获取 |
| 8 | `app.h` | `UniformBufferObject`, `WIDTH`, `HEIGHT`, `MODEL_PATH`, `TEXTURE_PATH`, GLFW 键名 | 当前保留（Step 5 创建 Application 时一并处理） |

---

## 修改顺序与详细操作

### 2.1 `core/Device.h` — 替换类型来源

**改动**：移除 `#include "vulkan_utils.h"`，改为 `#include "VulkanTypes.h"`。

由于 `Device.h` 在 `vkr` namespace 内部使用 `QueueFamilyIndices` 和 `SwapChainSupportDetails`，而 `VulkanTypes.h` 中这些类型也在 `vkr` namespace 中，替换后类型查找自然匹配。

`Device.cpp` 不直接 include `vulkan_utils.h`，但通过 `Device.h` 间接使用了 `QueueFamilyIndices`、`SwapChainSupportDetails`、`enableValidationLayers`、`validationLayers`、`deviceExtensions`。

**问题**：`Device.cpp` 中的 `createLogicalDevice()` 使用了 `enableValidationLayers`、`validationLayers`、`deviceExtensions`。这些符号将在 VulkanContext.cpp 操作中被移入匿名命名空间，**不再全局可见**。

**解决方案**：在 `Device.cpp` 中也定义一份匿名命名空间的局部常量（`deviceExtensions` 和 `validationLayers`），或者通过接口从 VulkanContext 获取。最简单的做法是在 Device.cpp 内部定义：

```cpp
// Device.cpp 顶部
namespace {
    const bool enableValidationLayers = true;
    const std::vector<const char*> validationLayers = { "VK_LAYER_KHRONOS_validation" };
    const std::vector<const char*> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
}
```

**Device.h 改动后：**
```cpp
#pragma once

#include <optional>
#include <vector>
#include <vulkan/vulkan.h>

#include "VulkanContext.h"
#include "VulkanTypes.h"    // 替换 vulkan_utils.h
#include "vk_mem_alloc.h"

namespace vkr {
// ... 类定义不变 ...
} // namespace vkr
```

**Device.cpp 需新增的头部：**
```cpp
#include "Device.h"

#include <cstring>    // strcmp (checkValidationLayerSupport 若在此文件无则不需要)
#include <iostream>
#include <set>        // createLogicalDevice 中使用
#include <stdexcept>
#include <string>     // checkDeviceExtensionSupport 中使用

namespace {
const bool enableValidationLayers = true;
const std::vector<const char *> validationLayers = {
    "VK_LAYER_KHRONOS_validation"};
const std::vector<const char *> deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME};
} // namespace
```

> **注意**：`Device.cpp` 原本不直接 include `vulkan_utils.h`，而是通过 `Device.h` 间接获取。移除 Device.h 中的 `vulkan_utils.h` 后，Device.cpp 内部对 `std::set`、`enableValidationLayers` 等的使用需要补上直接 include 和局部定义。需查看 Device.cpp 顶部实际的 include 情况并补充。

---

### 2.2 `core/VulkanContext.cpp` — 内化私有符号

**改动**：移除 `#include "vulkan_utils.h"`，将以下符号搬入文件顶部匿名命名空间：

```cpp
// VulkanContext.cpp
#include "VulkanContext.h"

#include <GLFW/glfw3.h>   // glfwCreateWindowSurface, glfwGetRequiredInstanceExtensions

#include <cstring>         // strcmp
#include <iostream>        // std::cerr
#include <stdexcept>       // std::runtime_error
#include <vector>

namespace {

const bool enableValidationLayers = true;

const std::vector<const char *> validationLayers = {
    "VK_LAYER_KHRONOS_validation"};

VkResult CreateDebugUtilsMessengerEXT(
    VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkDebugUtilsMessengerEXT *pDebugMessenger) {
    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance, "vkCreateDebugUtilsMessengerEXT");
    if (func != nullptr) {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    } else {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
}

void DestroyDebugUtilsMessengerEXT(
    VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger,
    const VkAllocationCallbacks *pAllocator) {
    auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance, "vkDestroyDebugUtilsMessengerEXT");
    if (func != nullptr) {
        func(instance, debugMessenger, pAllocator);
    }
}

} // namespace

namespace vkr {
// ... 实现不变 ...
```

**注意**：`VulkanContext.cpp` 中有一个自由函数 `getRequiredExtensions()`（非成员）和一个成员函数 `VulkanContext::getRequiredExtensions()`，两者签名相同，注意不要冲突。查看代码发现自由函数是重复代码（已有成员版本），可直接删除自由函数。

---

### 2.3 `core/Pipeline.cpp` — 使用 Vertex.h

**改动**：移除 `#include "vulkan_utils.h"`，添加 `#include "render/Vertex.h"`。

Pipeline.cpp 使用了 `Vertex::getBindingDescription()` 和 `Vertex::getAttributeDescriptions()`。替换后需要用 `vkr::Vertex` 全限定名（Pipeline.cpp 已在 `namespace vkr {}` 内部，所以直接写 `Vertex` 即可，因为 `vkr::Vertex` 在同一命名空间中找得到）。

```cpp
#include "Pipeline.h"
#include "Device.h"
#include "render/Vertex.h"   // 替换 vulkan_utils.h

#include <fstream>
#include <stdexcept>
```

**无其他改动**，Pipeline.cpp 中的 `Vertex::getBindingDescription()` 等调用在 `namespace vkr` 内会自动匹配 `vkr::Vertex`。

---

### 2.4 `render/Renderer.h` — MAX_FRAMES_IN_FLIGHT 内聚

**改动**：移除 `#include "vulkan_utils.h"`，将 `MAX_FRAMES_IN_FLIGHT` 定义为类内常量。

```cpp
#pragma once

#include "core/Buffer.h"
#include "core/Device.h"
#include "core/Image.h"
#include "core/SwapChain.h"

#include <array>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

inline constexpr int MAX_FRAMES_IN_FLIGHT = 2;  // 从 vulkan_utils.h 移入

class Renderer {
  public:
    // ... 不变 ...
};

} // namespace vkr
```

将常量定义在 `namespace vkr` 内、`class Renderer` 之前，这样 `Material.cpp` 等其他 render 模块 include `Renderer.h` 后也可使用 `MAX_FRAMES_IN_FLIGHT`。

---

### 2.5 `render/Renderer.cpp` — 移除两个无用 include

**改动**：
1. 移除 `#include "vulkan_utils.h"`（`MAX_FRAMES_IN_FLIGHT` 已通过 `Renderer.h` 获取）
2. 移除 `#include <GLFW/glfw3.h>`（**零使用**）

```cpp
#include "Renderer.h"
#include "core/Device.h"
#include "core/SwapChain.h"

#include <array>
#include <stdexcept>
```

Renderer.cpp 中用到了 `QueueFamilyIndices`（在 `createCommandPool()` 中），此类型通过 `Device.h` → `VulkanTypes.h` 间接可达，无需额外 include。

---

### 2.6 `render/Mesh.cpp` — 使用 Vertex.h

**改动**：移除 `#include "vulkan_utils.h"`，添加 `#include "Vertex.h"`。

```cpp
#include "Mesh.h"
#include "Renderer.h"
#include "Vertex.h"          // 替换 vulkan_utils.h
#include "core/Device.h"

#include <tiny_obj_loader.h>

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
```

Mesh.cpp 在 `namespace vkr` 内使用 `Vertex`，替换后自动匹配 `vkr::Vertex`。`std::hash<vkr::Vertex>` 特化在 `Vertex.h` 中已定义，`std::unordered_map<Vertex, uint32_t>` 可正常工作。

---

### 2.7 `render/Material.cpp` — MAX_FRAMES_IN_FLIGHT + sizeof(UniformBufferObject)

**改动**：移除 `#include "vulkan_utils.h"`。

`MAX_FRAMES_IN_FLIGHT` 通过 `Renderer.h`（已在 Material.h 间接 include）获取。

**关键问题**：Material.cpp 中有 `sizeof(UniformBufferObject)` 用于 descriptor 的 bufferInfo.range。此处表示 UBO 的大小。

**解决方案**：Material 不应硬编码 UBO 的大小。改为从 Renderer 获取 `uniformBufferSize`（Renderer 构造时已知 UBO 大小）。在 Material.cpp 中替换为 `renderer_->uniformBufferSize()` 调用。

需在 `Renderer.h` 中新增访问器：
```cpp
VkDeviceSize uniformBufferSize() const { return uniformBufferSize_; }
```

Material.cpp 修改：
```cpp
bufferInfo.range = renderer_->uniformBufferSize();  // 替代 sizeof(UniformBufferObject)
```

完整的 Material.cpp 头部：
```cpp
#include "Material.h"

#include <array>
#include <stdexcept>
```

不再需要额外头文件——Material.h 已 include Pipeline.h、Device.h、Renderer.h、Texture.h。

---

### 2.8 `app.h` — 暂时保留

`app.h` 是当前应用入口，使用了 `UniformBufferObject`、`WIDTH`、`HEIGHT`、`MODEL_PATH`、`TEXTURE_PATH` 等所有应用层符号。

**本步骤暂不修改 `app.h`**——它将在 Step 5 被 `app/Application.h` 完全替代并删除。为避免 Step 2 改动过大，此处保留 `#include "vulkan_utils.h"`。

但为了能删除 `vulkan_utils.h`，需要调整策略：**先不删 vulkan_utils.h，仅将其精简为只服务 app.h 的最小版本**。等 Step 5 替换掉 app.h 后再最终删除。

精简后的 `vulkan_utils.h`（临时过渡版本）：
```cpp
#pragma once

// [过渡] 仅服务于旧 app.h / app.cpp，待 Step 5 Application 替代后删除

#define GLFW_INCLUDE_VULKAN
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/hash.hpp>

#include <cstdint>
#include <string>

// ---- 常量（仅 app.h/app.cpp 使用）----
const uint32_t WIDTH = 800;
const uint32_t HEIGHT = 600;

const std::string MODEL_PATH = "models/viking_room.obj";
const std::string TEXTURE_PATH = "textures/viking_room.png";

// ---- UBO 布局（仅 app.cpp 使用）----
struct UniformBufferObject {
    glm::mat4 view;
    glm::mat4 proj;
};
```

已移除的内容：
- `MAX_FRAMES_IN_FLIGHT` → `Renderer.h`
- `enableValidationLayers` / `validationLayers` / `deviceExtensions` → 各 .cpp 匿名命名空间
- `QueueFamilyIndices` / `SwapChainSupportDetails` → `VulkanTypes.h`
- `Vertex` / `hash<Vertex>` → `Vertex.h`
- `CreateDebugUtilsMessengerEXT` / `DestroyDebugUtilsMessengerEXT` → `VulkanContext.cpp`
- `stb_image.h` / `tiny_obj_loader.h` → 不再间接传播
- 大量标准库 include → 不再间接传播

---

## Renderer.h 新增访问器汇总

```cpp
// 新增
VkDeviceSize  uniformBufferSize() const { return uniformBufferSize_; }
```

---

## 逐文件修改顺序（推荐）

按依赖关系从底层到高层修改，每步编译验证：

```
① core/VulkanTypes.h         — Step 1 已创建，不需改动
② core/Device.h              — 替换 include
③ core/Device.cpp            — 添加匿名命名空间局部常量 + 补 include
④ core/VulkanContext.cpp     — 内化符号 + 删重复自由函数
⑤ core/Pipeline.cpp          — 替换 include 为 Vertex.h
    ↓ 编译验证 core 模块 ✓
⑥ render/Renderer.h          — 移入 MAX_FRAMES_IN_FLIGHT + 新增 uniformBufferSize()
⑦ render/Renderer.cpp        — 移除两个无用 include
⑧ render/Mesh.cpp            — 替换 include 为 Vertex.h
⑨ render/Material.cpp        — 移除 vulkan_utils.h，改用 renderer_->uniformBufferSize()
    ↓ 编译验证 render 模块 ✓
⑩ vulkan_utils.h             — 精简为仅服务 app.h 的过渡版本
    ↓ 全量编译验证 ✓
```

---

## 验收标准

| # | 检查项 | 验收方式 |
|---|--------|---------|
| 1 | `grep -r "vulkan_utils" src/core/ src/render/` 零命中 | grep |
| 2 | `grep -r "vulkan_utils" src/` 仅命中 `app.h`（过渡） | grep |
| 3 | `vulkan_utils.h` 已精简为仅含 app 层用常量和 UBO | 目视 |
| 4 | `Renderer.h` 包含 `MAX_FRAMES_IN_FLIGHT` 定义 | 目视 |
| 5 | `VulkanContext.cpp` / `Device.cpp` 中的 `enableValidationLayers` 等为局部定义 | 目视 |
| 6 | Debug + Release 编译通过 | 编译 |
| 7 | 渲染结果与修改前一致 | 运行 |

---

## 风险与注意事项

1. **Device.cpp 中 `std::set` 的 include**：现有 Device.cpp 通过 vulkan_utils.h 间接获得 `<set>`。移除后需显式 `#include <set>`，否则 `createLogicalDevice()` 中的 `std::set<uint32_t>` 编译失败。

2. **VulkanContext.cpp 中自由函数 `getRequiredExtensions()` 与成员函数重复**：代码中有一个非成员版本（第 32 行左右）和一个成员版本 `VulkanContext::getRequiredExtensions()`。非成员版本在 `createInstance()` 中被调用（`auto extensions = getRequiredExtensions()`），但由于 `createInstance()` 是成员函数，实际调用的是成员版本（名称查找优先找到成员）。应删除非成员版本以避免歧义。

3. **Pipeline.cpp 中 `Vertex` 命名空间**：Pipeline.cpp 在 `namespace vkr {}` 内，include `render/Vertex.h` 后 `vkr::Vertex` 直接以 `Vertex` 访问，无需修改调用代码。

4. **Material.cpp 中 `sizeof(UniformBufferObject)` 耦合**：此处的语义是"uniform buffer 有多大"，不应由 Material 硬编码。通过 `renderer_->uniformBufferSize()` 获取是正确解耦。

5. **SwapChain.cpp 不直接 include vulkan_utils.h**：它通过 `Device.h` 间接获取 `QueueFamilyIndices`。修改 Device.h 后，SwapChain.cpp 需确认 `QueueFamilyIndices` 仍可达——通过 `Device.h` → `VulkanTypes.h` 链确保可达。

6. **`#include` 路径**：`Pipeline.cpp` 在 `src/core/` 目录，include `render/Vertex.h` 时路径为 `"render/Vertex.h"`（相对于 src/）。CMake 中 `target_include_directories` 已包含 `src/`，可正常解析。
