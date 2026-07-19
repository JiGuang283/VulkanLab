# 诊断与自动化启动配置

> Status: Current
> Last verified: 2026-07-19
> Verified against: `dca6ddb`

本文说明当前可用的构建诊断信息和确定性启动参数。截图命令、独立 Named Pipe endpoint 和视觉回归 runner 尚未完成；相关内容在文末明确标记为 Pending。

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
  --automation `
  --window-size 800x600 `
  --fixed-delta 0.016666667 `
  --no-gui `
  --capture-root .\artifacts\captures
```

- `--automation`：禁用用户相机移动输入，并使窗口不可调整大小。
- `--window-size WIDTHxHEIGHT`：设置窗口和 swapchain 初始尺寸，并使窗口不可调整大小。范围为每轴 `1..16384`。
- `--fixed-delta SECONDS`：Scene 的 `dt` 和 simulation time 按固定步长推进，不再以 wall clock 作为 Scene update 输入。范围为 `(0, 1]`。
- `--no-gui`：不创建 ImGui context，也不记录 GUI draw commands。
- `--capture-root PATH`：覆盖诊断输出根目录；相对路径以启动时工作目录解析。

这些参数可独立使用。普通启动不启用 automation、不固定 delta，窗口保持可调整，GUI 保持可见，因此默认交互行为不变。

启用 Runtime Control 后，`info` 响应的 `result.diagnostics` 会回显最终配置，包括窗口尺寸、是否可调整、GUI 状态、fixed delta 和规范化后的 capture root。

## 输出与限制

开发运行默认输出根目录为 executable 旁的 `artifacts/captures/`。显式 `--capture-root` 不能用于 cooked package，避免交付包写入任意外部位置。当前阶段只解析并公开该路径，尚不会创建截图文件。

以下能力仍为 **Pending**：

- `render.capture`、`capture.status` 和 PNG 输出：M5/M6；
- `--runtime-control-pipe <suffix>` 和多实例隔离：M6；
- `VulkanLabRenderTest`、图像比较和 golden 管理：M7。

在这些阶段完成前，不应把 capture root 中没有文件视为错误，也不应在脚本中调用尚不存在的 capture 命令。

## 参数校验

`--help` 在创建 Window/Vulkan 前返回。非法尺寸、非法 fixed delta、缺失参数或未知参数同样在初始化前失败并返回非零退出码：

```powershell
.\VulkanLab.exe --help
.\VulkanLab.exe --window-size 800x600 --fixed-delta 0.016666667
```
