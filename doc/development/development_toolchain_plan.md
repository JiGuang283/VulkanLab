# VulkanLab 开发诊断与自动化工具链计划

> Status: Active
> Last verified: 2026-07-21
> Verified against: `d6f17cf`

> Progress: Stage 0-1 已完成。当前入口是 Stage 2：RenderDoc Labels And Validation Profiles；已完成切片保留为设计背景，当前操作以 guides/architecture 为准。

## Summary

VulkanLab 已具备 Runtime Control v3、响应式场景加载、KTX2 派生纹理、Catalog、Cook/package、加载统计、确定性截图和初始自动视觉回归。当前下一项短板是 GPU 对象缺少稳定名称和 pass 标记、Validation 配置不可复现；后续还包括 glTF 导入校验、统一 CPU/GPU timeline 和 Shader 接口契约。

本计划建立一套分层工具链，使后续材质、光照、阴影、后处理和资源管理开发具备以下闭环：

```text
源资产
  -> glTF 规范校验
  -> VulkanLab 导入/KTX2/Cook
  -> 自动加载固定场景与相机
  -> 确定性截图和图像回归
  -> Vulkan Validation/RenderDoc GPU 诊断
  -> Tracy CPU/GPU 性能分析
  -> CI 构建、测试、Shader 契约和 package 校验
```

实施顺序以自动化收益为优先。阶段 1 至 6 是近期计划；glTF 优化、显存 residency 和厂商 profiler 属于有量化门槛的后续阶段，不因为工具可用就提前改变运行时格式或架构。

前置工作由[工程结构与构建系统重构计划](engineering_refactor_plan.md)负责。该计划完成 target 化、build-tree Shader 和开发资源布局后，本计划 Stage 0 直接复用其 CMake Presets/build metadata，Stage 1 再接入截图服务；两份计划不重复实现同一构建基础。

Stage 0-1 的具体提交和验收见[工程基础到自动视觉回归执行记录](../archive/plans/engineering/engineering_to_visual_regression_execution_plan.md)。后续从本文 Stage 2 顺序继续。

当前实现依据：

- [构建与运行](../guides/build_and_run.md)
- [Runtime Control](../guides/runtime_control.md)
- [系统概览](../architecture/overview.md)
- [渲染流程](../architecture/rendering.md)
- [资源加载](../architecture/resource_loading.md)

## Goals

- 能从命令行完成“启动渲染器、加载场景、固定相机、等待稳定帧、截图、比较、退出”的完整流程。
- 小型确定性场景的渲染回归不再依赖人工肉眼检查。
- RenderDoc 中可以按业务名称识别 image、buffer、pipeline、pass、upload batch 和 draw 区域。
- Debug 运行可以选择 Core、Synchronization 和 GPU Assisted Validation 配置，并在日志中记录实际启用状态。
- 任意外部 glTF/GLB 在写入 Catalog 前获得机器可读的规范校验报告。
- 可以用统一 timeline 分析主线程、worker、KTX2 transcode、增量上传、每帧 CPU 和 Vulkan GPU 时间。
- 每个 SPIR-V 在构建时经过验证；Shader descriptor、push constant 和 vertex input 与 C++ 契约自动检查。
- Debug、Release、测试、诊断构建由 CMake Presets 统一，核心检查可在 Windows CI 重复执行。
- 所有诊断能力默认不进入 Cooked package，不改变普通 Release 的性能和部署依赖。

## Non-Goals

- 本计划不实现新的渲染效果、Deferred、Shadow、IBL 或 PostProcess。
- 不把截图像素完全一致作为跨 GPU、跨驱动的强制标准。
- 不在第一阶段建设通用编辑器测试框架或场景脚本语言。
- 不自动修复任意损坏的 glTF，也不覆盖用户的源文件。
- 不因为引入 glTF Transform 就立即要求运行时支持 Draco、meshopt、LOD 或新扩展。
- 不在本计划中迁移全部依赖到 vcpkg/Conan。
- 不在没有 memory budget 和实际 profile 数据时实现纹理 streaming、LRU 或多场景 residency。

## Tool Categories And Dependency Policy

工具按交付方式分成四类：

| 类别 | 工具 | 接入策略 |
|---|---|---|
| Vulkan SDK 工具 | `glslc`、`spirv-val`、`spirv-opt` | 由 CMake 从 Vulkan SDK 查找并记录版本，不复制进 Cook 包。 |
| 仓库依赖 | Tracy、SPIRV-Reflect | 固定版本的 Git submodule，默认关闭或只构建测试工具。 |
| 开发机程序 | RenderDoc、glTF Validator、Nsight Graphics、Radeon GPU Profiler | 通过显式路径或 PATH 发现，缺失时禁用对应能力，普通构建不失败。 |
| 可选资产工具 | glTF Transform、meshoptimizer | 只有后续优化阶段启用；使用项目内版本锁，不依赖全局隐式版本。 |

统一规则：

1. 所有工具版本写入日志、Runtime Control `system.info` 或测试报告。
2. 可选工具缺失必须提供清晰诊断，不允许静默改变导入结果。
3. Cook/package 只包含运行时必需文件，不包含 profiler、validator、golden image 或开发报告。
4. 自动生成内容放在 build、test output 或 derived cache，不提交机器相关路径。
5. 新 submodule 必须固定 commit，并更新递归初始化文档和许可证清单。

## Target Test Layers

完成后测试分为五层，失败责任边界明确：

| 层 | 解决的问题 | 是否需要 GPU |
|---|---|---:|
| CPU unit/contract | Catalog、manifest、协议、Shader reflection、图像比较算法 | 否 |
| Asset validation | glTF 是否符合规范、依赖是否完整 | 否 |
| Runtime smoke | 场景能否加载、是否有 Validation error、截图是否有效 | 是 |
| Visual regression | 固定场景是否发生可见变化 | 是 |
| Performance capture | CPU/GPU 时间、同步、显存和硬件瓶颈 | 是 |

CI 默认执行前两层和无需显示设备的 package 测试。GPU smoke、视觉基准和 profiler capture 先作为本地/专用机器任务，不能假设普通 GitHub Hosted Runner 提供稳定 Vulkan GPU。

## Stage 0: Baseline And Configuration Foundation

> CMake target 化、Presets 和 build metadata 的实现归属 [工程结构与构建系统重构计划](engineering_refactor_plan.md)。若前置计划已完成，本阶段只核对并消费这些能力。

### Scope

- 新增 `CMakePresets.json`，至少提供：
  - `windows-msvc-debug`
  - `windows-msvc-release`
  - `windows-msvc-test`
  - `windows-msvc-tracy`
- 新增统一诊断配置结构，例如 `DiagnosticsConfig`：
  - validation profile
  - RenderDoc capture availability
  - Tracy compiled/enabled
  - screenshot/output root
  - external validator path/version
- `system.info` 增加 `diagnostics` 对象，报告构建配置、Git revision、Vulkan SDK/Shader tools、BC7、validation profile 和可选工具状态。
- 新增 `.editorconfig` 和 `.clang-format`，只格式化项目源码，不机械改写 `external/` 和历史归档。
- 增加 `doc/guides/diagnostics.md` 骨架，后续各阶段补充实际使用方式。

### Implementation Notes

- Preset 只封装已有 CMake 入口，不改变当前生成器和 MSVC runtime 选择。
- Git revision 由 CMake `configure_file` 写入生成头文件；源码归档或无 Git 环境时使用 `unknown`。
- 所有诊断开关默认关闭，Debug 仍保持当前 Core Validation 默认行为。
- 不在本阶段引入 vcpkg/Conan，也不重新组织全部 CMake target。

### Acceptance

- 新机器可以使用 preset 完成 Debug、Release 和 CTest，不需要记忆 build 目录参数。
- `VulkanLab.exe --help` 与 `system.info` 能准确展示诊断选项和实际启用状态。
- `external/` 不进入格式化和静态检查范围。
- Debug/Release 现有 4 个 CTest 全部通过，Cook package 文件集合不变。

## Stage 1: Deterministic Screenshot And Visual Regression

这是本计划的最高优先级阶段。

### Runtime Interfaces

Runtime Control v3 增加：

```text
camera.get
camera.set
render.status
capture.screenshot
capture.status
capture.cancel
```

自动化实例不能继续只依赖固定的 `\\.\pipe\VulkanLab`。新增启动参数 `--runtime-control-pipe <suffix>`，实际 endpoint 为受约束的本地 pipe 名；`VulkanLabCtl` 和 RenderTest Runner 使用对应 `--pipe` 参数连接。suffix 只允许 ASCII 字母、数字、`-` 和 `_`，避免形成任意对象名或路径。省略参数时保持当前固定 endpoint，兼容人工调试命令。

- `camera.set` 接收 position、yaw、pitch，并在主线程修改 Camera。
- 确定性 viewport、固定 simulation delta 和 automation mode 通过启动参数设置；shader 复用现有 `shader.set`，单次截图是否包含 GUI 由 `capture.screenshot` 参数决定。
- `render.status` 返回 scene/load、pending upload、frame serial、present 和 capture queue 状态。
- `capture.screenshot` 接收 capture root 下的相对路径和是否包含 ImGui，立即返回 capture taskId；主线程继续渲染，客户端通过 `capture.status` 轮询，完成结果包含文件 SHA-256、尺寸和帧号。
- `capture.cancel` 只能取消尚未提交 readback 的任务；已经提交的 copy 仍等待对应 fence 后安全回收，但不再写 PNG。
- `render.wait` 是 VulkanLabCtl/RenderTest 的客户端命令，通过轮询 `load.status` 和 `render.status` 等待稳定帧；Runtime 主线程不创建跨帧 wait task。
- 启动参数 `--capture-root <path>` 决定允许写入的根目录；默认使用运行目录下 `artifacts/captures`。请求只接受规范化相对路径，不能通过 `..`、绝对路径或 Named Pipe 写入任意系统位置。
- 控制协议错误保持现有 `{id, ok:false, error}` 结构。

`VulkanLabCtl` 增加：

```powershell
VulkanLabCtl camera set --position 0,-35,10 --yaw 93 --pitch -2
VulkanLabCtl render wait --stable-frames 8
VulkanLabCtl capture screenshot sponza.png --no-gui
VulkanLabCtl capture status <task-id>
```

### Vulkan Capture Path

- Swapchain 创建时查询 `supportedUsageFlags`；支持时增加 `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`。
- 截图请求在目标帧 render pass 完成后、present 前记录：
  1. swapchain image 从当前布局转换为 `TRANSFER_SRC_OPTIMAL`；
  2. copy 到 host-visible readback buffer；
  3. 转回 present 所需布局；
  4. 使用该帧 fence 判断 readback 可读。
- 不支持 swapchain transfer source 的设备返回 `capture_unsupported`。只有实际需要覆盖此类设备时，才引入独立 offscreen color target，避免 Stage 1 扩大渲染架构改动。
- 处理 BGRA/RGBA channel 顺序、row pitch 和垂直方向；输出固定为 8-bit RGBA PNG。
- v1 只支持 8-bit RGBA/BGRA swapchain format；HDR 或其他 packed format 返回 `capture_format_unsupported`。
- `includeGui=false` 时该帧跳过 ImGui draw，而不是事后裁剪。
- resize、scene switch 或退出会取消未完成请求并返回确定错误，不能让 readback buffer 引用已销毁 swapchain。

### Render Test Runner

新增 `VulkanLabRenderTest.exe`，复用 Named Pipe 协议而不是直接链接 Application：

```powershell
VulkanLabRenderTest run `
  --spec tests/render/sponza_basecolor.json `
  --output build-test/render-results
```

测试 spec v1 包含：

- stable scene ID 和 profile ID
- shader display name/ID
- camera pose
- viewport
- warmup/stable frame count
- GUI visibility
- golden image 路径或 smoke-only
- 最大平均绝对误差、坏像素比例和单通道阈值

Runner 负责启动带 `--runtime-control` 的 VulkanLab、等待 ping、加载场景、应用设置、截图、比较并退出。异常时保存实际图、差异图、Runtime Control JSON 和日志。

Runner 为每次执行生成唯一 pipe suffix 和隔离输出目录，并通过启动参数固定窗口尺寸、关闭用户输入影响、启用固定 simulation delta。v1 仍创建普通可见 GLFW 窗口，不引入 headless surface；窗口必须能在后台保持有效 present。若操作系统最小化会停止 swapchain 更新，Runner 应报告 `window_not_rendering`，不能把超时误报为图像差异。

### Comparison Policy

- v1 使用可测试的 RGBA8 比较器，输出 MAE、RMSE、最大误差、超过阈值像素比例和 diff PNG。
- 所有 GPU 都执行 smoke invariant：尺寸正确、非全黑/全白、无 NaN 概念对应的异常输出、有效颜色覆盖率达到阈值。
- Golden comparison 先限定同一 reference GPU/driver family。跨厂商结果只作报告，不作为 blocking gate。
- Golden image 必须包含场景、shader、profile、viewport、GPU/driver 和生成 commit 元数据。
- 更新 golden 必须使用显式 `--accept`，并在提交中附带 diff 报告；普通测试不能自动覆盖基准。

### Initial Coverage

第一批使用小而稳定的场景，避免 CI/本地回归每次编码 Main Sponza：

- Viking Room + Legacy Forward
- Sheen Chair + PBR-lite NormalMapped
- 一个覆盖 alpha/transmission 的小 glTF
- 一个覆盖 AO UV1、doubleSided 和 vertex color 的专用测试资产

Main Sponza 仅作为本地 extended suite，不进入每次快速测试。

### Acceptance

- 一条命令可完成至少两个场景的加载、固定相机、无 GUI 截图、比较和退出。
- 连续两次相同配置截图在 reference GPU 上处于阈值内。
- 故意修改 baseColor 或 AO shader 时，对应 golden test 确定失败并生成可读 diff。
- capture 期间 resize、取消 scene load 和退出不产生 validation error 或悬挂请求。
- CookedOnly 下只允许包内安全输出目录截图，或明确禁用 capture；不能削弱 package 路径约束。

## Stage 2: RenderDoc Labels And Validation Profiles

### Debug Utils

新增集中式 `GpuDebugUtils`，封装：

- `vkSetDebugUtilsObjectNameEXT`
- `vkCmdBeginDebugUtilsLabelEXT`
- `vkCmdInsertDebugUtilsLabelEXT`
- `vkCmdEndDebugUtilsLabelEXT`

对象命名至少覆盖：

- swapchain image/view、depth image/view
- global/material descriptor set 和 pool
- vertex/index/staging/uniform/readback buffer
- texture image/view/sampler，名称包含 scene/material/slot
- graphics pipeline，名称包含 shader variant、queue、cull mode
- command pool/buffer、fence/semaphore

Command label 至少覆盖：

```text
Frame N
  MainForwardPass
    Opaque
    Transparent
  ImGui
  ScreenshotCopy
SceneUpload task=<id> batch=<n>
```

命名和 label helper 在 extension 不可用时为空操作，不把 Debug Utils 设为 Release 启动硬要求。

### Validation Profiles

新增启动参数：

```text
--validation off|core|sync|gpu
```

- `core`：当前标准 validation。
- `sync`：通过 `VkValidationFeaturesEXT` 启用 synchronization validation。
- `gpu`：启用 GPU Assisted Validation；遵循 SDK 建议，不与完整 Core profile 默认叠加。
- Cooked package 继续强制 `off`。
- 日志和 `system.info` 必须记录请求值、实际值和任何 feature fallback。

提供三个本地 smoke 脚本：对小场景分别运行 core、sync、gpu，加载、切 shader、resize、截图并退出。GPU-AV 性能很低是预期行为，不用于 Main Sponza 常规测试。

### RenderDoc Workflow

- 不把 RenderDoc 二进制加入仓库或 Cook 包。
- 文档说明通过 RenderDoc UI 启动 VulkanLab 和通过现有进程注入两种方式。
- 可选 Stage 2B：仅在 RenderDoc module 已加载时，动态获取 in-application API，增加 `capture.renderdoc` 命令；未加载返回 `renderdoc_unavailable`。
- 自动捕获不是 Stage 2A 的完成条件，业务对象名和 pass label 才是首要交付。

### Acceptance

- RenderDoc Event Browser 中能直接看到 Opaque、Transparent、ImGui 和 upload 区域。
- 纹理、pipeline 和 buffer 不再主要以匿名 handle 出现。
- 三种 validation profile 均能启动小场景，协议返回实际 profile。
- 人为制造 image layout 或 descriptor 越界测试时，sync/GPU profile 能产生预期错误；测试代码不得留在正常路径。

## Stage 3: glTF Validator Import Gate

### Tool Integration

- 固定并记录 Khronos glTF Validator 版本。
- 新增工具发现顺序：显式 `--gltf-validator`、与 `VulkanLabAssetTool.exe` 同目录、PATH。
- 统一由 `VulkanLabAssetTool` 托管外部进程：

```powershell
VulkanLabAssetTool validate scene `
  --source D:\Assets\Example\scene.glb `
  --report build\reports\example.validation.json
```

- UI 不直接管理 validator 进程。`SceneImportService::preflight` 通过资产工具任务获得结构化结果，延续现有 Job Object、取消和日志边界。
- Validator 缺失时，OnDemand 导入 UI 明确显示 `Validator unavailable`。初始 rollout 允许用户显式继续；稳定后再决定是否把 validator 设为新资产导入硬要求。

### Result Model

新增 `AssetValidationReport`：

- validator name/version
- source SHA-256、size、mtime
- error/warning/info/hint 数量
- issue code、message、JSON pointer、severity
- asset statistics
- report path 和生成时间

报告放在 derived cache 的 validation 子目录，以 source hash 和 validator version 为 key；不修改源 glTF，不把机器绝对路径写入 Catalog。

### Import Policy

- glTF 规范 Error 默认阻止 Catalog transaction。
- Warning/Info 允许导入，但 Import modal 显示摘要和可展开问题列表。
- 缺失本地依赖、路径逃逸和远程 URI 仍由现有 preflight 先阻止，不依赖 validator 代替安全检查。
- 对 unknown/unsupported extension 分开记录：规范允许但 VulkanLab 不支持时，作为 renderer capability warning，不冒充 glTF invalid。
- Reimport 时 source hash 或 validator version 变化会重新校验。

### UI And Runtime Control

- Import modal 增加 Validation 区域和 `Open Report`。
- Assets/Scenes 面板显示最近状态：Valid、Warnings、Invalid、Not Checked。
- Runtime Control 增加只读 `asset.validation` 查询；v1 不允许通过管道提交任意外部路径。

### Tests

- 最小合法 `.gltf` 返回 Valid。
- 缺失 accessor、越界 index、非法 enum、错误 MIME 分别产生稳定 issue。
- Warning 不阻止导入，Error 阻止 Catalog 写入。
- validator crash、超时、畸形 JSON 和取消不会留下半写 report/Catalog。
- 同一 source/version 命中报告 cache，修改 source 后失效。

### Acceptance

- 用户导入第三方模型前可以明确看到“模型问题”与“渲染器不支持的扩展”。
- Invalid 模型不会进入 Catalog；已有 Catalog 和派生纹理不受失败事务影响。
- Validator 不可用不会导致普通运行、已有场景加载或 Cook package 启动失败。

## Stage 4: Tracy CPU And Vulkan GPU Profiling

### Build Integration

- 以固定 commit 添加 `external/tracy` submodule。
- CMake option：

```text
VULKANLAB_ENABLE_TRACY=OFF
```

- 只有 `windows-msvc-tracy` preset 默认开启；普通 Debug/Release/Cook 关闭。
- 关闭时 profiling macro 编译为空操作，不保留 Tracy 网络线程或运行时依赖。
- Tracy profiler GUI 使用官方预编译开发工具，不进入仓库 package。

### CPU Instrumentation

首批 zone：

- `Application::run` frame
- Runtime command dispatch
- scene admission/import supervision
- `GltfPreparer` parse/material/mesh/KTX read/transcode
- `SceneGpuBuilder::pump`
- upload staging/submit/fence poll
- RenderQueue collect/sort
- MainForwardPass opaque/transparent
- pipeline cache miss/create
- swapchain acquire/present/resize

worker 设置稳定线程名；scene/task ID 作为 zone text，但避免每帧动态格式化大量字符串。

### Vulkan GPU Instrumentation

- 每个 frame-in-flight 建立正确生命周期的 Tracy Vulkan context/query 资源。
- GPU zone 对应 MainForwardPass、Opaque、Transparent、ImGui 和 upload batch。
- query result 只在对应 fence 完成后读取，不增加 `vkQueueWaitIdle()`。
- swapchain recreate 和 Device 销毁顺序覆盖 context/query 回收。
- GPU timestamp 不可用时只保留 CPU profile，并记录能力降级。

### Metrics And Capture Policy

- `FrameMark` 每 present 一次。
- 将 scene load progress、staging bytes、VMA allocation bytes、draw count 和 pipeline count 作为 plot。
- 标准 capture 分为：空闲场景 10 秒、连续 scene switch、Main Sponza load、相机移动 30 秒。
- profile 文件保存到测试输出目录，不默认提交；重要基准只在文档记录摘要和环境。

### Acceptance

- 能在同一 timeline 看到 worker prepare、主线程逐帧 upload 和 GPU rendering。
- Main Sponza load 中不再依靠日志推断线程空洞或长帧来源。
- Tracy 关闭的 Release 与当前基准相比无可测的额外线程和明显二进制增长。
- Tracy 开启后小场景 CPU frame overhead 目标低于 2%，超过时减少高频 zone。

## Stage 5: SPIR-V Validation And Shader Contract Tests

### Build Pipeline

CMake 同时查找：

```text
glslc
spirv-val
spirv-opt
```

每个 Shader 的构建流程：

```text
GLSL
  -> glslc temporary SPIR-V
  -> spirv-val --target-env <project Vulkan target>
  -> Debug: copy validated SPIR-V
  -> Release: spirv-opt conservative optimization
  -> spirv-val final SPIR-V
```

- 所有输出使用 `add_custom_command(OUTPUT ...)`，不再依赖手工同步 `compile.bat` 作为权威构建入口。
- Release 优化参数固定并写入 build log；不能删除 descriptor/interface 所需信息。
- Validator/optimizer 版本进入 `system.info` 和 package build metadata。

### Reflection Strategy

- 固定版本接入 SPIRV-Reflect。
- 第一版只做 contract test，不自动生成生产 DescriptorSetLayout，避免反射结果直接改变运行时 ABI。
- 新增 `ShaderContractTests`，对 `kShaderVariants` 全量检查：
  - SPIR-V 文件存在且 stage 正确
  - vertex input location/format 与 `Vertex` layout 一致
  - set 0 global binding 和 stage visibility
  - set 1 material texture binding
  - push constant size、offset 和 stage
  - fragment color output location
  - variant path和 display name 唯一
- Shader 可以不消费某个 vertex attribute，但不能声明与 C++ format 不兼容的同 location 输入。

### Generated Contract Manifest

构建生成 `shader_contract.json`，记录每个 SPIR-V 的 SHA-256、stage、entry point、descriptor、push constant 和 inputs。Cook 工具将 manifest 纳入 package，并校验 package 中 SPIR-V 与 manifest hash 一致。

### Acceptance

- 修改 binding、push constant 或 vertex location 而未同步 C++ 时，CTest 确定失败。
- 非法 SPIR-V 不会进入运行目录或 Cook package。
- Debug/Release Shader contract 一致，优化前后接口不变。
- 所有现有 shader variant 构建和运行画面保持不变。

## Stage 6: Windows CI And Quality Gates

### CI Jobs

新增 Windows CI，使用递归 submodule checkout，并固定 Vulkan SDK 安装版本：

1. `configure-and-build-debug`
2. `build-release`
3. `cpu-and-asset-tests`
4. `shader-contract`
5. `cook-package-smoke`

CI 不运行 Main Sponza，也不提交模型和 KTX2 cache。使用仓库内最小测试资产验证真实 KTX build、Catalog import、Cook 和 package verify。

### Static Checks

- `git diff --check`
- 当前 Markdown 相对链接检查
- `clang-format --dry-run`，只覆盖项目新增/修改 C++ 文件
- 可选 `clang-tidy` preset：先对 `src/assets`、`src/control`、新工具代码运行，不一次性把历史告警设为全仓库 blocking
- 检查 Cook package 不含 validator、Tracy、golden image、源 PNG/JPEG 或绝对路径

### Artifact And Failure Reporting

- 失败时上传 CTest XML、日志、validator report、render test JSON 和 diff image。
- 编译产物不长期发布；正式 package 发布仍使用显式 release 流程。
- CI cache 只缓存 Vulkan SDK/KTX build 中间结果，不缓存可影响正确性的共享 derived asset manifest。

### Acceptance

- 从干净 checkout 可以通过 preset 完成全部无 GPU 检查。
- Shader ABI、Catalog transaction、KTX2、Cook closure 和 package hash 进入 PR 门禁。
- CI 脚本没有开发机绝对路径，不依赖已安装的 RenderDoc、Tracy GUI 或厂商 profiler。

## Stage 7: Optional glTF Normalization And Geometry Optimization

该阶段只有在 Validator 稳定、截图回归可用后进入，避免无法判断优化是否改变画面。

### Evaluation First

- 使用 glTF Transform `inspect` 对代表模型生成统计，不修改源文件。
- 对 Main Sponza、CarConcept、ChronographWatch 测量：
  - primitive/draw count
  - vertex/index bytes
  - duplicate data
  - animation bytes
  - 可 prune 内容
- 评估 `prune`、`dedup`、`reorder`、`weld` 和 animation `resample`；每个 transform 单独生成派生模型并跑视觉回归。

### Derived Model Policy

- 输出放在 derived cache，例如 `prepared_scenes/<scene>/<profile>/scene.glb`。
- cache key 包含源 hash、工具版本、transform 列表和参数。
- 原始 glTF/GLB 永远不覆盖；Catalog 仍指向源资产，ArtifactIndex 指向派生结果。
- transform 失败回退源模型，但在 CookedOnly 中必须在 Cook 前确定并验证唯一产物。

### Meshopt Gate

只有满足以下条件才加入 meshoptimizer/`EXT_meshopt_compression`：

- representative scene 的 geometry IO、CPU conversion 或 GPU vertex bandwidth 被 Tracy/厂商 profiler 证明为瓶颈；或 package geometry 体积达到明确目标门槛。
- GltfPreparer 已实现 extension capability check、解码、取消、统计和损坏输入测试。
- source 与 optimized 两条路径通过相同 screenshot suite。

不要同时引入 Draco 和 meshopt。优先 meshopt 是因为它覆盖 vertex cache、fetch、压缩和后续 LOD 基础；最终选择仍由数据决定。

### Acceptance

- 每项优化都有独立 size/load/frame 对比和视觉 diff。
- 失败或不支持扩展不会产生半成品 Catalog/cache。
- 没有量化收益的 transform 不进入默认导入 profile。

## Stage 8: Memory Budget And Vendor Profiling Gate

### VK_EXT_memory_budget

- 在 Device 中可选启用 `VK_EXT_memory_budget`，结合 VMA 获取 heap budget/usage。
- Stats、Runtime Control 和 Tracy plot 显示 device-local budget、usage、场景预计增量和峰值。
- scene admission 在 GPU build 前检查预计资源量；v1 只警告或拒绝明显超预算任务，不实现 eviction。
- allocation/block bytes 继续与真实 heap budget 分开命名。

进入 residency/streaming 的条件沿用现有[资源加载决策](../architecture/resource_loading.md#platform-artifact-与-residency-决策)：代表场景 p95、单场景预算比例、多场景驻留需求或发布体积达到已记录门槛后再开启设计。

### Vendor Tools

- NVIDIA 机器使用 Nsight Graphics 分析 shader、pipeline、barrier 和硬件计数器。
- AMD 机器使用 Radeon GPU Profiler 分析 wave、cache、occupancy 和 queue timeline。
- 工程只提供 capture runbook、稳定测试场景和 marker；不链接厂商 SDK，不把单厂商工具设为构建依赖。
- 每次性能结论记录 GPU、driver、分辨率、scene/profile、shader、commit 和 capture 工具版本。

### Acceptance

- 能解释 VMA allocation、Vulkan heap usage 和操作系统显示显存之间的差异。
- 任何 residency、LOD、streaming 或 pipeline 优化都以 Tracy/厂商 capture 和视觉回归共同证明。

## Cross-Stage Test Matrix

| 场景 | 目的 | 快速测试 | 扩展测试 |
|---|---|---:|---:|
| Viking Room | OBJ baseline、Legacy | 是 | 是 |
| Sheen Chair | glTF PBR、normal | 是 | 是 |
| 小型材质 fixture | alpha、transmission、AO UV1、doubleSided | 是 | 是 |
| CarConcept | 大量材质、玻璃 | 否 | 是 |
| ChronographWatch | AO atlas、复杂材质 | 否 | 是 |
| Main Sponza | 大场景、KTX2、405 meshes | 否 | 是 |

每个近期阶段至少执行：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-test

cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release
```

具体 preset 名称在 Stage 0 实现时可以按 CMake 约束调整，但完成后文档和 CI 必须只使用最终稳定名称。

## Global Test Plan

### Automated

- Debug、Release 和 Tracy-enabled 配置至少各构建一次。
- CTest 覆盖 Runtime Control 新协议、pipe suffix 校验、截图读回、图像比较器、validator report parser、Shader reflection 和工具缺失降级。
- 快速 render suite 覆盖固定 viewport、固定 camera、无 GUI capture、diff 失败产物和进程清理。
- Core 与 Synchronization Validation 下完成 scene load、shader switch、capture、resize 和退出。
- Asset import 测试覆盖 validator success/warning/error/crash/cancel，以及 Catalog 原子性。
- Cook/package 测试确认不包含开发工具和测试产物，并继续通过完整 SHA-256 verify。
- `git diff --check`、格式检查、Markdown 链接检查和 SPIR-V validation 全部通过。

### Manual

- 在 reference GPU 上批准第一批 golden，并重复运行确认容差稳定。
- RenderDoc 抓取一个小场景和一次 Main Sponza 帧，检查命名、event label、descriptor、mip 和 pipeline state。
- Tracy 抓取 Main Sponza 导入/加载以及 30 秒稳定渲染，确认 CPU/GPU zone 对齐且没有 profiler 引入的同步等待。
- 分别在可用的 NVIDIA/AMD 机器运行厂商 profiler，记录工具版本和驱动。
- 对跨 GPU screenshot 只记录差异分布，达到足够样本前不设置统一 blocking threshold。

### Regression Invariants

- 禁用诊断选项时，普通 VulkanLab 命令行、ImGui、Runtime Control 默认 endpoint 和 CookedOnly 行为不变。
- Shader、descriptor layout、push constant 和现有材质视觉行为不因工具集成而改变。
- 诊断工具缺失不会阻止已有 Catalog 场景加载。
- 所有外部进程、readback buffer、query pool 和 profiler context 在取消、resize、异常和退出路径正确回收。

## Commit Strategy

每个阶段保持可独立验证和回退，建议提交边界：

1. `build: add diagnostic presets and tool discovery`
2. `feat: add deterministic screenshot capture`
3. `test: add runtime visual regression runner`
4. `feat: label Vulkan resources and command regions`
5. `feat: add Vulkan validation profiles`
6. `feat: validate glTF assets before import`
7. `feat: add optional Tracy profiling`
8. `build: validate and reflect SPIR-V contracts`
9. `ci: add Windows build and asset quality gates`
10. 每阶段对应的 guides/architecture 文档提交

不要把 external submodule 更新、全仓库格式化、功能实现和 golden image 更新混在同一提交。

## Manual Work Required

代码、构建、CPU/CLI 测试和自动截图流程可以由开发代理完成。以下操作需要目标机器或用户确认：

- 安装 RenderDoc，并确认 Vulkan implicit layer 能正常注入。
- 下载/批准固定版本的 glTF Validator 可执行文件；如许可证或分发方式不允许随仓库提供，则配置本地路径。
- 启动 Tracy Profiler GUI，保存并检查代表 capture。
- 在 NVIDIA/AMD 目标 GPU 上执行厂商 profiler capture。
- 首次批准 golden images，以及确认有意的视觉变化。
- 记录跨驱动截图容差和 Windows 任务管理器显存曲线。

所有手动步骤必须在对应 guide 中给出命令、预期结果和失败诊断；不能只写“用工具检查”。

## Assumptions

- 第一版继续以 Windows、MSVC 和 Vulkan SDK 为目标环境；跨平台工具发现和 CI 后续单独扩展。
- Vulkan SDK 提供匹配项目目标环境的 `glslc`、`spirv-val` 和 `spirv-opt`。
- 自动视觉测试允许创建可见 GLFW 窗口；真正 headless/offscreen renderer 不属于近期阶段。
- Reference GPU golden 由人工审核，仓库不会自动接受新基准。
- glTF Validator 的固定版本和分发方式需要在 Stage 3 开始前确认；计划不假设可以无条件把第三方 exe 提交到仓库。
- 原始 glTF/GLB 始终是 source of truth；validator、normalization、KTX2 和 optimized geometry 都属于可重建派生产物。
- Tracy、RenderDoc 和厂商 profiler 只用于开发构建，不进入正式 Cook package。

## Rollout And Stop Conditions

- Stage 1 完成前，不把更多 shader variant 或复杂材质扩展作为主要开发方向，因为缺少自动视觉回归。
- Stage 2 和 Stage 3 可在 Stage 1 的 screenshot 基础稳定后并行实施。
- Stage 4 Tracy 的结论用于决定性能优化，不用 profiler 的单次观感替代数据。
- Stage 5 和 Stage 6 完成后，新的 Shader/资产功能必须进入自动门禁。
- Stage 7/8 如果没有达到量化 gate，记录 Deferred 决策并停止，不为了“工具齐全”继续增加运行时复杂度。

## Risks And Mitigations

| 风险 | 缓解措施 |
|---|---|
| 跨 GPU 浮点和采样差异导致 golden 不稳定 | 分离 smoke 与 reference-GPU golden；使用容差和差异比例，不要求全平台逐像素一致。 |
| 截图 readback 改变 swapchain 同步 | 使用现有 frame fence 管理生命周期；Stage 1 必须通过 sync validation。 |
| GPU-AV/Tracy 明显降低性能 | 使用显式 profile/preset，默认关闭，不用于普通 Release。 |
| Validator 外部进程不可用 | 明确工具发现和状态；初期允许显式继续，不影响已有资产。 |
| glTF 优化改变材质或扩展语义 | 只生成派生副本，每项 transform 单独跑 validator 和视觉回归。 |
| SPIR-V reflection 与运行时 ABI 分叉 | 第一版只作为 contract test，不自动生成生产 layout。 |
| CI 没有稳定 Vulkan GPU | GPU 测试留在专用 runner；Hosted CI 只执行 CPU、Shader、资产和 package 门禁。 |
| 工具依赖膨胀 Cook 包 | Cook closure test 明确禁止 profiler、validator、golden 和开发报告。 |

## Completion Criteria

本计划只有在以下条件全部满足后才可归档：

- 至少四类代表材质行为由自动 screenshot suite 覆盖。
- Runtime Control 能确定性设置相机、等待稳定帧并生成截图。
- RenderDoc 能显示稳定对象名和 pass label。
- glTF import preflight 生成规范报告并正确执行 Error/Warning policy。
- Tracy 可以同时展示 scene load CPU 工作和 Vulkan GPU pass。
- 所有 shader variant 通过 `spirv-val` 和 reflection contract tests。
- Windows CI 从干净 checkout 通过 Debug/Release、CTest、KTX2、Cook 和 package verify。
- 当前 guides/architecture 已更新，所有可选工具安装方式、版本和故障处理有明确说明。
- Stage 7/8 已根据量化数据实施或正式记录 Deferred 决策。

完成后使用 `git mv` 将本文移入 `doc/archive/plans/tooling/`，并将实际行为写入 Current guides/architecture；不能把本文继续作为当前功能说明。

## References

- [RenderDoc](https://github.com/baldurk/renderdoc)
- [Khronos GPU Assisted Validation](https://vulkan.lunarg.com/doc/view/latest/windows/gpu_validation.html)
- [Tracy Profiler](https://github.com/wolfpld/tracy)
- [Khronos glTF Validator](https://github.com/KhronosGroup/glTF-Validator)
- [glTF Transform](https://gltf-transform.dev/)
- [Khronos SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools)
- [Khronos glTF 2.0](https://github.com/KhronosGroup/glTF)
