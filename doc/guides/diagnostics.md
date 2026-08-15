# 诊断与自动化启动配置

> Status: Current
> Last verified: 2026-08-15
> Verified against: Bindless Material Resources v1

本文说明当前可用的构建诊断信息、确定性启动参数、独立 Runtime Control endpoint 和截图输出。端到端 smoke/golden 编排见[自动视觉回归](visual_regression.md)。

Vulkan Validation、RenderDoc 标签与对象命名见 [RenderDoc 与 Vulkan Validation](renderdoc_validation.md)。
CPU 与 Vulkan GPU 统一时间线见 [Tracy 性能分析](tracy_profiling.md)。

## CMake Presets

Windows MSVC 开发环境提供以下 presets：

```text
windows-msvc-debug
windows-msvc-release
windows-msvc-dev-fast
windows-msvc-tracy
windows-msvc-runtime
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

`windows-msvc-dev-fast` 保留全部运行时开发功能，但不生成测试、Ctl 或 RenderTest；`windows-msvc-tracy` 在同类 Debug 配置上启用 Tracy 并构建 Ctl；`windows-msvc-runtime` 是只保留渲染功能和 KTX2 读取的精简 Release。完整开关矩阵见[构建与运行](build_and_run.md#编译期功能开关)。

## BuildInfo

每次构建都会生成当前二进制的 BuildInfo，包含：

- Git revision 和 tracked-file dirty 状态；
- Debug/Release configuration；
- compiler 及版本；
- Vulkan SDK 版本；
- `glslc` 版本。
- Editor、Runtime Control、Capture、Asset Authoring、Validation、GPU Debug Utils、GPU Profiling、Tracy 和工具目标的编译状态。

Tracy 运行状态位于 `result.diagnostics.tracy`，包含编译状态、版本、是否已连接、GPU timestamp 可用性和连接模式。该状态由渲染器主线程生成，不依赖 Profiler GUI。

材质绑定状态位于 `result.diagnostics.materialBinding`，包含 requested/active
backend、设备与 Manifest 能力、固定容量、active/retiring/high-water 数量、
descriptor 写入和 slot 复用/耗尽计数。逐帧运行状态也在
`render.status.materials` 返回同一结构。

没有 Git 或对应工具时字段明确返回 `unknown`。启用 Runtime Control 后可读取完整信息：

```powershell
.\VulkanLab.exe --runtime-control
.\VulkanLabCtl.exe --json info
```

响应中的 `result.build` 是当前运行二进制的构建信息，不是控制工具自身的信息。编译能力位于 `result.build.features`。

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

可在同一启动命令中增加 `--validation off|core|sync|gpu` 和
`--material-binding auto|legacy|bindless`。自动化验证应显式指定 Validation
profile，并检查 `system.info.diagnostics.validation.actual` 与 requested 一致；
材质后端对比应检查 `system.info.diagnostics.materialBinding.active`。

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
- 独立 `VulkanLabRenderTest`、CPU PNG comparator、JSON 报告和 diff PNG；
- 受 GPU identity 约束、只能通过 `--accept` 更新的 golden baseline 流程。

截图是异步任务。`capture.screenshot` 返回 task ID 后，调用者必须轮询 `capture.status` 并确认 `state=Completed`，不能只根据目标文件是否暂时存在判断成功。完整命令和协议见 [Runtime Control](runtime_control.md)；一般测试应直接使用 RenderTest runner。

## 参数校验

`--help` 在创建 Window/Vulkan 前返回。非法尺寸、非法 fixed delta、缺失参数或未知参数同样在初始化前失败并返回非零退出码：

```powershell
.\VulkanLab.exe --help
.\VulkanLab.exe --window-size 800x600 --fixed-delta 0.016666667
```
