# Phase A 工程地基实施方案

## 0. 目标

Phase A 的目标是先把后续架构演进需要的“工程底座”放进去，并且不改变当前画面、不改变主渲染路径、不提前引入 Pass / RenderGraph / SceneGraph。

本阶段完成后，项目应具备：

1. 统一日志入口，替换零散 `std::cout` / `std::cerr` / `fprintf(stderr, ...)`。
2. 更可读的 Vulkan 错误异常，`VK_CHECK` 能输出 VkResult 名称、文件、行号和表达式。
3. 通用 `Handle<T>` / `ResourcePool<T>`，先能自测，不大规模替换资源引用。
4. 统一 descriptor pool pages 分配入口，为后续拆 `Material -> Renderer` 依赖做准备。
5. `PipelineConfigBuilder`，让 pipeline 配置构造集中化，减少在 Material 中 mutation config 的范围。

判断原则：Phase A 只铺路，不重写 renderer。

---

## 1. 当前切入点

| 切入点 | 当前代码 | Phase A 处理 |
|---|---|---|
| spdlog include | `CMakeLists.txt` 未包含 `external`，也未定义 `SPDLOG_HEADER_ONLY` | 加入 target include 和 compile definitions。 |
| 项目输出 | `Device.cpp`、`Mesh.cpp`、`Application.cpp`、`main.cpp` 等直接输出终端 | 增加 `core/Log.*`，逐步替换。 |
| validation 输出 | `VulkanContext.cpp::debugCallback` 直接 `std::cerr << "validation layer:"` | 改用 `[Vulkan][Validation]` logger，按 severity 映射。 |
| `VK_CHECK` | `core/VulkanCheck.h` 只抛 `Vulkan error <int>` | 增加 `VulkanException`、`vkResultName()`、表达式字符串。 |
| descriptor pool | `Material` 每个实例创建一个 pool | 新增 `DescriptorAllocator`，先让 `Material` 可选接入或为 Phase B 准备。 |
| pipeline 配置 | `PipelineConfig` 是可变 struct，`Material` append descriptor layout | 新增 builder，保留旧 struct 兼容。 |
| 资源生命周期 | `Scene` 用 `shared_ptr<Texture/Material/Mesh>` | 新增 handle/pool，自测通过后再迁移。 |

---

## 2. 实施顺序

推荐拆成 5 个小提交或 5 个可单独验证的工作块。

```text
A1 Log + CMake
  -> A2 VulkanException + VK_CHECK
      -> A3 ResourceHandle + ResourcePool
          -> A4 DescriptorAllocator
              -> A5 PipelineConfigBuilder
```

每一步都必须能单独编译并运行现有场景。

---

## 3. A1：日志系统落地

### 3.1 修改 CMake

修改 `CMakeLists.txt`：

```cmake
set(SPDLOG_ROOT "${CMAKE_SOURCE_DIR}/external")

target_include_directories(VulkanLab PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${GLM_INCLUDE_DIR}
    ${SPDLOG_ROOT}
)

target_compile_definitions(VulkanLab PRIVATE
    SPDLOG_HEADER_ONLY
    $<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
    $<$<NOT:$<CONFIG:Debug>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>
)
```

注意：`external/spdlog/spdlog.h` 内部包含路径是 `<spdlog/...>`，所以 include root 必须是 `external`，不是 `external/spdlog`。

### 3.2 新增文件

```text
src/core/Log.h
src/core/Log.cpp
```

`Log.h` 建议接口：

```cpp
#pragma once

#include <memory>
#include <string_view>

#include <spdlog/logger.h>

namespace vkr::log {

enum class Level {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off,
};

struct Settings {
    Level consoleLevel = Level::Info;
    Level fileLevel = Level::Trace;
    bool enableFile = true;
    bool enableColor = true;
    std::string_view filePath = "logs/VulkanLab.log";
};

void init(const Settings& settings = {});
void shutdown();
void setConsoleLevel(Level level);

std::shared_ptr<spdlog::logger> logger(std::string_view tag);

} // namespace vkr::log

#define VKR_LOG_TRACE(tag, ...) \
    SPDLOG_LOGGER_TRACE(::vkr::log::logger(tag), __VA_ARGS__)
#define VKR_LOG_DEBUG(tag, ...) \
    SPDLOG_LOGGER_DEBUG(::vkr::log::logger(tag), __VA_ARGS__)
#define VKR_LOG_INFO(tag, ...) \
    SPDLOG_LOGGER_INFO(::vkr::log::logger(tag), __VA_ARGS__)
#define VKR_LOG_WARN(tag, ...) \
    SPDLOG_LOGGER_WARN(::vkr::log::logger(tag), __VA_ARGS__)
#define VKR_LOG_ERROR(tag, ...) \
    SPDLOG_LOGGER_ERROR(::vkr::log::logger(tag), __VA_ARGS__)
#define VKR_LOG_CRITICAL(tag, ...) \
    SPDLOG_LOGGER_CRITICAL(::vkr::log::logger(tag), __VA_ARGS__)
```

`Log.cpp` 内部实现要点：

- 使用 `stderr_color_sink_mt` 或 `stderr_sink_mt` 作为控制台 sink。
- 使用 `rotating_file_sink_mt` 写 `logs/VulkanLab.log`。
- 所有模块 logger 共用 sinks。
- 用 `std::unordered_map<std::string, std::shared_ptr<spdlog::logger>>` 缓存 tag logger。
- `init()` 必须幂等，重复调用不重复注册 sinks。
- `shutdown()` 调用 `spdlog::shutdown()`。

建议默认 pattern：

```text
console: %^[%H:%M:%S.%e] [%l] [%n] %v%$
file:    [%Y-%m-%d %H:%M:%S.%e] [%l] [%n] [%s:%# %!] %v
```

### 3.3 main 初始化顺序

修改 `main.cpp`：

```cpp
int main() {
    vkr::log::init();
    try {
        ...
    } catch (const std::exception& e) {
        VKR_LOG_CRITICAL("App", "{}", e.what());
        vkr::log::shutdown();
        return EXIT_FAILURE;
    }
    vkr::log::shutdown();
    return EXIT_SUCCESS;
}
```

注意：如果要清理 `VK_LOADER_DEBUG`，必须在任何 Vulkan API 调用前做。可在 `main()` 中 `log::init()` 前后都可以，但必须早于 `VulkanContext` 创建。

### 3.4 第一批替换点

优先替换最吵的输出：

| 文件 | 替换内容 | logger tag | 建议级别 |
|---|---|---|---|
| `src/main.cpp` | `std::cerr << e.what()` | `App` | `critical` |
| `src/core/Device.cpp` | GPU 选择信息 | `Device` | `info` |
| `src/app/Application.cpp` | 场景切换 | `Scene` | `info` |
| `src/render/Mesh.cpp` | vertices/indices 统计 | `Mesh` | `debug` 或 `info` |
| `src/render/GltfLoader.cpp` | 图片解码降级/异常前提示 | `Gltf` | `warn` |
| `src/core/VulkanContext.cpp` | validation callback | `Vulkan` | 按 severity |

### 3.5 Vulkan validation severity 映射

```cpp
if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    VKR_LOG_ERROR("Vulkan", "[Validation] {}", pCallbackData->pMessage);
} else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    VKR_LOG_WARN("Vulkan", "[Validation] {}", pCallbackData->pMessage);
} else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
    VKR_LOG_DEBUG("Vulkan", "[Validation] {}", pCallbackData->pMessage);
} else {
    VKR_LOG_TRACE("Vulkan", "[Validation] {}", pCallbackData->pMessage);
}
```

同时建议把 `populateDebugMessengerCreateInfo()` 默认订阅改为：

```cpp
createInfo.messageSeverity =
    VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
```

verbose 可后续通过配置打开，不作为默认控制台输出。

### 3.6 A1 验收

- 控制台不再出现项目自己的裸 `std::cout` 分隔线。
- 运行后生成 `logs/VulkanLab.log`。
- validation warning/error 有 `[Vulkan]` tag。
- `run_stdout.log` / terminal 中项目日志格式统一。

---

## 4. A2：VulkanException 与增强 VK_CHECK

### 4.1 新增文件

```text
src/core/VulkanException.h
src/core/VulkanException.cpp
```

接口建议：

```cpp
#pragma once

#include <stdexcept>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

const char* vkResultName(VkResult result);

class VulkanException : public std::runtime_error {
public:
    VulkanException(VkResult result,
                    const char* expression,
                    const char* file,
                    int line);

    VkResult result() const { return result_; }

private:
    VkResult result_;
};

} // namespace vkr
```

实现要求：

- `vkResultName()` 至少覆盖常见错误：
  - `VK_SUCCESS`
  - `VK_NOT_READY`
  - `VK_TIMEOUT`
  - `VK_EVENT_SET`
  - `VK_EVENT_RESET`
  - `VK_INCOMPLETE`
  - `VK_ERROR_OUT_OF_HOST_MEMORY`
  - `VK_ERROR_OUT_OF_DEVICE_MEMORY`
  - `VK_ERROR_INITIALIZATION_FAILED`
  - `VK_ERROR_DEVICE_LOST`
  - `VK_ERROR_MEMORY_MAP_FAILED`
  - `VK_ERROR_LAYER_NOT_PRESENT`
  - `VK_ERROR_EXTENSION_NOT_PRESENT`
  - `VK_ERROR_FEATURE_NOT_PRESENT`
  - `VK_ERROR_INCOMPATIBLE_DRIVER`
  - `VK_ERROR_TOO_MANY_OBJECTS`
  - `VK_ERROR_FORMAT_NOT_SUPPORTED`
  - `VK_ERROR_SURFACE_LOST_KHR`
  - `VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`
  - `VK_SUBOPTIMAL_KHR`
  - `VK_ERROR_OUT_OF_DATE_KHR`
- unknown result 输出 `VK_UNKNOWN_RESULT(<int>)`。

### 4.2 修改 `VulkanCheck.h`

```cpp
#pragma once

#include "VulkanException.h"

#define VK_CHECK(expr)                                                        \
    do {                                                                      \
        const VkResult vkResult = (expr);                                     \
        if (vkResult != VK_SUCCESS) {                                         \
            throw ::vkr::VulkanException(vkResult, #expr, __FILE__, __LINE__);\
        }                                                                     \
    } while (0)
```

### 4.3 注意 swapchain 特例

当前 `FrameSync::beginFrame()` 和 `endFrame()` 已经手动处理 `VK_ERROR_OUT_OF_DATE_KHR` / `VK_SUBOPTIMAL_KHR`。这些地方不要盲目套 `VK_CHECK`，否则 resize 路径会变成异常。

规则：

- 普通 Vulkan 创建/提交 API 用 `VK_CHECK`。
- 需要业务分支处理的 VkResult 保持显式 `if`。

### 4.4 A2 验收

- 故意给错误 shader 路径时，异常中包含表达式和文件行号。
- Vulkan API 失败时，日志/异常出现 `VK_ERROR_*` 名称，而不只是整数。
- resize swapchain 行为不变。

---

## 5. A3：ResourceHandle / ResourcePool

### 5.1 新增文件

```text
src/core/ResourceHandle.h
src/core/ResourcePool.h
```

`ResourceHandle.h`：

```cpp
#pragma once

#include <cstdint>
#include <limits>

namespace vkr {

template <typename Tag>
struct Handle {
    uint32_t index = invalidIndex();
    uint32_t generation = 0;

    static constexpr uint32_t invalidIndex() {
        return std::numeric_limits<uint32_t>::max();
    }

    bool valid() const { return index != invalidIndex(); }
    explicit operator bool() const { return valid(); }

    friend bool operator==(Handle lhs, Handle rhs) {
        return lhs.index == rhs.index && lhs.generation == rhs.generation;
    }
    friend bool operator!=(Handle lhs, Handle rhs) { return !(lhs == rhs); }
};

struct TextureTag {};
struct MeshTag {};
struct MaterialTag {};
struct PipelineTag {};

} // namespace vkr
```

`ResourcePool.h`：

```cpp
#pragma once

#include <cassert>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "ResourceHandle.h"

namespace vkr {

template <typename T, typename Tag>
class ResourcePool {
public:
    using HandleT = Handle<Tag>;

    template <typename... Args>
    HandleT emplace(Args&&... args);

    HandleT insert(T value);
    T* get(HandleT handle);
    const T* get(HandleT handle) const;
    bool alive(HandleT handle) const;
    void release(HandleT handle);
    void clear();
    size_t size() const { return liveCount_; }

private:
    struct Slot {
        std::optional<T> value;
        uint32_t generation = 1;
    };

    std::vector<Slot> slots_;
    std::vector<uint32_t> freeList_;
    size_t liveCount_ = 0;
};

} // namespace vkr
```

### 5.2 先不替换现有资源

Phase A 只验证 handle/pool 基础能力，不要立刻把 `Scene` 的 `shared_ptr` 换掉。原因：

- `Material` 还依赖 `Renderer`。
- glTF loader 当前直接构造 `Texture` / `Mesh` / `Material` shared_ptr。
- 一次性替换会扩大风险，偏离 Phase A 的“不改变画面”。

### 5.3 临时自测入口

项目当前没有测试框架。Phase A 可先用 debug-only 函数做最小自测：

```text
src/core/ResourcePoolSelfTest.h
src/core/ResourcePoolSelfTest.cpp
```

接口：

```cpp
namespace vkr {
void runResourcePoolSelfTest();
}
```

只在 Debug 且启动早期调用一次：

```cpp
#ifndef NDEBUG
vkr::runResourcePoolSelfTest();
#endif
```

自测覆盖：

- 插入后 handle alive。
- release 后旧 handle 不 alive。
- 复用 slot 后 generation 增加。
- `get(oldHandle) == nullptr`。
- `clear()` 后所有 handle 失效。

后续引入测试框架时，把这段迁移到真实单元测试。

### 5.4 A3 验收

- Debug 启动时 self-test 通过且无输出噪音，失败时明确抛异常。
- `Handle<T>` 是小对象，可直接放进未来 `RenderCommand`。
- 未改变现有场景资源所有权。

---

## 6. A4：DescriptorAllocator / DescriptorSetManager

### 6.1 新增文件

```text
src/core/DescriptorAllocator.h
src/core/DescriptorAllocator.cpp
```

第一阶段先做 descriptor pool page allocator，不做 layout cache。

接口建议：

```cpp
#pragma once

#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;

class DescriptorAllocator {
public:
    explicit DescriptorAllocator(Device& device);
    ~DescriptorAllocator();

    DescriptorAllocator(const DescriptorAllocator&) = delete;
    DescriptorAllocator& operator=(const DescriptorAllocator&) = delete;

    VkDescriptorSet allocate(VkDescriptorSetLayout layout);
    void resetPools();

private:
    VkDescriptorPool createPool();
    VkDescriptorPool currentPool();

    Device* device_ = nullptr;
    std::vector<VkDescriptorPool> usedPools_;
    std::vector<VkDescriptorPool> freePools_;
};

} // namespace vkr
```

建议 pool size：

```cpp
std::array<VkDescriptorPoolSize, 4> sizes = {{
    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64},
    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128},
    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32},
    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 32},
}};
```

`VkDescriptorPoolCreateInfo::maxSets` 可先设为 `128`。

### 6.2 与 Material 的关系

Phase A 有两个可选落地深度：

#### 方案 A：只新增 allocator，不接入 Material

优点：风险最低。缺点：验收只能确认 allocator 可构建，不能减少当前 pool 数量。

#### 方案 B：让 Material 使用外部 allocator

优点：直接减少“每个 Material 一个 pool”。缺点：构造链路会稍微多传一个依赖。

推荐 Phase A 使用方案 B，但保留兼容入口。

改动思路：

1. `Application` 持有：

```cpp
std::unique_ptr<DescriptorAllocator> descriptorAllocator_;
```

2. `Application::init()` 在 `Device` 创建后初始化：

```cpp
descriptorAllocator_ = std::make_unique<DescriptorAllocator>(*device_);
```

3. 场景工厂和 `Material` 构造函数新增可选 allocator 参数。

如果不想在 Phase A 改穿整个场景工厂参数，可先只实现 allocator，不接入 Material，等 Phase B descriptor set 拆分时统一改。

### 6.3 不做的事情

Phase A 不拆 descriptor set 语义：

- 仍允许 `binding 0 = GlobalUBO`、`binding 1 = sampler` 的旧布局存在。
- 不强行引入 set 0 / set 1 分离。
- 不改变 shader descriptor layout。

真正的 global/material set 拆分放到 Phase B。

### 6.4 A4 验收

- allocator 析构时销毁所有 pool。
- `allocate()` 遇到 `VK_ERROR_OUT_OF_POOL_MEMORY` 或 `VK_ERROR_FRAGMENTED_POOL` 时能创建新 pool 重试。
- 如果接入 Material，场景切换和多 glTF 材质仍正常显示。

---

## 7. A5：PipelineConfigBuilder

### 7.1 新增文件

```text
src/core/PipelineConfigBuilder.h
src/core/PipelineConfigBuilder.cpp
```

接口建议：

```cpp
#pragma once

#include "PipelineConfig.h"

namespace vkr {

class PipelineConfigBuilder {
public:
    PipelineConfigBuilder& shaders(std::string vertPath, std::string fragPath);
    PipelineConfigBuilder& vertexLayout(VertexLayout layout);
    PipelineConfigBuilder& defaultVertexLayout();
    PipelineConfigBuilder& rasterization(VkCullModeFlags cullMode,
                                          VkFrontFace frontFace,
                                          VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL);
    PipelineConfigBuilder& depth(bool test,
                                 bool write,
                                 VkCompareOp compare = VK_COMPARE_OP_LESS);
    PipelineConfigBuilder& blending(bool enable);
    PipelineConfigBuilder& msaa(VkSampleCountFlagBits samples);
    PipelineConfigBuilder& descriptorLayout(VkDescriptorSetLayout layout);
    PipelineConfigBuilder& pushConstant(VkPushConstantRange range);

    PipelineConfig build() const;

private:
    PipelineConfig config_;
};

} // namespace vkr
```

### 7.2 第一批替换点

优先替换 `BuiltinScenes.cpp::makeStandardConfig()` 这类构造函数，而不是立刻改 `Material`。

目标是把：

```cpp
PipelineConfig cfg;
cfg.vertShaderPath = ...;
cfg.fragShaderPath = ...;
cfg.vertexLayout = defaultVertexLayout();
cfg.msaaSamples = device.msaaSamples();
```

改成：

```cpp
return PipelineConfigBuilder{}
    .shaders(vp, fp)
    .defaultVertexLayout()
    .msaa(device.msaaSamples())
    .build();
```

### 7.3 Material mutation 的处理边界

Phase A 可以保留 `Material` 里：

```cpp
config_.descriptorLayouts.push_back(descriptorSetLayout_);
```

但要在文档和代码注释中标记为兼容期行为。Phase B 再把 descriptor layout 归属迁移到 `MaterialTemplate`。

### 7.4 A5 验收

- `PipelineConfig` 仍是 `Pipeline` 的输入，不改变 `Pipeline` 构造函数。
- 至少一个场景配置改用 builder。
- 构造出的 pipeline 与当前画面一致。

---

## 8. 文件改动清单

建议新增：

```text
src/core/Log.h
src/core/Log.cpp
src/core/VulkanException.h
src/core/VulkanException.cpp
src/core/ResourceHandle.h
src/core/ResourcePool.h
src/core/ResourcePoolSelfTest.h
src/core/ResourcePoolSelfTest.cpp
src/core/DescriptorAllocator.h
src/core/DescriptorAllocator.cpp
src/core/PipelineConfigBuilder.h
src/core/PipelineConfigBuilder.cpp
```

建议修改：

```text
CMakeLists.txt
src/main.cpp
src/core/VulkanCheck.h
src/core/VulkanContext.cpp
src/core/Device.cpp
src/app/Application.cpp
src/render/Mesh.cpp
src/render/GltfLoader.cpp
src/scene/BuiltinScenes.cpp
```

可选修改：

```text
src/render/Material.h
src/render/Material.cpp
src/scene/SceneFactory.h/.cpp
```

是否修改可选文件取决于 A4 是否立即接入 `DescriptorAllocator`。

---

## 9. 推荐执行任务列表

### Task A1.1：CMake 接入 spdlog

- 增加 `SPDLOG_ROOT`。
- 将 `${SPDLOG_ROOT}` 放入 `target_include_directories`。
- 增加 `SPDLOG_HEADER_ONLY` 和 `SPDLOG_ACTIVE_LEVEL` definitions。

验证：重新 configure/build 成功。

### Task A1.2：实现 `core/Log.*`

- 实现 settings、sink 创建、tag logger 缓存。
- 支持 `VKR_LOG_LEVEL`、`VKR_LOG_FILE`、`VKR_LOG_NO_COLOR`。
- 默认创建 `logs` 目录。

验证：启动后有统一格式日志和文件日志。

### Task A1.3：替换主要输出点

- `main.cpp` fatal。
- `Device.cpp` GPU info。
- `Application.cpp` scene switch。
- `Mesh.cpp` mesh stats。
- `GltfLoader.cpp` warn。
- `VulkanContext.cpp` validation callback。

验证：搜索 `std::cout|std::cerr|fprintf(stderr`，只允许少量合理残留。

### Task A2.1：实现 VulkanException

- 新增 result name 映射。
- 异常文本格式：

```text
Vulkan call failed: vkCreateImage(...)
result: VK_ERROR_OUT_OF_DEVICE_MEMORY (-2)
at: src/core/Image.cpp:28
```

验证：错误时信息可读。

### Task A2.2：替换 VK_CHECK 宏

- `VK_CHECK(expr)` 使用 `#expr`。
- 保留 swapchain 特例手动分支。

验证：Debug/Release 都可编译。

### Task A3.1：实现 Handle/Pool

- 新增 `ResourceHandle.h` / `ResourcePool.h`。
- generation 从 1 开始，每次 release 后递增。
- `get()` 对无效 handle 返回 `nullptr`。

验证：self-test 通过。

### Task A3.2：加入 ResourcePool self-test

- Debug 启动时跑一次。
- 失败抛 `std::runtime_error`，成功不刷屏。

验证：启动无额外噪音。

### Task A4.1：实现 DescriptorAllocator

- 支持 pool page。
- 支持 out-of-pool 自动扩容重试。
- 析构销毁 used/free pools。

验证：可以分配多个 descriptor set。

### Task A4.2：决定是否接入 Material

- 若接入：`Material` 不再拥有 descriptor pool，只保存 descriptor sets。
- 若不接入：记录为 Phase B 第一项。

建议：如果想稳，A4 只新增 allocator；如果想更实际，A4 接入 Material，但不要拆 set layout。

### Task A5.1：实现 PipelineConfigBuilder

- 保持 `PipelineConfig` 原结构。
- Builder 只负责构造，不负责 Vulkan 对象。

验证：至少 `makeStandardConfig()` 使用 builder。

---

## 10. 验证矩阵

| 验证项 | 方式 | 通过标准 |
|---|---|---|
| 编译 | CMake Tools build `VulkanLab` | 无编译错误。 |
| 启动 | 运行默认场景 | 窗口显示与当前一致。 |
| 日志 | 查看终端与 `logs/VulkanLab.log` | 格式统一，有模块 tag。 |
| validation | 开启验证层运行 | warning/error 走 `[Vulkan]` logger。 |
| resize | 拖动窗口/最大化 | 不因 `VK_SUBOPTIMAL_KHR` 或 `VK_ERROR_OUT_OF_DATE_KHR` 崩溃。 |
| 场景切换 | 用现有 UI 切换场景 | 能正常释放/创建资源。 |
| ResourcePool | Debug self-test | 旧 handle 失效、generation 生效。 |
| descriptor | 加载多材质 glTF | descriptor set 分配正常。 |

---

## 11. Phase A 不做事项

为避免重构扩散，本阶段明确不做：

- 不引入 `RenderQueue`。
- 不拆 `Scene::render()`。
- 不拆 `Renderer::beginRenderPass/endRenderPass`。
- 不拆 global/material descriptor set。
- 不改 shader binding。
- 不把 `Texture/Mesh/Material` 全部换成 handle。
- 不引入 ECS、SceneGraph、Light、Shadow、PostProcess。

这些都放到 Phase B 之后按路线图推进。

---

## 12. 完成定义

Phase A 完成时，应满足：

- `CMakeLists.txt` 正确接入 header-only spdlog。
- `core/Log.*` 可用，主要输出已迁移到 logger。
- `core/VulkanException.*` 可用，`VK_CHECK` 异常可读。
- `core/ResourceHandle.h` / `core/ResourcePool.h` 可用，并有最小自测。
- `core/DescriptorAllocator.*` 已实现，是否接入 Material 有明确决定。
- `core/PipelineConfigBuilder.*` 已实现，并至少替换一个 pipeline config 构造点。
- 默认场景、场景切换、窗口 resize 的行为与 Phase A 之前一致。

完成这些后，再进入 Phase B 的“材质与资源解耦”会自然很多：日志能帮忙定位问题，VK_CHECK 能给出明确失败点，descriptor allocator 可以承接 set 0 / set 1 拆分，handle/pool 可以逐步接管 Texture/Mesh/MaterialInstance。