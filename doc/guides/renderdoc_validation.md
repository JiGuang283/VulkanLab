# RenderDoc 与 Vulkan Validation

> Status: Current
> Last verified: 2026-08-15
> Verified against: `62f6cc4`

本文说明 VulkanLab 的 RenderDoc 外部抓帧工作流、GPU 事件标签、Vulkan 对象命名和 Validation Profiles。当前实现不链接 RenderDoc SDK，也不提供进程内触发抓帧；抓帧由 RenderDoc UI 启动程序或注入已运行进程。

## Validation Profiles

开发运行默认使用 `core`。可以在创建窗口和 Vulkan 前通过启动参数选择：

```powershell
.\VulkanLab.exe --validation off
.\VulkanLab.exe --validation core
.\VulkanLab.exe --validation sync
.\VulkanLab.exe --validation gpu
```

| Profile | 行为 |
|---|---|
| `off` | 应用不显式加载 `VK_LAYER_KHRONOS_validation`。非 Cooked 构建仍会启用可用的 Debug Utils，因此 RenderDoc 标签和对象名称仍可使用。 |
| `core` | 标准 Khronos Validation，默认配置。 |
| `sync` | Core Validation 加 Synchronization Validation。 |
| `gpu` | GPU Assisted Validation 与 Reserve Binding Slot；关闭 Core Checks，不同时启用 Sync。该模式至少请求 Vulkan 1.1。 |

`sync` 或 `gpu` 缺少 `VK_EXT_validation_features` 时回退到 `core`；Layer 缺失时继续回退到 `off`。GPU profile 遇到 Vulkan 1.0 loader 也会回退到 `core`。普通交互启动允许回退并记录原因，自动 smoke 将任何回退视为失败。

Cooked package 保留 requested profile 供诊断，但 actual 强制为 `off`，同时关闭 Debug Utils，避免交付包依赖开发层。

启用 Runtime Control 后可读取实际状态：

```powershell
.\VulkanLab.exe --runtime-control --validation sync
.\VulkanLabCtl.exe --json info
```

相关字段位于 `result.diagnostics.validation`：

```json
{
  "requested": "sync",
  "actual": "sync",
  "layerAvailable": true,
  "validationFeaturesAvailable": true,
  "debugUtilsAvailable": true,
  "debugUtilsEnabled": true,
  "fallbackReason": null,
  "warningCount": 0,
  "errorCount": 0
}
```

`warningCount` 可以包含驱动、性能和接口使用警告；自动 smoke 的硬性条件是 profile 不回退且 `errorCount == 0`。

## RenderDoc 启动抓帧

在 RenderDoc 的 **Launch Application** 中填写：

- Executable Path：`<repo>\build\windows-msvc-dev-fast\Debug\VulkanLab.exe`
- Working Directory：`<repo>\build\windows-msvc-dev-fast\Debug`
- Command-line Arguments：建议先用 `--validation off`，需要同时查看 Core Validation 时改为 `--validation core`
- Capture API：Vulkan

工作目录使用运行目录最直观；VulkanLab 本身会通过 executable 旁的 project locator 找到源码项目，不依赖当前工作目录。

启动后按 RenderDoc 的 Capture Frame 热键抓取。`--validation off` 只关闭 Validation Layer，不关闭 Debug Utils，因此抓帧中仍应出现事件标签和对象名称。

也可以先从终端启动：

```powershell
cd build\windows-msvc-dev-fast\Debug
.\VulkanLab.exe --validation off
```

然后在 RenderDoc 中使用 **File -> Attach to Running Instance** 选择该进程。注入模式适合复现已经操作到特定场景的状态；启动模式更适合可重复抓帧。

## Event Browser

事件层级由编译后的 RenderGraph group/node 自动生成。默认功能和设备能力会裁剪未使用节点；启用相应功能后可看到以下代表性结构：

```text
Frame N
  DirectionalShadow
    Cascade 0..3
  PointShadow
    Light 0..3
      Face 0..5
  SpotShadow
    Light 0..3
  SurfacePrepass
  HiZBuild/Mip0..N
  OcclusionCull/Dispatch
  SSAO | GTAO | CACAO
  Atmosphere
  SkyBackground
  MainForward
    Opaque
    Transparent
  SSR | SSGI | DDGI
  TAA
  Bloom
  ToneMap
  Present + UI
    ImGui
  ScreenshotCopy
```

只有实际执行截图复制时才会出现 `ScreenshotCopy`。模型和其他增量上传使用稳定的任务、模型与批次标签，例如：

```text
ModelUpload model=<model-id> profile=<profile-id> batch=<n>
SceneUpload task=<id> batch=<n>
```

Shadow label 只细分 cascade、light 和 cubemap face，不会为每个 draw call 添加标签，避免 Main Sponza 的 Event Browser 被数百个重复节点淹没。检查 Point Shadow 时，binding 7 应显示 24-layer cube-compatible depth image 的 Cube Array view；每个 attachment draw使用单 layer 2D view，fragment depth 是归一化径向距离。Spot binding 8 应显示四层 2D Array。

主要 Vulkan 对象使用 `/` 分层命名，例如：

```text
Frame/0/CommandBuffer
RenderTarget/HDR Color/Frame0
Scene/Main Sponza/Texture/42/Image42/Normal
Pipeline/MainForward/pbr-lite-normal-mapped/Opaque/CullBack
RenderTarget/ViewportColor/Frame0
Capture/Task1/ReadbackBuffer
```

名称仅用于诊断。`PipelineConfig::debugName` 不参与 Pipeline Cache equality/hash，不会因为名称不同创建额外 pipeline。

## Validation Smoke

先构建目标配置，然后从仓库根目录运行：

```powershell
.\tools\validation\Run-ValidationSmoke.ps1 -Profile core
.\tools\validation\Run-ValidationSmoke.ps1 -Profile sync
.\tools\validation\Run-ValidationSmoke.ps1 -Profile gpu
```

Release 示例：

```powershell
.\tools\validation\Run-ValidationSmoke.ps1 `
  -Profile sync `
  -BuildDirectory build/windows-msvc-release `
  -Configuration Release
```

脚本会使用唯一 Named Pipe，启动 automation 模式，加载 `Renderer Smoke Scene`，切换到 `PBR-lite NormalMapped`，运行时 resize 到 `1024x720`，等待稳定帧，截图并检查最终 Validation 状态。该场景只依赖 engine primitive，适合 core/sync/GPU-AV 的快速诊断；GPU-AV 不加载 Main Sponza。

输出位于：

```text
artifacts/validation-smoke/<profile>-<pid>/
```

Runtime Control 的 `window.resize` 仅在 `--automation` 模式可用，尺寸范围为 `1..16384`。普通交互实例返回 `automation_required`。

## Vulkan Configurator

运行上述 smoke 前，关闭 Vulkan Configurator 的全局 preset。全局 Layer 设置会叠加或覆盖进程请求，可能造成：

- requested/actual 与程序配置不一致；
- SyncVal 与 GPU-AV 被意外同时启用；
- 同一 Validation feature 被外部配置重复注入；
- 性能、消息数量和失败条件无法复现。

需要临时使用 Vulkan Configurator 验证独立问题时，应记录 preset 内容，并在完成后恢复为关闭状态。日常自动化以 `--validation` 参数作为唯一配置来源。

## 限制

- 未接入 RenderDoc in-application API，Runtime Control 不能触发 `.rdc` 抓帧。
- Debug Utils 不可用时对象命名和标签自动变为空操作，渲染仍可继续。
- Validation profile 只在启动时生效，不能通过 ImGui 或 Runtime Control 动态切换。
- Stage 2A 不提交 RenderDoc SDK、二进制文件或抓帧文件。
