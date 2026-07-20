# VulkanLab Runtime Control 使用说明

> Status: Current
> Last verified: 2026-07-20
> Verified against: `4bcabe9`

Runtime Control 通过 Windows Named Pipe 控制已经运行的 VulkanLab。它面向本机开发、诊断和自动化，可以查询状态、加载场景、设置相机和 Shader、等待渲染稳定、异步截图并安全退出程序。`scene.list.entries[]` 同时返回稳定 scene ID、Catalog profile ID 和该 profile 的纹理限制。

Runtime Control 默认关闭。启用时必须显式传入 `--runtime-control`；Named Pipe 拒绝远程客户端，不开放网络端口。

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

### 渲染状态与稳定等待

```powershell
.\VulkanLabCtl.exe render status
.\VulkanLabCtl.exe render wait
.\VulkanLabCtl.exe render wait --stable-frames 8 --timeout-ms 30000
```

`render.status --json` 返回：

- 当前 scene、scene generation 和最新 load operation；
- submitted/completed frame serial 与累计 presented frame 数；
- 待上传 texture/mesh、in-flight upload batch；
- capture queue 计数和 capture capability；
- GUI 可见性、窗口最小化、swapchain recreate 和 rendering 状态。

`render.wait` 不是服务端阻塞命令。控制工具反复请求 `render.status` 和必要的 `load.status`，要求同一 generation 已完成加载、pending upload 为 0、窗口可渲染，并观察指定数量的新 presented frames。默认等待 8 帧、超时 30 秒；超时返回 `render_wait_timeout`，持续最小化时返回 `window_not_rendering`。

### 异步截图

```powershell
.\VulkanLabCtl.exe capture screenshot suite\viking.png --no-gui
.\VulkanLabCtl.exe capture screenshot suite\viking-ui.png --include-gui
.\VulkanLabCtl.exe capture status 1
.\VulkanLabCtl.exe capture status 1 --json
.\VulkanLabCtl.exe capture cancel 1
```

`capture screenshot` 校验请求后立即返回 task ID，不等待未来帧、GPU 完成或 PNG 编码。调用者应轮询 `capture status`，直到 `terminal=true`。状态包括 `Queued`、`Recording`、`WaitingForGpu`、`Encoding`、`Cancelling`、`Completed`、`Cancelled` 和 `Failed`。

完成结果包含：

- width、height、swapchain format 和 frame serial；
- capture root 下的最终绝对 output path；
- PNG SHA-256；
- recording、GPU wait、CPU copy、encode 和 total timing。

截图路径必须是 capture root 下的非空相对 `.png` 路径。绝对路径、`..` 逃逸、其他扩展名和解析后落在根目录外的路径都会以 `invalid_capture_path` 拒绝。PNG 先写临时文件再原子发布；取消或失败不会留下最终文件。

开发运行默认 capture root 位于 runtime 旁的 `artifacts/captures/`，可通过 `--capture-root <path>` 覆盖。Cooked package 不创建 CaptureService，也不允许覆盖 capture root，因此截图命令返回 `capture_disabled`。设备或 swapchain 不支持 8-bit RGBA/BGRA transfer-source 时返回 `capture_unsupported`。

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

这些写操作只在 `--asset-mode ondemand` 下可用。ReadOnly/CookedOnly 返回 `asset_import_disabled`；查询 Catalog、状态和 cache 仍可用。Runtime Control 不接收任意本地模型路径，注册新源文件仍使用 ImGui 导入器或 `VulkanLabAssetTool catalog add`。

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
