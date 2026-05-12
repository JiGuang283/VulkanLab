# VulkanLab 日志系统引入方案（spdlog）

## 1. 背景与结论

当前项目已经把 `spdlog` 放到了 `external/spdlog`。这意味着日志系统不再需要自研格式化、颜色、文件滚动和级别过滤，建议直接在 `src/core` 增加一个很薄的 `Log` 门面，把项目所有终端输出统一收束到 spdlog。

这次整理要解决两类问题：

1. **项目自己的输出不成体系**：GPU 信息、Mesh 统计、场景切换、glTF 图片降级、异常输出分别使用 `std::cout`、`std::cerr`、`std::fprintf(stderr, ...)`，没有统一级别、来源标签和过滤策略。
2. **Vulkan Loader / validation 信息混在一起**：当前 `run_stderr.log` 中的 `Searching for ICD drivers`、`Loading layer library`、Overwolf DLL 加载行并不是项目自己的日志，也不是 Vulkan debug callback 的业务验证消息，而是 Vulkan Loader 的调试轨迹。项目的 debug callback 又使用同样的 `validation layer:` 前缀，导致肉眼无法区分真正的 validation warning/error 和 Loader 噪音。

推荐目标：

- 屏幕默认只显示 `info/warn/error`，并且每行都有时间、级别、模块名。
- 文件日志保留更细的 `trace/debug`，便于排查启动、资源加载和 validation 问题。
- Vulkan debug callback 输出改成 `[Vulkan][Validation]` 形式，保留 warning/error，默认不订阅 verbose。
- 尽量从启动环境上关闭 Vulkan Loader 的 debug trace，而不是在日志系统里做 stderr 文本过滤。

## 2. 当前输出来源盘点

基于当前代码和本次运行日志，输出点如下：

| 来源 | 当前位置 | 当前方式 | 建议归属 |
|---|---|---|---|
| fatal exception | `src/main.cpp` | `std::cerr << e.what()` | `App` / `critical` |
| 场景切换 | `src/app/Application.cpp` | `std::cout << "[Scene] ..."` | `Scene` / `info` |
| GPU 选择信息 | `src/core/Device.cpp` | 多行 `std::cout` + 分隔线 | `Device` / `info` |
| OBJ 顶点/索引统计 | `src/render/Mesh.cpp` | `std::cout` | `Mesh` / `info` 或 `debug` |
| glTF 图片解码失败降级 | `src/render/GltfLoader.cpp` | `std::fprintf(stderr, ...)` | `Gltf` / `warn` |
| Vulkan debug callback | `src/core/VulkanContext.cpp` | `std::cerr << "validation layer:"` | `Vulkan` / 按 severity 映射 |
| Loader ICD/layer 扫描 | Vulkan Loader / 环境变量 | stderr，前缀也是 `validation layer:` | 启动环境治理，不归项目 logger |

当前 `run_stdout.log` 是干净但格式松散的业务输出：

```text
---------------------------------
Selected GPU: NVIDIA GeForce RTX 4060 Laptop GPU
Device Type : Discrete GPU
API Version : 1.4.312
---------------------------------
Vertices: 3566, Indices: 11484
```

当前 `run_stderr.log` 主要是 Loader 调试轨迹：

```text
validation layer: Searching for ICD drivers named .\igvk64.dll
validation layer: Searching for ICD drivers named .\nvoglv64.dll
validation layer: Loading layer library C:\VulkanSDK\...\VkLayer_khronos_validation.dll
validation layer: Loading layer library C:\Program Files (x86)\Overwolf\...\ow-graphics-vulkan.dll
validation layer: Loading layer library C:\Windows\System32\...\nvoglv64.dll
```

这里最关键的判断是：**这些 `Searching/Loading layer library` 行不应该通过项目 logger 处理**。它们发生在 Vulkan Loader 内部，正确处理方式是清理启动环境里的 Loader debug 开关，或者在 VS Code launch/task 环境中显式关闭。

## 3. spdlog 接入方式

### 3.1 CMake 配置

当前 `external/spdlog` 的目录结构是：

```text
external/spdlog/spdlog.h
external/spdlog/sinks/...
external/spdlog/fmt/...
```

`spdlog.h` 内部使用 `#include <spdlog/common.h>` 这种路径，因此 CMake 的 include 根目录应该加 `external`，而不是 `external/spdlog`。

建议在 `CMakeLists.txt` 中加入：

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

说明：

- 这个 spdlog 目录没有单独参与 CMake build，最稳妥的方式是 header-only。
- `SPDLOG_HEADER_ONLY` 必须在包含 spdlog 头之前可见，所以放在 target compile definitions。
- 继续使用 `file(GLOB_RECURSE SOURCES ...)` 的现状时，新加的 `src/core/Log.cpp` 会自动进入目标；后续如果改成显式 source list，再把它补进去。
- 现有 CMake 里同时有全局 `include_directories(...)` 和 `target_include_directories(...)`。日志方案只需要改 target 级 include，减少后续外部库互相污染。

### 3.2 新增日志门面

新增文件：

```text
src/core/Log.h
src/core/Log.cpp
```

不建议业务代码到处直接调用 `spdlog::get(...)`。保留一层 `vkr::log` 的原因是：

- 以后想把日志接到 ImGui 面板、文件路径配置、RenderDoc marker 或 telemetry 时，不需要全项目替换。
- 可以统一模块名、默认 pattern、flush 策略和环境变量解析。
- 可以把 spdlog 作为实现细节，避免渲染/场景层被第三方库 API 绑死。

建议接口形态：

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

void init(const Settings &settings = {});
void shutdown();
void setConsoleLevel(Level level);

std::shared_ptr<spdlog::logger> logger(std::string_view tag);

} // namespace vkr::log

#define VKR_LOG_TRACE(tag, ...) SPDLOG_LOGGER_TRACE(::vkr::log::logger(tag), __VA_ARGS__)
#define VKR_LOG_DEBUG(tag, ...) SPDLOG_LOGGER_DEBUG(::vkr::log::logger(tag), __VA_ARGS__)
#define VKR_LOG_INFO(tag, ...)  SPDLOG_LOGGER_INFO (::vkr::log::logger(tag), __VA_ARGS__)
#define VKR_LOG_WARN(tag, ...)  SPDLOG_LOGGER_WARN (::vkr::log::logger(tag), __VA_ARGS__)
#define VKR_LOG_ERROR(tag, ...) SPDLOG_LOGGER_ERROR(::vkr::log::logger(tag), __VA_ARGS__)
#define VKR_LOG_CRITICAL(tag, ...) SPDLOG_LOGGER_CRITICAL(::vkr::log::logger(tag), __VA_ARGS__)
```

宏使用 spdlog 的 `SPDLOG_LOGGER_*` 系列，而不是 `logger->info(...)`，这样可以保留文件名、行号、函数名等 source location 信息，未来文件日志里会很有用。

### 3.3 Logger 与 sink 设计

推荐 `Log.cpp` 中维护一个 registry：每个模块一个 logger name，例如 `App`、`Device`、`Vulkan`、`Scene`、`Mesh`、`Gltf`。所有 logger 共享同一组 sinks。

Sink 组合：

1. `spdlog::sinks::stderr_color_sink_mt`：控制台彩色输出。
2. `spdlog::sinks::rotating_file_sink_mt`：滚动文件输出，默认 `logs/VulkanLab.log`，例如 5 MB * 3 个文件。

建议 pattern：

控制台：

```text
%^[%H:%M:%S.%e] [%^%l%$] [%n] %v%$
```

文件：

```text
[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] [%s:%# %!] %v
```

示例效果：

```text
[14:08:12.031] [info] [Device] Selected GPU: NVIDIA GeForce RTX 4060 Laptop GPU (Discrete GPU, API 1.4.312)
[14:08:12.188] [info] [Mesh] models/viking_room.obj: 3566 vertices, 11484 indices
[14:08:17.442] [info] [Scene] Switched to "Sheen Chair"
[14:08:19.004] [warn] [Vulkan] [Validation][Performance] messageId=... message=...
```

默认级别建议：

| 构建/输出 | console | file |
|---|---|---|
| Debug | `info` | `trace` |
| Release | `info` 或 `warn` | `debug` |

环境变量覆盖：

| 变量 | 用途 |
|---|---|
| `VKR_LOG_LEVEL=trace/debug/info/warn/error/off` | 覆盖 console 最小级别 |
| `VKR_LOG_FILE=path/to/file.log` | 覆盖文件日志路径 |
| `VKR_LOG_NO_COLOR=1` | 禁用控制台颜色 |
| `VKR_LOG_FLUSH=trace/debug/info/warn/error` | 调整 flush 级别，默认 `warn` |

## 4. Vulkan Loader 噪音治理

### 4.1 不建议做 stderr 文本过滤

不要通过 `freopen`、重定向 stderr、hook `std::cerr`、正则过滤 `validation layer:` 等方式处理 Loader 噪音。原因：

- 会误吞真正的 validation warning/error。
- 第三方 layer、驱动、Vulkan SDK 版本改变后文本会变。
- 一旦 stderr 被重定向，VS Code / CMake Tools 的错误捕获也可能受影响。

### 4.2 在进程最早位置清理 Loader debug 环境

在 `main()` 的最开头、任何 Vulkan API 被调用之前做一次启动环境清理：

```cpp
static void sanitizeVulkanLoaderEnvironment() {
#ifdef _WIN32
    _putenv_s("VK_LOADER_DEBUG", "");
#else
    unsetenv("VK_LOADER_DEBUG");
#endif
}
```

这个动作只针对 Loader debug trace，不关闭 validation layer 本身。

注意：如果仍能看到 `Searching for ICD drivers` / `Loading layer library`，说明噪音可能发生得比 `main()` 更早，或来自 VS Code/CMake 启动环境之外的全局注入。此时需要在 `.vscode/launch.json`、CMake Tools launch 配置或系统环境变量里删除 `VK_LOADER_DEBUG`。

### 4.3 第三方 implicit layer 的处理策略

日志中出现了 Overwolf：

```text
C:\Program Files (x86)\Overwolf\...\ow-graphics-vulkan.dll
C:\Program Files (x86)\Overwolf\...\owclient.dll
```

这属于 Vulkan implicit layer 注入。默认建议不要在项目里粗暴禁用所有 implicit layer，因为 RenderDoc、Nsight、Steam overlay 等调试工具也依赖同类机制。

处理策略：

1. 默认只清理 `VK_LOADER_DEBUG`，减少可视噪音。
2. 如果某个 implicit layer 造成 crash 或大量输出，再在本机启动配置中临时禁用对应 layer。
3. 不把 Overwolf 专用环境变量硬编码到项目代码里，避免把个人机器状态写进工程逻辑。

## 5. Vulkan debug callback 改造

当前 `VulkanContext.cpp` 中：

```cpp
std::cerr << "validation layer: " << pCallbackData->pMessage << std::endl;
```

建议改成：

1. 按 `messageSeverity` 映射到 spdlog 级别。
2. 按 `messageType` 加 `[General]`、`[Validation]`、`[Performance]` 标签。
3. 输出 `pMessageIdName` / `messageIdNumber`，方便未来过滤已知噪音。
4. 默认只订阅 `WARNING | ERROR`，需要深入诊断时再通过配置打开 `INFO | VERBOSE`。

建议映射：

| Vulkan severity | spdlog level | 默认订阅 |
|---|---|---|
| `VERBOSE` | `trace` | 否 |
| `INFO` | `debug` | 否 |
| `WARNING` | `warn` | 是 |
| `ERROR` | `error` | 是 |

`populateDebugMessengerCreateInfo` 默认：

```cpp
createInfo.messageSeverity =
    VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
```

如果之后想让文件日志保留 validation info/verbose，可以加环境变量：

```text
VKR_VALIDATION_VERBOSE=1
```

当它存在时，实例创建前把 `INFO | VERBOSE` 也加入订阅，同时 console 仍由 `VKR_LOG_LEVEL` 控制是否显示。

## 6. 需要替换的调用点

| 文件 | 当前输出 | 推荐替换 |
|---|---|---|
| `src/main.cpp` | `std::cerr << e.what()` | `VKR_LOG_CRITICAL("App", "Fatal error: {}", e.what())` |
| `src/app/Application.cpp` | `[Scene] switched to ...` | `VKR_LOG_INFO("Scene", "Switched to \"{}\"", entry.name)` |
| `src/core/Device.cpp` | GPU 多行 banner | 单行 `VKR_LOG_INFO("Device", "Selected GPU: {} ({}, API {}.{}.{})", ...)` |
| `src/render/Mesh.cpp` | `Vertices: ..., Indices: ...` | `VKR_LOG_INFO("Mesh", "{}: {} vertices, {} indices", path, vertices.size(), indices.size())` |
| `src/render/GltfLoader.cpp` | image decode failed `fprintf` | `VKR_LOG_WARN("Gltf", "image[{}] decode failed, using fallback white texture", i)` |
| `src/core/VulkanContext.cpp` | `validation layer:` | `VKR_LOG_WARN/ERROR("Vulkan", "[Validation] ...")` |

替换之后，`<iostream>` / `<cstdio>` include 也可以跟着清理，但只清理确实不再使用的文件，避免无关改动扩大。

## 7. 初始化与生命周期

推荐 `main.cpp` 顺序：

```cpp
int main() {
    sanitizeVulkanLoaderEnvironment();
    vkr::log::init();

    try {
        vkr::Config config;
        vkr::Application app(config);
        // register scenes...
        app.run();
    } catch (const std::exception &e) {
        VKR_LOG_CRITICAL("App", "Fatal error: {}", e.what());
        vkr::log::shutdown();
        return EXIT_FAILURE;
    }

    vkr::log::shutdown();
    return EXIT_SUCCESS;
}
```

`shutdown()` 需要调用 `spdlog::shutdown()`，保证文件 sink flush 完成。异常路径也要走 shutdown。

如果担心 `main.cpp` 变得过长，可以把场景注册继续留在 `main`，日志初始化保持前置即可。现在的项目仍是教程式入口，这样最直接。

## 8. 分阶段实施计划

### Step 1：接入 spdlog 编译配置

改动：

- `CMakeLists.txt` 增加 `${CMAKE_SOURCE_DIR}/external` include。
- 增加 `SPDLOG_HEADER_ONLY`。
- Debug 打开 `SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE`。

验收：

- 新建一个临时最小 include 后能通过编译。
- 不引入新的链接库依赖。

### Step 2：新增 `Log` 模块

改动：

- 新增 `src/core/Log.h` / `src/core/Log.cpp`。
- 实现 sinks、pattern、环境变量解析、logger registry、shutdown。
- 默认创建 `logs/` 目录并写滚动文件。

验收：

- 程序启动后生成 `logs/VulkanLab.log`。
- console 与 file pattern 不同：console 简洁，file 带 source location。

### Step 3：替换项目自己的输出

改动：

- 替换 `main.cpp`、`Application.cpp`、`Device.cpp`、`Mesh.cpp`、`GltfLoader.cpp` 的输出点。
- GPU banner 从 5 行变成 1 行。
- Mesh 统计保留 path，方便知道是哪一个资产。

验收：

- `run_stdout.log` 不再出现业务日志。
- console 输出统一为 spdlog 格式。

### Step 4：改造 Vulkan debug callback

改动：

- `VulkanContext.cpp` 引入 `Log.h`。
- callback 按 severity/type 映射。
- 默认 messageSeverity 只保留 warning/error。
- 可选 `VKR_VALIDATION_VERBOSE=1` 打开 info/verbose。

验收：

- 人为触发 validation warning 时，输出为 `[warn] [Vulkan] [Validation] ...`。
- 不再出现由项目代码打印的 `validation layer:` 前缀。

### Step 5：治理 Loader 噪音

改动：

- `main()` 最早调用 `sanitizeVulkanLoaderEnvironment()`。
- 如 VS Code/CMake Tools 仍注入 `VK_LOADER_DEBUG`，补充 `.vscode` 启动配置或记录排查步骤。

验收：

- 正常启动时不再出现 `Searching for ICD drivers` / `Loading layer library` 的 Loader trace。
- validation layer 本身仍然启用，真正的 warning/error 仍能出现。

### Step 6：文档与排查约定

改动：

- 在 `doc/log` 保留本方案。
- 后续实现后可增加 `doc/log/usage.md`，记录常用环境变量和日志文件位置。

验收：

- 新开发者能通过文档知道如何打开 trace、如何定位 validation 消息、如何区分 Loader 和 callback 输出。

## 9. 目标输出对照

改造前：

```text
validation layer: Searching for ICD drivers named .\igvk64.dll
validation layer: Loading layer library ...\VkLayer_khronos_validation.dll
---------------------------------
Selected GPU: NVIDIA GeForce RTX 4060 Laptop GPU
Device Type : Discrete GPU
API Version : 1.4.312
---------------------------------
Vertices: 3566, Indices: 11484
```

改造后：

```text
[14:08:12.031] [info] [Device] Selected GPU: NVIDIA GeForce RTX 4060 Laptop GPU (Discrete GPU, API 1.4.312)
[14:08:12.188] [info] [Mesh] models/viking_room.obj: 3566 vertices, 11484 indices
[14:08:17.442] [info] [Scene] Switched to "Sheen Chair"
```

出现 validation warning 时：

```text
[14:09:03.705] [warn] [Vulkan] [Validation] messageId=VUID-... message=...
```

文件日志中同一条会带源码位置：

```text
[2026-05-06 14:09:03.705] [warn] [Vulkan] [src/core/VulkanContext.cpp:55 debugCallback] [Validation] messageId=VUID-... message=...
```

## 10. 风险与取舍

1. **header-only 编译时间略增**：当前项目规模很小，可以接受；如果以后变大，再改为 spdlog 编译库模式。
2. **日志宏会暴露 spdlog 头**：通过 `Log.h` 集中暴露，业务层不直接 include spdlog 其他头，后续迁移成本可控。
3. **Loader 噪音不一定能在代码里完全消失**：如果来自进程启动前或外部工具注入，需要改 VS Code/CMake Tools/系统环境。项目代码负责尽早清理，文档负责给排查路径。
4. **不要默认禁用 implicit layers**：否则可能影响 RenderDoc、Nsight 等图形调试工具。只在具体 layer 出问题时临时禁用。
5. **validation verbose 不应默认进 console**：Vulkan 信息量很大，默认 warning/error 更适合日常开发。

## 11. 推荐完成定义

完成该日志系统接入后，应满足：

- `std::cout` / `std::cerr` / `fprintf(stderr, ...)` 不再用于项目业务日志。
- 控制台所有项目日志都有统一格式：时间、级别、模块名、消息。
- `logs/VulkanLab.log` 能保留更详细的 trace/debug 信息。
- Vulkan validation warning/error 仍然可见，并且和 Loader trace 一眼可分。
- 正常启动不再被 ICD/layer 搜索轨迹刷屏。
- Debug 和 Release 均可编译运行。

## 12. 后续扩展

日志系统稳定后，可以继续做三件小扩展：

1. **ImGui Log 窗口**：增加一个内存 ring buffer sink，把最近 N 条日志显示在 UI 中，适合全屏运行时查看。
2. **Vulkan object name 辅助**：封装 `vkSetDebugUtilsObjectNameEXT`，让 validation 消息出现 Buffer/Image/Pipeline 的业务名。
3. **资源加载耗时日志**：用 `spdlog::stopwatch` 或 `std::chrono` 记录 glTF、Texture、Pipeline 创建耗时，帮助后续优化启动时间。

## 13. 和旧方案的关系

旧文档 `doc/cout/log_redesign.md` 的现状分析仍然有效，尤其是“Loader 噪音不等于 validation callback”这一点。但旧方案选择了自研轻量 logger，并写着 `spdlog 暂缓`。

现在仓库已经有 `external/spdlog`，因此新的推荐是：**不再自研 logger，直接用 spdlog 做实现，用 `vkr::log` 做项目门面**。这样能少写维护代码，同时马上获得彩色输出、文件 sink、滚动日志、fmt 格式化和编译期级别控制。