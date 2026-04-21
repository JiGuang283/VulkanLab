# Step 1：新建基础头文件（详细方案）

> **原则**：本步骤只新建文件，**不修改**任何现有代码。新建完成后现有项目必须仍可正常编译运行。

---

## 概述

新建 5 个头文件，将 `vulkan_utils.h` 中的类型定义、全局常量、UBO 布局提前"复制"到各自的目标位置。此阶段新旧并存，`vulkan_utils.h` 保持不变。

| 序号 | 新文件 | 内容来源 | 说明 |
|------|--------|---------|------|
| 1 | `src/core/VulkanCheck.h` | 全新 | `VK_CHECK` 宏 |
| 2 | `src/core/VulkanTypes.h` | `vulkan_utils.h` 中的 `QueueFamilyIndices` + `SwapChainSupportDetails` | core 模块公共类型 |
| 3 | `src/render/Vertex.h` | `vulkan_utils.h` 中的 `Vertex` + `std::hash<Vertex>` | render 模块使用 |
| 4 | `src/app/Config.h` | 全新（部分值取自 `vulkan_utils.h` 中的全局常量） | 运行时配置 |
| 5 | `src/app/UniformData.h` | `vulkan_utils.h` 中的 `UniformBufferObject` | 应用层 UBO 布局 |

---

## 1. `src/core/VulkanCheck.h`

### 用途
统一 Vulkan API 调用的错误检查，替代后续 Step 3 中散落在各处的 `if (vkXxx(...) != VK_SUCCESS) throw` 模式。

### 完整内容

```cpp
#pragma once

#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>

#define VK_CHECK(expr)                                                         \
    do {                                                                       \
        VkResult _vk_result = (expr);                                          \
        if (_vk_result != VK_SUCCESS) {                                        \
            throw std::runtime_error(                                          \
                std::string("Vulkan error ") +                                 \
                std::to_string(static_cast<int>(_vk_result)) + " at " +        \
                __FILE__ + ":" + std::to_string(__LINE__));                     \
        }                                                                      \
    } while (0)
```

### 依赖
- `<vulkan/vulkan.h>`（`VkResult`, `VK_SUCCESS`）
- `<stdexcept>`（`std::runtime_error`）
- `<string>`（`std::string`, `std::to_string`）

### 注意事项
- 变量名 `_vk_result` 带下划线前缀，避免与外部变量名冲突（单下划线 + 小写在用户代码中合法）。
- `do { ... } while(0)` 保证宏在 if/else 中安全使用。
- `__FILE__` 和 `__LINE__` 在预处理时展开，指向调用处。

---

## 2. `src/core/VulkanTypes.h`

### 用途
提供 `QueueFamilyIndices` 和 `SwapChainSupportDetails` 两个结构体，当前被 `Device.h`、`Device.cpp`、`SwapChain.cpp` 使用。

### 完整内容

```cpp
#pragma once

#include <vulkan/vulkan.h>

#include <optional>
#include <vector>

namespace vkr {

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool isComplete() const {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR        capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR>   presentModes;
};

} // namespace vkr
```

### 与现有代码的差异

| 差异 | 原始 (`vulkan_utils.h`) | 新版 (`VulkanTypes.h`) |
|------|------------------------|----------------------|
| 命名空间 | 全局 | `vkr` |
| `isComplete()` | 非 const | `const`（修复） |
| `capabilities` | 未初始化 | 值初始化 `{}` |

### 注意事项
- 当前 `vulkan_utils.h` 中的 `QueueFamilyIndices` 在全局命名空间，而 `Device.h` 已在 `vkr` namespace 内使用它。这在现有代码中可行是因为 `vulkan_utils.h` 在 namespace 外部被 include。
- **Step 1 阶段不改现有代码**，因此 `VulkanTypes.h` 的 `vkr::QueueFamilyIndices` 暂时不会被任何人使用，与全局的 `QueueFamilyIndices` 并存。到 Step 2 再切换。

---

## 3. `src/render/Vertex.h`

### 用途
提供 `Vertex` 结构体及其 hash 特化。当前使用者：
- `Pipeline.cpp`：读取 `Vertex::getBindingDescription()` / `getAttributeDescriptions()` 来配置顶点输入。
- `Mesh.cpp`：在 `fromOBJ()` 中按 `Vertex` 结构体组装顶点数据并做去重。

### 完整内容

```cpp
#pragma once

#define GLM_ENABLE_EXPERIMENTAL

#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>     // offsetof
#include <functional>  // std::hash

namespace vkr {

struct Vertex {
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec2 texCoord;

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding   = 0;
        bindingDescription.stride    = sizeof(Vertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return bindingDescription;
    }

    static std::array<VkVertexInputAttributeDescription, 3>
    getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 3> attrs{};

        attrs[0].binding  = 0;
        attrs[0].location = 0;
        attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[0].offset   = offsetof(Vertex, pos);

        attrs[1].binding  = 0;
        attrs[1].location = 1;
        attrs[1].format   = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[1].offset   = offsetof(Vertex, color);

        attrs[2].binding  = 0;
        attrs[2].location = 2;
        attrs[2].format   = VK_FORMAT_R32G32_SFLOAT;
        attrs[2].offset   = offsetof(Vertex, texCoord);

        return attrs;
    }

    bool operator==(const Vertex &other) const {
        return pos == other.pos && color == other.color &&
               texCoord == other.texCoord;
    }
};

} // namespace vkr

// hash 特化必须在全局 std 命名空间中
namespace std {
template <> struct hash<vkr::Vertex> {
    size_t operator()(vkr::Vertex const &vertex) const {
        return ((hash<glm::vec3>()(vertex.pos) ^
                 (hash<glm::vec3>()(vertex.color) << 1)) >>
                1) ^
               (hash<glm::vec2>()(vertex.texCoord) << 1);
    }
};
} // namespace std
```

### 与现有代码的差异

| 差异 | 原始 | 新版 |
|------|------|------|
| 命名空间 | 全局 | `vkr::Vertex` |
| hash 特化 | `hash<Vertex>` | `hash<vkr::Vertex>` |
| `GLM_ENABLE_EXPERIMENTAL` | 在 vulkan_utils.h 顶部统一定义 | 在本文件自行定义（`glm/gtx/hash.hpp` 需要） |

### 注意事项
- `GLM_ENABLE_EXPERIMENTAL` 必须在 `#include <glm/gtx/hash.hpp>` 之前定义，否则编译报错。
- `GLM_FORCE_DEPTH_ZERO_TO_ONE` 不在此处定义——它只影响 `glm::perspective` 等投影函数，仅 Camera 使用。Camera.cpp 或一个统一的 GLM 配置头中处理更合适。
- Step 1 阶段 `vkr::Vertex` 与全局 `Vertex` 并存，不冲突。

---

## 4. `src/app/Config.h`

### 用途
以结构体形式集中管理当前散落在 `vulkan_utils.h` 中的全局常量（窗口尺寸、资源路径等）。

### 完整内容

```cpp
#pragma once

#include <cstdint>
#include <string>

namespace vkr {

struct Config {
    // ---- 窗口 ----
    uint32_t    windowWidth      = 800;
    uint32_t    windowHeight     = 600;
    std::string windowTitle      = "Vulkan Renderer";

    // ---- 资源路径 ----
    std::string modelPath        = "models/viking_room.obj";
    std::string texturePath      = "textures/viking_room.png";
    std::string vertShaderPath   = "shader/vert.spv";
    std::string fragShaderPath   = "shader/frag.spv";

    // ---- 渲染设置 ----
    bool        enableValidation = true;

    // ---- 输入参数 ----
    float       moveSpeed        = 2.0f;
    float       mouseSensitivity = 0.1f;
};

} // namespace vkr
```

### 来源映射

| Config 字段 | 原始全局常量 |
|-------------|-------------|
| `windowWidth` | `WIDTH` (800) |
| `windowHeight` | `HEIGHT` (600) |
| `modelPath` | `MODEL_PATH` |
| `texturePath` | `TEXTURE_PATH` |
| `enableValidation` | `enableValidationLayers` |
| `moveSpeed` | `app.cpp` 中硬编码的 `2.0f` |
| `mouseSensitivity` | `app.cpp` 中硬编码的 `0.1f` |

### 注意事项
- `MAX_FRAMES_IN_FLIGHT` 不放入 Config——它是 Renderer 的内部实现细节而非用户配置项。Step 2 时将其移为 `Renderer.h` 中的 `static constexpr`。
- `validationLayers` / `deviceExtensions` 也不放入 Config——它们是 VulkanContext 的内部实现细节。Step 2 时移入 `VulkanContext.cpp` 匿名命名空间。
- 默认值与当前行为完全一致，确保不改变运行结果。

---

## 5. `src/app/UniformData.h`

### 用途
定义应用层的 UBO 布局。当前 `UniformBufferObject` 位于 `vulkan_utils.h`，仅被 `app.cpp` 的 `updateUniformBuffer()` 使用。

### 完整内容

```cpp
#pragma once

#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <glm/glm.hpp>

namespace vkr {

struct GlobalUBO {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
};

} // namespace vkr
```

### 与现有代码的差异

| 差异 | 原始 | 新版 |
|------|------|------|
| 名称 | `UniformBufferObject` | `vkr::GlobalUBO`（更清晰，且与后续 per-object UBO 区分） |
| 命名空间 | 全局 | `vkr` |
| `alignas(16)` | 无（当前 `glm::mat4` 自然 16 字节对齐，但显式标注更安全） | 有 |
| `GLM_FORCE_DEPTH_ZERO_TO_ONE` | 在 vulkan_utils.h 统一定义 | 在本文件定义（确保 Vulkan 深度范围 [0,1]） |

### 注意事项
- `GLM_FORCE_DEPTH_ZERO_TO_ONE` 影响 `glm::perspective` 生成的投影矩阵，必须在所有使用 GLM 投影函数的地方定义。但此文件本身不调用投影函数，这个宏在此定义主要是为了文档性（表明这个 UBO 接收 Vulkan 风格的投影矩阵）。实际编译时，Camera.cpp 需要此宏——这将在 Step 2 的头文件迁移中一并处理。

---

## CMakeLists.txt 注意事项

如果当前 `CMakeLists.txt` 使用 `file(GLOB ...)` 或 `aux_source_directory` 扫描 `src/` 的 .cpp 文件：
- 本步骤只新建 `.h` 文件，无新 `.cpp`，**不需要修改 CMakeLists.txt**。

如果使用显式文件列表：
- 纯头文件通常不需要加入 `add_executable()`，但建议加入以便 IDE 能索引到。这一操作优先级低，可延迟到 Step 5 创建 `Application.cpp` 时一起处理。

需确认 `include_directories` 中包含 `${CMAKE_SOURCE_DIR}/src`，以支持 `#include "core/VulkanTypes.h"` 等路径形式。

---

## 验收清单

| # | 检查项 | 验收方式 |
|---|--------|---------|
| 1 | 5 个新文件已创建且内容正确 | 文件存在性检查 |
| 2 | 新文件各自可独立编译（无语法错误） | 在任意 .cpp 中临时 `#include` 新文件并编译 |
| 3 | 现有代码完全不受影响 | Debug / Release 编译通过 |
| 4 | 渲染结果与新建前完全一致 | 运行验证 |
| 5 | 新文件中的类型/宏名称不与现有全局名称冲突 | `VulkanTypes.h` 的类型在 `vkr` 命名空间中，与全局 `QueueFamilyIndices` 等不冲突；`Vertex.h` 中的 `vkr::Vertex` 与全局 `Vertex` 不冲突；`VK_CHECK` 宏全局唯一 |

---

## 新文件与后续 Step 的衔接

```
Step 1 (本步骤)                    Step 2 (下一步)
──────────────                    ──────────────
core/VulkanCheck.h  ──────────►  各 .cpp 中 #include 并替换 if-throw (Step 3)
core/VulkanTypes.h  ──────────►  Device.h/SwapChain.cpp 改为 #include "VulkanTypes.h"
render/Vertex.h     ──────────►  Pipeline.cpp/Mesh.cpp 改为 #include "Vertex.h"
app/Config.h        ──────────►  Application.cpp 使用 Config 替代全局常量 (Step 5)
app/UniformData.h   ──────────►  Application.cpp 使用 GlobalUBO 替代 UniformBufferObject (Step 5)
```
