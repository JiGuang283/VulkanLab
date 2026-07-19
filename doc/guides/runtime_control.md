# VulkanLab Runtime Control 使用说明

> Status: Current
> Last verified: 2026-07-19
> Verified against: `dca6ddb`

Runtime Control 允许在 VulkanLab 已经运行时，从另一个终端切换场景、纹理限制和 Shader，并读取加载统计或关闭程序。控制接口仅用于本机调试，通过 Windows Named Pipe 通信，不开放网络端口。

Runtime Control 默认关闭。需要外部控制时，必须使用 `--runtime-control` 启动 VulkanLab。

## 构建与启动

构建 Debug：

```powershell
cmake --build build-debug --config Debug
```

构建 Release：

```powershell
cmake --build build --config Release
```

两个目标会生成在相同的配置目录中：

```text
build-debug/Debug/VulkanLab.exe
build-debug/Debug/VulkanLabCtl.exe
build/Release/VulkanLab.exe
build/Release/VulkanLabCtl.exe
```

VulkanLab 通过 executable 旁的 project locator 解析开发资产，因此可从仓库根目录、输出目录或其他工作目录启动。例如：

```powershell
cd build\Release
.\VulkanLab.exe --runtime-control
```

保持 VulkanLab 运行，在另一个终端进入同一目录后使用控制工具：

```powershell
cd build\Release
.\VulkanLabCtl.exe ping
```

输出 `pong` 表示控制通道可用。

## 命令参考

### 程序状态

```powershell
.\VulkanLabCtl.exe ping
.\VulkanLabCtl.exe info
.\VulkanLabCtl.exe quit
```

- `ping`：检查 Named Pipe 是否可连接。
- `info`：显示协议版本、当前场景、纹理限制、Shader、BuildInfo 和诊断启动配置。
- `quit`：收到成功响应后安全关闭 VulkanLab。

### 场景

```powershell
.\VulkanLabCtl.exe scene list
.\VulkanLabCtl.exe scene current
.\VulkanLabCtl.exe scene load "Main Sponza"
.\VulkanLabCtl.exe scene reload
.\VulkanLabCtl.exe --no-wait scene load "Main Sponza"
.\VulkanLabCtl.exe load status
.\VulkanLabCtl.exe load status 12
.\VulkanLabCtl.exe load cancel 12
```

场景名称使用 Catalog 的完整 display name 匹配，不区分 ASCII 大小写。名称中包含空格时必须加引号。可选模型文件不存在时，`scene list --json` 的 `entries` 中仍保留条目并显示 `available=false`；加载该条目返回 `scene_unavailable`。

`scene list --json` 兼容保留 `scenes` 字符串数组，并新增结构化 `entries`，包含稳定 `id`、`name`、`profileId`、`available` 和 `source`。`scene current --json` 同时返回 display name 与稳定 scene ID。`info --json` 还返回 `projectId`、当前 `sceneId`、`cacheRoot`、`assetMode` 和 `cookedPackage`。

协议中的 `scene.load` 和 `scene.reload` 会立即返回 taskId。Ready 场景返回普通 SceneLoad task；OnDemand 缺失时返回最高位为 1 的复合 operation ID，该 ID 先报告 `phase=importing`，再连续报告 `preparing`、`uploading` 和最终状态。VulkanLabCtl 默认每 100 ms 轮询 `load.status`，因此命令行行为仍是等待整个 operation 完成后输出 LoadStats。使用 `--no-wait` 会立即返回；之后可按同一 taskId 查询或取消。

任务状态包括 `Queued`、`PreparingCpu`、`ReadyForUpload`、`Uploading`、`WaitingForGpu`、`Completed`、`Cancelling`、`Cancelled` 和 `Failed`。加载失败或取消时，默认等待的控制命令返回退出码 `1`。

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

- `asset catalog`：列出各场景当前 profile 的 `Ready/Missing/Stale/Invalid/Importing`、manifest 和 blob 统计。
- `asset status [scene]`：查询 display name 或稳定 scene ID；省略时使用 UI 当前选中项。JSON 同时包含最后成功 import task/time、最后访问时间和可选最近失败。
- `asset import <scene>`：确保派生资产 Ready；已有有效缓存时立即完成，缺失时启动独立资产工具。`--force` 强制重编码，`--load-after` 在成功后继续加载 Scene。
- `asset cancel [task-id]`：取消当前或指定资产任务及其完整 `ktx.exe` 子进程树。
- `asset cache-info`：返回实际 cache root、ArtifactIndex 路径/schema、record/Ready 数、受引用 blob 和未引用 blob 的文件数与字节数。兼容字段 `files/bytes` 等于 blob 文件统计。

这些写操作只在 `--asset-mode ondemand` 下可用。`readonly` 和 `cooked-only` 会返回 `asset_import_disabled`；非 Ready 的 `scene.load` 返回 `artifact_not_ready`，不会静默启动编码器。Runtime Control 不接受任意本地导入路径；新源文件注册仍通过 ImGui 文件选择器或 `VulkanLabAssetTool catalog add`。

完成摘要例如：

```text
scene: Main Sponza
success: true
load: 40737.21 ms
textures: 75, meshes: 405, upload: 1335.58 MiB
legacy submits/waits: 0/0
batch submits/fence waits: 13/0
```

### 纹理限制

```powershell
.\VulkanLabCtl.exe texture-limit get
.\VulkanLabCtl.exe texture-limit set 2048
.\VulkanLabCtl.exe texture-limit set 1024
.\VulkanLabCtl.exe texture-limit set 512
.\VulkanLabCtl.exe texture-limit set full
```

开发模式下修改纹理限制会为当前 glTF 场景创建新的异步加载任务；VulkanLabCtl 默认等待该任务完成。`full` 表示不限制 glTF 纹理尺寸，大场景可能因此耗尽显存。CookedOnly 的纹理限制由 package profile 固定，设置其他值返回 `texture_limit_locked`，不会改变当前 scene context。

### Shader Variant

```powershell
.\VulkanLabCtl.exe shader list
.\VulkanLabCtl.exe shader current
.\VulkanLabCtl.exe shader set "PBR-lite NormalMapped"
.\VulkanLabCtl.exe shader set "Debug Occlusion"
```

Shader 名称使用完整显示名称匹配，不区分 ASCII 大小写。切换立即影响后续渲染帧，不会重载场景。

### 加载统计

人类可读摘要：

```powershell
.\VulkanLabCtl.exe stats
```

完整 JSON：

```powershell
.\VulkanLabCtl.exe stats --json
```

`--json` 也可以用于其他命令：

```powershell
.\VulkanLabCtl.exe --json info
.\VulkanLabCtl.exe scene load "Main Sponza" --json
```

完整统计包含：

- 各加载阶段耗时。
- 纹理、Mesh、顶点、索引、材质和对象数量。
- 解码、上传和 staging 字节数。
- worker/CPU prepare/GPU build、逐帧 upload pump 和总 wall time。
- legacy submit、queue wait、batch submit/completion、fence poll/wait 和 peak in-flight 数量。
- VMA 加载前后快照及差值。

## 自动化示例

下面的 PowerShell 脚本启动渲染器、加载 Main Sponza、保存统计并关闭程序：

```powershell
$workDir = Resolve-Path .\build\Release
$app = Start-Process `
    -FilePath "$workDir\VulkanLab.exe" `
    -WorkingDirectory $workDir `
    -ArgumentList '--runtime-control' `
    -PassThru

try {
    Start-Sleep -Seconds 2
    & "$workDir\VulkanLabCtl.exe" texture-limit set 2048
    & "$workDir\VulkanLabCtl.exe" scene load "Main Sponza"
    & "$workDir\VulkanLabCtl.exe" stats --json |
        Set-Content .\main-sponza-stats.json
} finally {
    & "$workDir\VulkanLabCtl.exe" quit
    $app.WaitForExit()
}
```

## 返回码

| 返回码 | 含义 |
|---:|---|
| `0` | 命令成功 |
| `1` | 已连接到 VulkanLab，但命令被拒绝或执行失败 |
| `2` | 命令格式错误、连接失败或协议错误 |

脚本中可通过 `$LASTEXITCODE` 检查结果：

```powershell
.\VulkanLabCtl.exe scene load "Main Sponza"
if ($LASTEXITCODE -ne 0) {
    throw "Scene load failed with exit code $LASTEXITCODE"
}
```

## 行为与限制

- v2 只支持 Windows，并使用固定管道 `\\.\pipe\VulkanLab`。
- Runtime Control 默认关闭，只有 `VulkanLab.exe --runtime-control` 会创建管道和控制线程。
- `system.info.capabilities` 按实例声明能力。OnDemand 包含 asset import/cancel；ReadOnly/CookedOnly 只保留 asset catalog/status。`system.info.cacheRoot` 是本次进程实际使用的共享、override 或 package-local cache。
- Named Pipe 每个连接处理一条短请求；客户端等待通过重复的 `load.status` 请求实现。
- KTX2 import 在独立工具进程执行，glTF CPU prepare 在 worker 执行，GPU build 在主线程按帧推进；各阶段期间仍可 ping、查询、切换 Shader 和取消任务。
- 新的 scene 请求会取消旧 generation，只有最新任务可以发布。
- Runtime Control 不支持截图、材质编辑、相机控制或远程访问。
- cooked package 可直接使用 `--runtime-control`，但会强制 CookedOnly：`asset import` 返回 `asset_import_disabled`，纹理 profile 锁定，`asset status` 和 load/statistics 命令仍可用。
- `models/main_sponza` 和 `models/pkg_a_curtains` 是本地可选资产，不提交到仓库。

## 常见问题

### 提示 runtime control pipe is unavailable

确认 VulkanLab 使用 `--runtime-control` 启动，并已完成初始场景加载。控制服务在应用初始化完成后开始监听。如果另一个 VulkanLab 实例已经占用固定管道，新实例会继续运行，但 Runtime Control 会被禁用。未开启控制服务时，`VulkanLabCtl` 返回退出码 `2`。

### scene load 长时间没有输出

VulkanLabCtl 默认等待任务完成。另开终端执行 `load status`，或改用 `--no-wait` 获取 taskId。Debug 下 Main Sponza 的 PNG 解码和 bilinear resize 仍可能耗时较长，但渲染器主循环和控制接口会继续响应。

### scene_not_found

先执行：

```powershell
.\VulkanLabCtl.exe scene list
```

确认名称和模型资产是否存在。场景匹配不区分大小写，但必须提供完整显示名称。

### 取消后任务仍短暂显示 Cancelling

取消保证任务不会发布，不代表已经提交的 GPU copy 会立即停止。渲染器会轮询对应 fence，完成后释放 staging 和半成品资源，再进入 `Cancelled`。

## 协议摘要

需要自行编写控制客户端时，连接 `\\.\pipe\VulkanLab`，先发送一个 little-endian `uint32` JSON 字节长度，再发送 UTF-8 JSON。单条消息最大 64 KiB。

请求示例：

```json
{
  "id": 1,
  "method": "scene.load",
  "params": {
    "name": "Main Sponza"
  }
}
```

成功响应：

```json
{
  "id": 1,
  "ok": true,
  "result": {
    "taskId": 12,
    "generation": 7,
    "state": "Queued",
    "terminal": false
  }
}
```

Runtime Control v2 支持 `load.status`、`load.cancel` 和 Stage C 的 `asset.*` 方法。`system.info.protocolVersion` 可用于脚本确认协议版本。

失败响应：

```json
{
  "id": 1,
  "ok": false,
  "error": {
    "code": "scene_not_found",
    "message": "Unknown scene"
  }
}
```
