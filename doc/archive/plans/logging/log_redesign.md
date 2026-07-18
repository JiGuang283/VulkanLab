# 终端输出整理 — 设计方案

## 1. 现状分析

### 1.1 实测输出分类

```
validation layer: windows_get_device_registry_files: GUID for 5 is not ...   ← ①
validation layer: Searching for ICD drivers named .\igvk64.dll              ← ①
validation layer: Loading layer library ...\VkLayer_khronos_validation.dll  ← ①
validation layer: Loading layer library ...\ow-graphics-vulkan.dll          ← ①
validation layer: Loading layer library ...\nvoglv64.dll                    ← ①
---------------------------------                                           ← ②
Selected GPU: NVIDIA GeForce RTX 4060 Laptop GPU                            ← ②
Device Type : Discrete GPU                                                  ← ②
API Version : 1.4.329                                                       ← ②
---------------------------------                                           ← ②
Vertices: 4725, Indices: 11484                                              ← ③
[Scene] switched to Sheen Chair                                             ← ④
[Scene] switched to A Beautiful Game                                        ← ④
validation layer: Unloading layer library ...\nvoglv64.dll                  ← ①
...                                                                         ← ①
```

| # | 来源 | 文件 / 代码位置 | 性质 |
|---|---|---|---|
| ① | **Loader 调试输出**（不是我们的代码，也不是 validation callback） | Vulkan Loader 自己，通过环境变量 `VK_LOADER_DEBUG` / `VK_LOADER_LAYERS_ENABLE` 触发 | 噪音 |
| ② | GPU 信息 | [src/core/Device.cpp](../../../../src/core/Device.cpp) L70–96 `std::cout` | 有用，偶尔看 |
| ③ | OBJ 载入完成统计 | [src/render/Mesh.cpp](../../../../src/render/Mesh.cpp) L103 `std::cout` | 信息级 |
| ④ | 场景切换 | [src/app/Application.cpp](../../../../src/app/Application.cpp) L118 `std::cout` | 信息级 |
| — | validation callback | [src/core/VulkanContext.cpp](../../../../src/core/VulkanContext.cpp) L44 `std::cerr` | 有用，不过目前**与 ① 的前缀相同**，容易混淆 |

### 1.2 核心问题

1. **前缀冲突**：Vulkan Loader 自己打的行（`validation layer: Searching / Loading / Unloading ...`）和我们 debug callback 打的行**完全共用 `validation layer:` 前缀**，无法区分谁是谁。用户看到的 90% 噪音都来自 Loader，不来自 callback。
2. **Loader 噪音没关**：环境变量 `VK_LOADER_DEBUG=all` / `VK_LAYER_PATH` 或者注册表里的 `VK_LOADER_LAYERS_ENABLE` 可能被设了，或 SDK 默认就开。进程内无法通过 API 彻底关闭（Loader 早于我们 Init 就打印了），只能在启动环境里抑制。
3. **没有级别/通道**：所有消息同级、同通道（`cout` + `cerr` 混用）、无时间戳、无来源标签。
4. **没有过滤**：verbose / warning / error 混杂在一起。debug callback 订阅了 `VERBOSE_BIT`，但目前没打出来只是因为 validation layer 默认不发 Info/Verbose —— 一旦开启就会雪崩。
5. **一次性信息与循环信息混在同一流**：开机 banner、切场景、每次 OBJ 加载都往同一片屏幕写，没有分组。

---

## 2. 设计目标

1. **默认屏幕只显示：** banner（1 次）+ warning/error + 用户主动触发的事件（如 "switched to XXX"）。
2. **彻底屏蔽 Vulkan Loader 的 `Searching/Loading/Unloading layer library` 噪音**。
3. 我们自己的日志全部加一致的前缀 + 级别，方便 `grep`。
4. 与 validation callback 的输出视觉上**一眼可分辨**。
5. 极小侵入：不引入第三方库（spdlog 暂缓），用 ~100 行自维护的 logger 就够。
6. 向后兼容：保留"把日志落盘到文件"这一扩展点，现在先只走 stderr。

---

## 3. 方案

### 3.1 统一日志门面 `Log`

新文件：`src/core/Log.h` / `src/core/Log.cpp`

```cpp
// Log.h
namespace vkr::log {

enum class Level { Trace, Info, Warn, Error };

void setMinLevel(Level lv);     // 运行时阈值，默认 Info
void setUseColor(bool on);      // Windows: 自动探测 ANSI 支持

void log(Level lv, const char* tag, const char* fmt, ...);

#define VKR_LOG_INFO(tag, ...)  ::vkr::log::log(::vkr::log::Level::Info,  tag, __VA_ARGS__)
#define VKR_LOG_WARN(tag, ...)  ::vkr::log::log(::vkr::log::Level::Warn,  tag, __VA_ARGS__)
#define VKR_LOG_ERROR(tag, ...) ::vkr::log::log(::vkr::log::Level::Error, tag, __VA_ARGS__)
#define VKR_LOG_TRACE(tag, ...) ::vkr::log::log(::vkr::log::Level::Trace, tag, __VA_ARGS__)

} // namespace
```

**输出格式**（目标样貌）：

```
[01:23.456 INFO  Device ] Selected GPU: NVIDIA GeForce RTX 4060 Laptop GPU (Discrete, API 1.4.329)
[01:25.012 INFO  Scene  ] Switched to "Sheen Chair"
[01:25.045 INFO  Mesh   ] viking_room.obj: 4725 verts / 11484 idx
[01:30.998 WARN  Vulkan ] [Validation] Buffer<...> destroyed while still in use
[01:31.200 ERROR Gltf   ] Failed to decode image 3 from buffer view
```

- 时间从进程启动计相对分:秒.毫秒，方便横向对比帧耗时段。
- `tag` 固定 7 字符宽对齐（`Device`, `Scene`, `Mesh`, `Gltf`, `Vulkan`, `App`）。
- 颜色（启用时）：Trace=灰、Info=默认、Warn=黄、Error=红。Windows 10+ 启用 `ENABLE_VIRTUAL_TERMINAL_PROCESSING` 后原生 ANSI 支持。
- 输出**全部走 `stderr`**（避免被行缓冲吞掉；gui/管道友好）。
- 可选 `VKR_LOG_FILE` 环境变量：若存在则 tee 到文件。

### 3.2 Validation callback 重构

[src/core/VulkanContext.cpp](../../../../src/core/VulkanContext.cpp) 的 `debugCallback`：

- 按 `messageSeverity` 映射到 `Level`：
  - `VERBOSE` → Trace
  - `INFO`    → Info（建议默认关掉订阅，见下）
  - `WARNING` → Warn
  - `ERROR`   → Error
- tag 统一用 `Vulkan`，消息前再加 `[Validation]` / `[Performance]` / `[General]` 按 `messageType` 区分。
- **订阅位改为：** `WARNING | ERROR`，不要 `VERBOSE | INFO`，从源头削减量。验证需要细看时再打开 Trace 阈值 + 订阅 VERBOSE。

### 3.3 关闭 Vulkan Loader 的调试噪音

这是**最关键的一步**。Loader 的 `Searching/Loading/Unloading` 日志由 Loader 自己用环境变量 `VK_LOADER_DEBUG` 控制，进程启动后修改无效。两条路：

**方案 A — 在 `main` 最开头强制清除（推荐）**

```cpp
// src/main.cpp 进 vkr::Application 构造之前
_putenv_s("VK_LOADER_DEBUG", "");        // 清空；等价于未设
_putenv_s("VK_LOADER_LAYERS_DISABLE", ""); // 以防 Overwolf 等注入
```

注意这只能影响**本进程之后**对该 env 的读取，而 Loader 在 `vkCreateInstance` 才读——晚于 `main` 早期写入，**有效**。实测可以干掉那几十行 `Searching/Loading layer library`。

**方案 B — 让用户用 `.vscode/launch.json` 设 env**

侵入性小但依赖外部配置，不可靠（直接双击 exe 就失效）。

→ 采用 **方案 A**，主入口加两行。

### 3.4 关闭 Overwolf 等第三方注入（可选）

日志里 `ow-graphics-vulkan.dll` / `owclient.dll` 来自 Overwolf 的 Vulkan layer 注入，不是我们能关的。但可以在 **instance 创建时显式白名单** `VK_INSTANCE_LAYERS`，让这些隐式 layer 不被加载：

```cpp
_putenv_s("DISABLE_VK_LAYER_OW_OBS_HOOK_1", "1"); // 或 Overwolf 的具体 layer 名
```

这是 runtime 可做的脏修；更干净的做法是 **不去管它**，只关 Loader 的 verbose 打印（方案 A 已经解决了 90% 可视噪音）。

### 3.5 调用点替换清单

| 文件 | 现在 | 改成 |
|---|---|---|
| [src/core/Device.cpp](../../../../src/core/Device.cpp) L70–96 | 6 行 `std::cout` + 分隔线 | 单行 `VKR_LOG_INFO("Device", "Selected GPU: %s (%s, API %d.%d.%d)", ...)` |
| [src/render/Mesh.cpp](../../../../src/render/Mesh.cpp) L103 | `std::cout << "Vertices: ..."` | `VKR_LOG_INFO("Mesh", "%s: %zu verts / %u idx", path, verts, idx)` |
| [src/app/Application.cpp](../../../../src/app/Application.cpp) L118 | `std::cout << "[Scene] switched to "` | `VKR_LOG_INFO("Scene", "Switched to \"%s\"", entry.name.c_str())` |
| [src/render/GltfLoader.cpp](../../../../src/render/GltfLoader.cpp) L139 | `std::fprintf(stderr, ...)` | `VKR_LOG_WARN("Gltf", ...)` |
| [src/main.cpp](../../../../src/main.cpp) L48 | `std::cerr << e.what()` | `VKR_LOG_ERROR("App", "Fatal: %s", e.what())` |
| [src/core/VulkanContext.cpp](../../../../src/core/VulkanContext.cpp) `debugCallback` | `std::cerr << "validation layer: " << ...` | 映射到 `VKR_LOG_WARN/ERROR("Vulkan", ...)` |

### 3.6 配置开关

一次性的工程决定，写死在 `Log.cpp` 静态构造里即可，无需做成配置文件：

- 默认 `minLevel = Info`（Release 构建）/ `Trace`（Debug 构建）
- 颜色：检测 `stderr` 是否是 tty，是就开
- 可被环境变量覆盖：`VKR_LOG_LEVEL=trace|info|warn|error`

---

## 4. 预期效果对照

**改造前（冷启动 + 切 3 次场景）**：
```
validation layer: windows_get_device_registry_files: ...
validation layer: Searching for ICD drivers named .\igvk64.dll
validation layer: Searching for ICD drivers named .\nvoglv64.dll
validation layer: Loading layer library ...VkLayer_khronos_validation.dll
validation layer: Loading layer library ...ow-graphics-vulkan.dll
validation layer: Loading layer library ...owclient.dll
validation layer: Loading layer library ...nvoglv64.dll
validation layer: Loading layer library ...nvoglv64.dll
---------------------------------
Selected GPU: NVIDIA GeForce RTX 4060 Laptop GPU
Device Type : Discrete GPU
API Version : 1.4.329
---------------------------------
Vertices: 4725, Indices: 11484
[Scene] switched to Sheen Chair
[Scene] switched to A Beautiful Game
[Scene] switched to Anisotropy Barn Lamp
[Scene] switched to Car Concept
validation layer: Unloading layer library ... (× 5)
```
≈ 21 行 / 一半是 Loader 噪音。

**改造后**：
```
[00:00.012 INFO  Device ] Selected GPU: NVIDIA GeForce RTX 4060 Laptop GPU (Discrete, API 1.4.329)
[00:00.310 INFO  Mesh   ] models/viking_room.obj: 4725 verts / 11484 idx
[00:05.120 INFO  Scene  ] Switched to "Sheen Chair"
[00:07.440 INFO  Scene  ] Switched to "A Beautiful Game"
[00:09.880 INFO  Scene  ] Switched to "Anisotropy Barn Lamp"
[00:12.030 INFO  Scene  ] Switched to "Car Concept"
```
6 行，全是**有信息量**的行，validation error 才会红色出现。

---

## 5. 实施步骤

1. 新增 [src/core/Log.h](../../../../src/core/Log.h) / `Log.cpp` — 实现上面描述的门面 + 彩色 + 时间戳。
2. [src/main.cpp](../../../../src/main.cpp) 入口加 `_putenv_s("VK_LOADER_DEBUG", "");`。
3. 替换 3.5 节列表里的 6 个调用点。
4. [src/core/VulkanContext.cpp](../../../../src/core/VulkanContext.cpp) 里：
   - `createInfo.messageSeverity` 去掉 `VERBOSE_BIT` 与 `INFO_BIT`
   - `debugCallback` 按 severity 映射到 `VKR_LOG_WARN/ERROR`
5. 编译 Debug+Release，对比输出。
6. （可选）把 Log 接到一个 ring buffer，未来用 ImGui 渲染一个"Log"窗口。

---

## 6. 风险与回滚

- **`_putenv_s` 跨平台**：仅 Windows。留 `#ifdef _WIN32` 包着即可；Linux 下 Loader 用 `VK_LOADER_DEBUG` 环境变量，`setenv("VK_LOADER_DEBUG", "", 1)` 同样生效。
- **Overwolf DLL 仍可能打印**：那是它们自己的层在自己的初始化路径里 `printf`，我们管不到。若问题严重，只能让用户退出 Overwolf。
- **彩色在 Windows 7 / 旧 cmd 可能乱码**：自动探测 + 环境变量 `VKR_NO_COLOR` 关闭。
- **回滚**：`git checkout HEAD -- src/core/Log.* src/main.cpp src/core/VulkanContext.cpp src/core/Device.cpp src/render/Mesh.cpp src/render/GltfLoader.cpp src/app/Application.cpp`（6 个文件）。

---

## 7. 小结

当前噪音的真正大头是 **Vulkan Loader 自己在 `VK_LOADER_DEBUG` 打开时打印的加载轨迹**，不是我们代码。方案分两步走：

1. 一行 `_putenv_s` 关掉 Loader 噪音；
2. 我们自己 6 个散落的 `cout/cerr/fprintf` 收束到统一的 `Log` 门面，带级别 + tag + 时间戳 + 颜色。

改动总量：新增约 120 行（Log 模块），替换约 20 行现有打印，共 8 个文件涉及。
