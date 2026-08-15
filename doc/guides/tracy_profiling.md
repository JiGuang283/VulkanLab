# Tracy 性能分析

> Status: Current
> Last verified: 2026-08-15
> Verified against: `62f6cc4`

VulkanLab 使用 Tracy v0.13.1 提供 CPU 与 Vulkan GPU 统一时间线。Tracy 只在专用构建中启用，采用 on-demand、localhost-only 模式；普通 Debug、Release、dev-fast、runtime 和 Cooked package 均不启动 Tracy 网络线程。

现有 `GpuPassProfiler` 继续提供 ImGui 和 Runtime Control 中的轻量 Pass 毫秒数。Tracy 用于定位线程空洞、长帧、加载流水线和 GPU Pass 内部关系，二者互不替代。

## 安装

首次克隆后初始化固定版本 submodule：

```powershell
git submodule update --init --recursive external/tracy
```

安装官方 Windows Profiler 工具：

```powershell
.\tools\setup\Install-TracyProfiler.ps1
```

脚本下载官方 `windows-0.13.1.zip`，校验固定 SHA-256，并安装到 Git 忽略目录：

```text
external/tools/tracy/0.13.1/
  tracy-profiler.exe
  tracy-capture.exe
  tracy-csvexport.exe
```

Profiler 工具、`.tracy` capture 和导出的 CSV 都不进入 Cooked package，也不应提交到仓库。

## 构建与启动

```powershell
cmake --preset windows-msvc-tracy
cmake --build --preset windows-msvc-tracy
```

该 preset 是 Debug 配置，启用 Editor、Runtime Control、Capture、Asset Authoring、Validation、Debug Utils、现有 GPU profiler 和 Tracy，同时构建 VulkanLab、VulkanLabCtl 与 AssetTool。

从仓库根目录启动：

```powershell
.\build\windows-msvc-tracy\Debug\VulkanLab.exe `
  --runtime-control `
  --runtime-control-pipe tracy `
  --validation off
```

`--validation off` 不是必需条件，只用于减少性能采集中的 Validation 开销。需要分析 Validation 本身时可以显式使用其他 profile，但结果不能与无 Validation 基线直接比较。

启动 Profiler GUI：

```powershell
.\external\tools\tracy\0.13.1\tracy-profiler.exe
```

连接 `127.0.0.1:8086`。客户端不会广播，也不接受远程连接；Profiler 连接前不会收集 zone 数据。

## 命令行采集

不打开 GUI 也可以保存固定时长 capture：

```powershell
New-Item -ItemType Directory -Force artifacts\tracy

.\external\tools\tracy\0.13.1\tracy-capture.exe `
  -o artifacts\tracy\renderer-smoke.tracy `
  -a 127.0.0.1 `
  -s 10
```

导出 CPU 汇总或 GPU 事件：

```powershell
.\external\tools\tracy\0.13.1\tracy-csvexport.exe `
  -f "Application Frame" artifacts\tracy\renderer-smoke.tracy

.\external\tools\tracy\0.13.1\tracy-csvexport.exe `
  -g -f "MainForward" artifacts\tracy\renderer-smoke.tracy
```

`-g` 输出每次 GPU zone 事件，不是聚合统计。分析 Main Sponza 加载通常只需在发出 `scene load` 前开始 6 到 10 秒采集；长时间稳定帧采集会迅速增加 trace 体积。

让 `tracy-capture.exe` 自然保存并退出后再关闭 VulkanLab。强制终止正在连接的 capture 客户端可能使该渲染器进程无法立即接受下一次 Tracy 连接；此时重新启动 Tracy 构建即可恢复干净会话。

## 时间线内容

CPU 线程使用稳定名称：

- `Main`
- `ScenePrepare`
- `EnvironmentPrepare`
- `AssetImport`
- `CaptureEncode`

主要 CPU zone 包括 frame、Runtime command、UI、RenderView、RenderQueue、frame fence、acquire/submit/present、swapchain resize、glTF parse/material/mesh/hierarchy、AssetRepository/ModelGpuBuilder、staging/upload、环境加载、截图编码和 pipeline cache miss。

Vulkan GPU zone 由 RenderGraph 节点和少量节点内阶段生成。实际列表随活动功能裁剪；Renderer Smoke Scene 的基础帧以及打开高级功能后的代表性结构为：

```text
DirectionalShadow/Cascade0..3
PointShadow/LightN/FaceN
SpotShadow/LightN
SurfacePrepass
HiZBuild/MipN
OcclusionCull/Dispatch
Atmosphere/*
SkyBackground
MainForward
  Opaque
  Transparent
SSAO/* | GTAO/* | CACAO
SSR/* | SSGI/* | DDGI/*
TAA
Bloom/Downsample/LN
Bloom/Upsample/LN
ToneMap
Present + UI
  ImGui
ScreenshotCopy
ModelUpload model=<id> profile=<profile> batch=<n>
```

不会为单个 draw、纹理或 mip 创建 zone，避免大场景 trace 膨胀。每次成功 submit/present 后发送 `FrameMark`。draw count、pipeline count、加载进度、staging bytes 和 VMA allocation/block bytes 作为 plot；VMA 只在 Profiler 已连接时每秒采样一次。

Tracy Vulkan context 使用 graphics queue。初始化 context 允许官方实现执行一次性同步；帧循环与上传路径没有为 Tracy 新增 queue/device idle。每帧在现有 command buffer 中调用一次 `TracyVkCollect()`，不使用 `WAIT_BIT`。graphics queue 不支持 timestamp 时 CPU profiling 仍可用，但 GPU timeline 会被禁用。

## 状态检查

```powershell
.\build\windows-msvc-tracy\Debug\VulkanLabCtl.exe `
  --pipe tracy --json info
```

检查：

```text
result.build.features.tracy
result.diagnostics.tracy.compiled
result.diagnostics.tracy.version
result.diagnostics.tracy.connected
result.diagnostics.tracy.gpuAvailable
result.diagnostics.tracy.connectionMode
```

Tracy preset 应报告 `compiled=true`、`version=0.13.1`、`connectionMode=on-demand-localhost`。普通构建报告 `compiled=false`、`connectionMode=disabled`。编辑器的 `Diagnostics -> Performance` 也显示连接和 GPU 可用状态。

## 采集基线

实现验收时在 RTX 4060 Laptop GPU 上完成了两类 capture：

- Renderer Smoke Scene：CPU frame/renderer zone 与 DirectionalShadow、PointShadow、MainForward、ToneMap GPU zone 可导出。
- Main Sponza 2048 的旧 Stage 2A 基线为：worker prepare 约 783 ms，旧 `SceneGpuBuilder::pump` 分布在 77 次主线程调用，5 个 `SceneUpload` batch 出现在同一 GPU timeline；本次完整场景加载约 1.1 秒。Stage 2 之后应以 `ModelGpuBuilder::pump` 和 `ModelUpload` label 重新采集，不直接横向比较旧 zone 名称。

这些数字只证明链路可用，不是跨机器性能目标。后续性能结论必须记录 GPU、驱动、分辨率、场景/profile、Shader、commit 和 capture 版本。
