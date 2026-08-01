# VulkanLab Runtime Control 使用说明

> Status: Current
> Last verified: 2026-08-01
> Verified against: Scene Authoring Stage 0-1 working tree

Runtime Control 通过 Windows Named Pipe 控制已经运行的 VulkanLab。它面向本机开发、诊断和自动化，可以查询状态、加载模型预览和环境、设置相机、Shader 与渲染参数、等待渲染稳定、异步截图并安全退出程序。Stage 1 继续保留 `scene.*` 协议名称；`scene.list.entries[]` 是 Catalog model previews，同时返回 `kind: "modelPreview"`、稳定 `modelId`、兼容 `sceneId`、Catalog profile ID 和该 profile 的纹理限制。原生 SceneDocument 尚不出现在此接口。

Runtime Control 默认关闭。启用时必须显式传入 `--runtime-control`；Named Pipe 拒绝远程客户端，不开放网络端口。

该服务还必须以 `VKL_ENABLE_RUNTIME_CONTROL=ON` 编译。全功能 Debug/Release 和 `windows-msvc-dev-fast` 包含服务端；`windows-msvc-runtime` 不包含服务端或 VulkanLabCtl，传入 `--runtime-control` 会在 Vulkan 初始化前报错。服务端存在但 Capture 或 Asset Authoring 被独立裁剪时，相关协议方法保留并返回 `feature_not_compiled`。

## 构建与启动

使用 presets 构建：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug

cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
```

每个配置都会生成渲染器和控制客户端：

```text
build/windows-msvc-debug/Debug/VulkanLab.exe
build/windows-msvc-debug/Debug/VulkanLabCtl.exe
build/windows-msvc-release/Release/VulkanLab.exe
build/windows-msvc-release/Release/VulkanLabCtl.exe
```

启动默认 endpoint：

```powershell
cd build\windows-msvc-debug\Debug
.\VulkanLab.exe --runtime-control
```

在另一个终端连接：

```powershell
.\VulkanLabCtl.exe ping
```

输出 `pong` 表示控制通道可用。没有传入 `--runtime-control` 时不会创建命令队列、管道或控制线程，客户端连接失败并返回退出码 `2`。

## 多实例隔离

默认 endpoint 为 `\\.\pipe\VulkanLab`。自动化和并行测试应为每个实例指定不同 suffix：

```powershell
.\VulkanLab.exe --runtime-control --runtime-control-pipe suite_a
.\VulkanLabCtl.exe --pipe suite_a ping
```

这组命令使用 `\\.\pipe\VulkanLab.suite_a`。suffix 最长 64 个字符，只允许 ASCII 字母、数字、`-` 和 `_`。渲染器和客户端必须使用同一个 suffix；省略 suffix 时保持默认 endpoint 的兼容行为。

## 命令参考

`--pipe <suffix>` 和 `--json` 是控制工具的全局选项，可放在命令前后。默认输出适合人工阅读，`--json` 输出完整协议响应。

### 程序与场景

```powershell
.\VulkanLabCtl.exe ping
.\VulkanLabCtl.exe info
.\VulkanLabCtl.exe scene list
.\VulkanLabCtl.exe scene current
.\VulkanLabCtl.exe scene load "Main Sponza"
.\VulkanLabCtl.exe scene reload
.\VulkanLabCtl.exe --no-wait scene load "Main Sponza"
.\VulkanLabCtl.exe load status
.\VulkanLabCtl.exe load status 12
.\VulkanLabCtl.exe load cancel 12
.\VulkanLabCtl.exe quit
```

场景名称使用 Catalog 的完整 display name，不区分 ASCII 大小写。`scene list --json` 同时返回兼容的 `scenes` 名称数组和带稳定 `id`、`profileId`、`available`、`source` 的 `entries`。

`scene.load` 和 `scene.reload` 立即返回 task ID。`VulkanLabCtl` 默认每 100 ms 轮询 `load.status`，直到整个 import/prepare/upload operation 完成；`--no-wait` 可关闭客户端等待。任务状态包括 `Queued`、`PreparingCpu`、`ReadyForUpload`、`Uploading`、`WaitingForGpu`、`Completed`、`Cancelling`、`Cancelled` 和 `Failed`。

`quit` 的成功响应会先写回并 flush，随后 Application 才退出主循环、停止管道和截图 worker。

### 相机

```powershell
.\VulkanLabCtl.exe camera get
.\VulkanLabCtl.exe camera set `
  --position 2,2,2 `
  --yaw -135 `
  --pitch -30
```

`camera.set` 要求同时提供 position、yaw 和 pitch。所有值必须是有限浮点数；`NaN` 和无穷值会在客户端或服务端被拒绝。设置和 ImGui/输入共用同一个 `Camera` 实例，查询还会返回当前 near/far clip plane。

### 自动化窗口尺寸

```powershell
.\VulkanLabCtl.exe window resize 1024 720
```

`window.resize` 只在渲染器以 `--automation` 启动时可用，合法范围为每个维度 `1..16384`。成功后 GLFW resize callback 会触发现有 swapchain 重建流程；普通交互实例返回 `automation_required`。启用时 `system.info.capabilities` 包含 `window_resize`，协议版本保持 `3`。

### 渲染状态与稳定等待

```powershell
.\VulkanLabCtl.exe render status
.\VulkanLabCtl.exe render wait
.\VulkanLabCtl.exe render wait --stable-frames 8 --timeout-ms 30000
```

`render.status --json` 返回：

- 当前 scene、scene generation 和最新 load operation；
- submitted/completed frame serial 与累计 presented frame 数；
- 最近一个已完成 frame 的 `gpuTimings`，包含 available、frameSerial、DirectionalShadow/Skybox/MainForward、可选 Bloom、ToneMap、Present + UI 分项与 totalMs；
- 待上传 texture/mesh、in-flight upload batch；
- 当前选择和已发布的 environment，以及环境加载任务；
- 当前 Scene 的 Directional/Point/Spot 数量、实际上传数量和超限忽略数量；
- capture queue 计数，以及 Workspace/Viewport 各自的 capture capability；
- `viewport` 的模式、可见/hover 状态、display extent、render extent 和 resize pending；
- GUI 可见性、窗口最小化、swapchain recreate 和 rendering 状态。

`render.wait` 不是服务端阻塞命令。控制工具反复请求 `render.status` 和必要的 `load.status`，要求同一 generation 已完成加载、pending upload 为 0、窗口可渲染，并观察指定数量的新 presented frames。默认等待 8 帧、超时 30 秒；超时返回 `render_wait_timeout`，持续最小化时返回 `window_not_rendering`。

GPU timing 在对应 frame slot 的正常 fence 已完成后读取，不使用 query `WAIT_BIT`，也不增加 queue/device idle。不支持 graphics timestamp 的设备返回 `available=false`；启动后的最初两个 frame 也可能暂时没有已完成结果。

### 异步截图

```powershell
.\VulkanLabCtl.exe capture screenshot suite\viking.png --no-gui
.\VulkanLabCtl.exe capture screenshot suite\viking-ui.png --include-gui
.\VulkanLabCtl.exe capture status 1
.\VulkanLabCtl.exe capture status 1 --json
.\VulkanLabCtl.exe capture cancel 1
```

`capture screenshot` 校验请求后立即返回 task ID，不等待未来帧、GPU 完成或 PNG 编码。调用者应轮询 `capture status`，直到 `terminal=true`。状态包括 `Queued`、`Recording`、`WaitingForGpu`、`Encoding`、`Cancelling`、`Completed`、`Cancelled` 和 `Failed`。

`--include-gui` 从最终 Swapchain 截取完整 Workspace；`--no-gui` 从 per-frame
Viewport Color 截取纯场景，输出尺寸是实际 Viewport render extent。后者不会丢弃
当前 ImGui frame，因此交互窗口不会闪烁。

完成结果包含：

- source（`Workspace` 或 `Viewport`）、width、height、实际 image format 和 frame serial；
- capture root 下的最终绝对 output path；
- PNG SHA-256；
- recording、GPU wait、CPU copy、encode 和 total timing。

截图路径必须是 capture root 下的非空相对 `.png` 路径。绝对路径、`..` 逃逸、其他扩展名和解析后落在根目录外的路径都会以 `invalid_capture_path` 拒绝。PNG 先写临时文件再原子发布；取消或失败不会留下最终文件。

开发运行默认 capture root 位于 runtime 旁的 `artifacts/captures/`，可通过 `--capture-root <path>` 覆盖。Cooked package 不创建 CaptureService，也不允许覆盖 capture root，因此截图命令返回 `capture_disabled`。Viewport Color 或 Swapchain source 不支持 8-bit RGBA/BGRA transfer-source 时返回 `capture_unsupported`；另一个来源仍可独立保持可用。

常见截图错误码还有 `capture_queue_full`、`capture_not_found`、`capture_not_cancellable` 和 `capture_failed`。截图路径不会调用 `vkQueueWaitIdle()` 或 `vkDeviceWaitIdle()`；完成状态由正常 frame fence 的 submission serial 推进。

### Shader 与纹理限制

```powershell
.\VulkanLabCtl.exe shader list
.\VulkanLabCtl.exe shader current
.\VulkanLabCtl.exe shader set "PBR-lite NormalMapped"

.\VulkanLabCtl.exe texture-limit get
.\VulkanLabCtl.exe texture-limit set 1024
.\VulkanLabCtl.exe texture-limit set full
```

Shader 名称使用完整 display name，不区分 ASCII 大小写。开发模式修改纹理限制会触发当前 glTF 场景的新加载任务；控制工具默认等待完成。允许值为 `full`、`512`、`1024` 和 `2048`。CookedOnly profile 固定，修改返回 `texture_limit_locked`。

### Environment

```powershell
.\VulkanLabCtl.exe environment list
.\VulkanLabCtl.exe environment current
.\VulkanLabCtl.exe environment set "Studio"
.\VulkanLabCtl.exe environment set None
.\VulkanLabCtl.exe environment reload
.\VulkanLabCtl.exe --no-wait environment set "Studio"
```

`environment list` 返回 `None` 和 Catalog environments，并报告 profile、派生 artifact 状态及设备是否支持 float IBL resources。`environment set` 接受不区分 ASCII 大小写的完整 display name 或稳定 ID；`None` 取消选择并立即回到 fallback resources。选择环境不会自动打开 IBL 或 Skybox。

加载是异步操作。默认客户端拿到 task ID 后通过现有 `load status` 等待 worker KTX2 读取、增量 GPU 上传和 descriptor generation 发布完成；`--no-wait` 只返回初始任务。`load status <task-id>` 与 `load cancel <task-id>` 同时识别 Scene 和 Environment 命名空间。加载失败或取消会保留旧的已发布环境。`environment reload` 要求当前已经选择非 None 环境。

### 阴影、IBL、Bloom、曝光与 Tone Mapping

```powershell
.\VulkanLabCtl.exe render-settings get
.\VulkanLabCtl.exe render-settings set --shadows on
.\VulkanLabCtl.exe render-settings set `
  --receiver-bias 0.0015 `
  --constant-bias 1.25 `
  --slope-bias 1.75
.\VulkanLabCtl.exe render-settings set `
  --exposure 1.0 `
  --tone-mapper aces
.\VulkanLabCtl.exe render-settings set `
  --ibl on `
  --skybox on `
  --environment-intensity 1.25 `
  --environment-rotation-deg 90
.\VulkanLabCtl.exe render-settings set `
  --bloom on `
  --bloom-threshold 1.0 `
  --bloom-soft-knee 0.5 `
  --bloom-intensity 0.1
```

`render-settings set` 支持部分更新，并要求至少提供一个选项。`--shadows`、`--ibl`、`--skybox` 和 `--bloom` 接受 `on/off`，`--tone-mapper` 接受 `aces`、`reinhard` 或 `passthrough`。Receiver bias 范围为 `[0, 0.05]`，constant/slope bias 为 `[0, 10]`，exposure 为 `[-10, 10]` EV，environment intensity 为 `[0, 100]`；Bloom threshold、soft knee 和 intensity 分别为 `[0,20]`、`[0,1]` 和 `[0,5]`。CLI 用 degree 表示 rotation，协议字段 `environmentRotationRadians` 使用弧度；服务端将其规范化到一个完整旋转。

Tone Mapping policy 由 Shader Manifest 决定：两个 PBR-lite 和 `Debug IBL Diffuse/Specular` 可配置，Legacy 与其他 Debug variant 强制 PassThrough。Bloom compatibility 也由 Manifest 决定，目前只有两个 PBR-lite variant 支持；设置会保留，但其他 variant 下 `bloomActive=false`。`render-settings get` 返回 `bloomAvailable`、`bloomActive`、`bloomUnavailableReason` 和四个 Bloom 设置。设备不满足 compute/`RGBA16F` storage image 要求时，尝试开启会返回 `bloom_unsupported`。

阴影只影响 PBR-lite 的第一盏方向光，但 `Debug Shadow` 可显示最终 visibility。IBL 只在环境已发布且开关开启时替代 PBR 的 constant ambient；Skybox 开关独立。UI 的 `Render -> Pipeline/Post Processing/Lighting` 与 Runtime Control 修改同一个 `RenderSettings` 对象。

### 派生资产

```powershell
.\VulkanLabCtl.exe asset catalog
.\VulkanLabCtl.exe asset status "Main Sponza"
.\VulkanLabCtl.exe asset import "Main Sponza"
.\VulkanLabCtl.exe --force asset import "Main Sponza"
.\VulkanLabCtl.exe --load-after asset import "Main Sponza"
.\VulkanLabCtl.exe --no-wait asset import "Main Sponza"
.\VulkanLabCtl.exe asset cancel 9223372036854775808
.\VulkanLabCtl.exe asset cache-info
```

这些 scene 写操作只在 `--asset-mode ondemand` 下可用。ReadOnly/CookedOnly 返回 `asset_import_disabled`；查询 Catalog、状态和 cache 仍可用。Runtime Control 不接收任意本地模型/HDR 路径，注册新源文件仍使用 ImGui 导入器或 `VulkanLabAssetTool catalog add`/`catalog add-environment`。Environment 派生缓存的 Build/Rebuild 目前由 Assets UI 或 AssetTool 完成，不由 Runtime Control 自动 bake。

Catalog glTF 的最近验证报告可只读查询；响应最多包含 32 条 issue，完整报告仍从 Assets/Scenes UI 打开：

```powershell
.\VulkanLabCtl.exe asset validation main-sponza
.\VulkanLabCtl.exe --json asset validation sheen-chair
```

响应包含 `Valid/Warnings/Invalid/Stale/Unavailable/Failed/NotChecked/NotApplicable` 状态、Validator 版本、四级计数、renderer extension 诊断和 report path。该方法只接受 Catalog model ID 或完整显示名，不接受原生 SceneDocument ID 或任意外部文件路径；响应同时保留 `sceneId` 并增加 `modelId` 与 `assetKind: "model"`。

### 加载统计

```powershell
.\VulkanLabCtl.exe stats
.\VulkanLabCtl.exe stats --json
```

完整 JSON 包含阶段耗时、资源数量、decode/transcode/upload 字节、KTX2 cache 命中、增量上传同步数据和 VMA 前后快照。没有完成过场景加载时返回 `no_load_stats`。

## 自动化示例

下面的 PowerShell 示例启动隔离实例、等待场景和渲染稳定、截图并退出：

```powershell
$runtime = Resolve-Path .\build\windows-msvc-release\Release
$suffix = "smoke_$PID"
$captureRoot = Join-Path $PWD "artifacts\smoke-$PID"
$app = Start-Process `
  -FilePath "$runtime\VulkanLab.exe" `
  -WorkingDirectory $runtime `
  -ArgumentList @(
    '--runtime-control',
    '--runtime-control-pipe', $suffix,
    '--automation',
    '--window-size', '800x600',
    '--fixed-delta', '0.016666667',
    '--no-gui',
    '--capture-root', $captureRoot
  ) `
  -PassThru

try {
  & "$runtime\VulkanLabCtl.exe" --pipe $suffix ping
  & "$runtime\VulkanLabCtl.exe" --pipe $suffix scene load "Viking Room"
  & "$runtime\VulkanLabCtl.exe" --pipe $suffix camera set `
    --position 2,2,2 --yaw -135 --pitch -30
  & "$runtime\VulkanLabCtl.exe" --pipe $suffix render wait `
    --stable-frames 8 --timeout-ms 30000

  $capture = & "$runtime\VulkanLabCtl.exe" --pipe $suffix --json `
    capture screenshot "suite\viking.png" --no-gui | ConvertFrom-Json
  $taskId = $capture.result.taskId

  do {
    Start-Sleep -Milliseconds 50
    $status = & "$runtime\VulkanLabCtl.exe" --pipe $suffix --json `
      capture status $taskId | ConvertFrom-Json
  } until ($status.result.terminal)

  if (-not $status.ok -or $status.result.state -ne 'Completed') {
    throw "Capture failed: $($status | ConvertTo-Json -Depth 8)"
  }
} finally {
  & "$runtime\VulkanLabCtl.exe" --pipe $suffix quit
  $app.WaitForExit()
}
```

日常 smoke/golden 测试应使用已经固化这段编排的 `VulkanLabRenderTest`；手写脚本仍需自行检查每条命令的 `$LASTEXITCODE`。规格和报告说明见[自动视觉回归](visual_regression.md)。

## 返回码

| 返回码 | 含义 |
|---:|---|
| `0` | 命令成功。 |
| `1` | 已连接渲染器，但命令被拒绝、任务失败或超时。 |
| `2` | 参数、连接或消息协议错误。 |

## 协议摘要

Runtime Control 当前协议版本为 `3`。客户端连接目标 Named Pipe 后，先发送 little-endian `uint32` JSON 字节长度，再发送 UTF-8 JSON；响应使用同样 framing。单条消息最大 64 KiB，每次连接处理一条请求。

```json
{
  "id": 1,
  "method": "capture.screenshot",
  "params": {
    "path": "suite/viking.png",
    "includeGui": false
  }
}
```

成功响应统一为：

```json
{
  "id": 1,
  "ok": true,
  "result": {
    "taskId": 1,
    "state": "Queued",
    "terminal": false
  }
}
```

失败响应统一为：

```json
{
  "id": 1,
  "ok": false,
  "error": {
    "code": "invalid_capture_path",
    "message": "Capture output must be a relative PNG path."
  }
}
```

管道线程只负责 framing、JSON 解析、排队和回写。所有 Scene、Camera、Shader、统计和 Vulkan 相关操作都由 Application 主线程执行；服务端不会用控制命令阻塞等待未来帧。
