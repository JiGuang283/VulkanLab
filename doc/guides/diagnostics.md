# 诊断与自动化启动配置

> Status: Current
> Last verified: 2026-07-20
> Verified against: `a25f8ad`

本文说明当前可用的构建诊断信息、确定性启动参数、独立 Runtime Control endpoint 和截图输出。自动视觉回归 runner 尚未完成，相关内容在文末明确标记为 Pending。

## CMake Presets

Windows MSVC 开发环境提供以下 presets：

```text
windows-msvc-debug
windows-msvc-release
windows-msvc-test
```

推荐使用：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-test

cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
```

Debug 和 Release 分别写入 `build/windows-msvc-debug/` 与 `build/windows-msvc-release/`，不会共享生成的 Shader 或 BuildInfo。

## BuildInfo

每次构建都会生成当前二进制的 BuildInfo，包含：

- Git revision 和 tracked-file dirty 状态；
- Debug/Release configuration；
- compiler 及版本；
- Vulkan SDK 版本；
- `glslc` 版本。

没有 Git 或对应工具时字段明确返回 `unknown`。启用 Runtime Control 后可读取完整信息：

```powershell
.\VulkanLab.exe --runtime-control
.\VulkanLabCtl.exe --json info
```

响应中的 `result.build` 是当前运行二进制的构建信息，不是控制工具自身的信息。

## 确定性启动

完整示例：

```powershell
.\VulkanLab.exe `
  --runtime-control `
  --runtime-control-pipe diagnostic_01 `
  --automation `
  --window-size 800x600 `
  --fixed-delta 0.016666667 `
  --no-gui `
  --capture-root .\artifacts\captures
```

- `--automation`：禁用用户相机移动输入，并使窗口不可调整大小。
- `--runtime-control-pipe SUFFIX`：把控制 endpoint 隔离为 `\\.\pipe\VulkanLab.SUFFIX`；最长 64 个字符，只允许 ASCII 字母、数字、`-` 和 `_`。
- `--window-size WIDTHxHEIGHT`：设置窗口和 swapchain 初始尺寸，并使窗口不可调整大小。范围为每轴 `1..16384`。
- `--fixed-delta SECONDS`：Scene 的 `dt` 和 simulation time 按固定步长推进，不再以 wall clock 作为 Scene update 输入。范围为 `(0, 1]`。
- `--no-gui`：不创建 ImGui context，也不记录 GUI draw commands。
- `--capture-root PATH`：覆盖诊断输出根目录；相对路径以启动时工作目录解析。

这些参数可独立使用。普通启动不启用 automation、不固定 delta，窗口保持可调整，GUI 保持可见，因此默认交互行为不变。

启用 Runtime Control 后，`info` 响应的 `result.diagnostics` 会回显最终配置，包括窗口尺寸、是否可调整、GUI 状态、fixed delta、pipe suffix 和规范化后的 capture root。自动化进程应使用唯一 suffix，避免与人工运行或并行测试共享默认 endpoint。

## 输出与限制

开发运行默认输出根目录为 executable 旁的 `artifacts/captures/`。显式 `--capture-root` 不能用于 cooked package，避免交付包写入任意外部位置。`capture.screenshot` 只接受该根目录下的相对 `.png` 路径，并以临时文件加原子替换发布结果。

当前已经提供：

- `camera.get/set`、`render.status` 和客户端 `render wait`；
- `capture.screenshot/status/cancel` 与异步 PNG 输出；
- `--runtime-control-pipe <suffix>` 和多实例隔离；
- `system.info.protocolVersion = 3` 和按实例声明的 capability 列表。

以下能力仍为 **Pending**：

- `VulkanLabRenderTest` runner；
- PNG comparator、diff/heatmap 和 JSON/JUnit 报告；
- golden baseline 的显式 approve/update 流程。

截图是异步任务。`capture.screenshot` 返回 task ID 后，调用者必须轮询 `capture.status` 并确认 `state=Completed`，不能只根据目标文件是否暂时存在判断成功。完整命令和协议见 [Runtime Control](runtime_control.md)。

## 参数校验

`--help` 在创建 Window/Vulkan 前返回。非法尺寸、非法 fixed delta、缺失参数或未知参数同样在初始化前失败并返回非零退出码：

```powershell
.\VulkanLab.exe --help
.\VulkanLab.exe --window-size 800x600 --fixed-delta 0.016666667
```
