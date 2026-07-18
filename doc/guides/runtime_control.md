# VulkanLab Runtime Control 使用说明

> Status: Current
> Last verified: 2026-07-18
> Verified against: `0516951`

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

VulkanLab 使用相对路径读取 Shader、模型和纹理，因此应从对应输出目录启动。例如：

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
- `info`：显示协议版本、当前场景、纹理限制和 Shader。
- `quit`：收到成功响应后安全关闭 VulkanLab。

### 场景

```powershell
.\VulkanLabCtl.exe scene list
.\VulkanLabCtl.exe scene current
.\VulkanLabCtl.exe scene load "Main Sponza"
.\VulkanLabCtl.exe scene reload
```

场景名称使用完整显示名称匹配，不区分 ASCII 大小写。名称中包含空格时必须加引号。可选模型文件不存在时，对应场景不会注册，也不会出现在 `scene list` 中。

`scene load` 和 `scene reload` 会等待场景加载完成，然后输出本次 LoadStats 摘要。例如：

```text
scene: Main Sponza
success: true
load: 40737.21 ms
textures: 75, meshes: 405, upload: 1335.58 MiB
legacy submits/waits: 0/0
batch submits/fence waits: 13/13
```

### 纹理限制

```powershell
.\VulkanLabCtl.exe texture-limit get
.\VulkanLabCtl.exe texture-limit set 2048
.\VulkanLabCtl.exe texture-limit set 1024
.\VulkanLabCtl.exe texture-limit set 512
.\VulkanLabCtl.exe texture-limit set full
```

修改纹理限制会同步重载当前场景。`full` 表示不限制 glTF 纹理尺寸，大场景可能因此耗尽显存。

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
- legacy submit、queue wait、batch submit 和 fence wait 数量。
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

- v1 只支持 Windows，并使用固定管道 `\\.\pipe\VulkanLab`。
- Runtime Control 默认关闭，只有 `VulkanLab.exe --runtime-control` 会创建管道和控制线程。
- 同一时间只支持一个 VulkanLab 控制实例和一个在途请求。
- 场景加载仍在渲染主线程同步执行。控制工具会等待加载完成，此时窗口仍可能显示无响应。
- 同步加载期间不能执行新的状态查询、切换或取消命令。
- Runtime Control 不支持截图、材质编辑、相机控制或远程访问。
- `models/main_sponza` 和 `models/pkg_a_curtains` 是本地可选资产，不提交到仓库。

## 常见问题

### 提示 runtime control pipe is unavailable

确认 VulkanLab 使用 `--runtime-control` 启动，并已完成初始场景加载。控制服务在应用初始化完成后开始监听。如果另一个 VulkanLab 实例已经占用固定管道，新实例会继续运行，但 Runtime Control 会被禁用。未开启控制服务时，`VulkanLabCtl` 返回退出码 `2`。

### scene load 长时间没有输出

这是当前同步完成语义。命令会在文件读取、图片解码、纹理缩放和 GPU 上传全部结束后返回。Main Sponza 2048 在当前基线上约需要 40 秒。

### scene_not_found

先执行：

```powershell
.\VulkanLabCtl.exe scene list
```

确认名称和模型资产是否存在。场景匹配不区分大小写，但必须提供完整显示名称。

### 第二条命令连接超时

v1 只有一个管道实例。前一条长命令执行期间，不要并行发送其他命令。等待前一条命令完成后重试。

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
  "result": {}
}
```

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
