# 工程基础到自动视觉回归执行计划

> Status: Active
> Last verified: 2026-07-19
> Verified against: `3bca8f3`

## Summary

本文定义从当前代码状态到完成“开发诊断与自动化工具链 Stage 1”的连续执行路线。终点不是只有截图 API，而是一条可以重复运行的验证闭环：

```text
clean checkout
  -> preset configure/build
  -> target-based runtime/tools/tests
  -> 增量生成 build-tree SPIR-V
  -> 从 ProjectContext 读取开发资产
  -> 启动唯一 Runtime Control 实例
  -> 加载 stable scene ID
  -> 设置固定相机和确定性时间
  -> 等待场景/上传/帧稳定
  -> GPU copy swapchain image
  -> 异步 PNG 编码
  -> smoke/golden 比较
  -> 输出 JSON、actual、diff 和日志
  -> 正常退出且无残留进程
```

本执行计划组合并细化两份长期计划：

- [工程结构与构建系统重构计划](engineering_refactor_plan.md)
- [开发诊断与自动化工具链计划](development_toolchain_plan.md)

它只覆盖工程重构中支撑自动化所必需的部分，以及工具链 Stage 1 的完整实现。Editor Panels 全量拆分、SceneWorkflowController、完整测试框架迁移、RenderDoc、GPU-AV profile、glTF Validator、Tracy、Shader reflection、CI 和 geometry optimization 不在本轮范围内。

## Authority And Plan Relationship

- 两份长期计划继续描述最终方向和后续阶段。
- 本文是近期执行顺序和接口决策的权威来源；如与长期计划中的阶段排序或截图协议细节不同，以本文为准，并同步修正长期计划。
- 本文完成后归档到 `doc/archive/plans/engineering/`，长期计划继续保留未完成阶段。
- 代码实现开始前，当前所有计划文档应作为 docs-only commit 提交，避免文档和大规模 CMake 变更混在一起。

## Current Baseline

当前代码具备：

- Runtime Control v2，固定 `\\.\pipe\VulkanLab`，主线程每帧处理至多一个命令。
- scene/load/import taskId、后台 glTF prepare、逐帧 GPU upload 和取消。
- KTX2/BC7 派生纹理、Catalog、ArtifactIndex、Cook/package 和 package verify。
- Debug/Release 构建与 4 个聚合 CTest。
- FrameSync 使用两个 frame-in-flight fence，Renderer 在同一个 command buffer 中完成 MainForwardPass 和 ImGui。
- MainForwardPass 使用 MSAA color attachment，并 resolve 到 swapchain image；render pass 结束后 swapchain image 为 `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`。

当前阻碍：

- 根 CMake 使用全局 include/link path、`GLOB_RECURSE` 和重复生产源码列表。
- 15 个 tracked SPIR-V 由全量 `PRE_BUILD` 写回源码树。
- POST_BUILD 扫描和复制完整 `models/`。
- `Application.cpp` 包含完整 Runtime method switch 和所有主要 UI/工作流编排。
- Runtime Control 没有相机、帧稳定、截图和唯一 endpoint 控制。
- 没有 screenshot readback、image comparator 或 RenderTest runner。

当前工作树已有未提交的计划文档，本轮代码前必须先形成干净基线。

## End State

完成本文后应具备：

1. 项目使用 target-scoped CMake 和内部复用库。
2. Shader 在 build tree 按文件增量生成，源码树无 SPIR-V。
3. 开发运行直接通过 ProjectContext 读取 Catalog/source asset，不复制完整 `models/`。
4. Runtime command 分派不再直接占据 Application 大型 switch。
5. Debug/Release/Test preset 和 BuildInfo 可重复使用。
6. 每个自动化实例使用唯一 Named Pipe 和隔离 capture root。
7. 可以固定相机、窗口、simulation delta、GUI visibility 和 shader。
8. CaptureService 安全地把 swapchain image 读回并在 worker 编码 PNG。
9. VulkanLabRenderTest 能自动启动、加载、等待、截图、比较、保存报告和清理进程。
10. Viking Room 与 Sheen Chair 至少具备跨 GPU smoke tests；reference GPU 至少具备一项经过人工批准的 golden test。

## Non-Goals

- 不实现 headless Vulkan surface 或 offscreen renderer。
- 不支持 HDR/10-bit/float swapchain screenshot。
- 不在本轮引入 RenderDoc API、Synchronization Validation 启动 profile 或 Tracy。
- 不实现 SSIM、感知模型或跨厂商统一 golden threshold。
- 不测试 Main Sponza 每次快速回归；它只进入 extended/manual suite。
- 不重构 RenderPipeline、MainForwardPass draw logic、材质、descriptor 或 Shader 内容。
- 不完成工程重构长期计划中的 Editor Panels 全量拆分和 SceneWorkflowController。
- 不迁移到 vcpkg/Conan，不替换 GLFW/KTX 来源。
- 不在截图请求中允许任意绝对输出路径。

## Delivery Milestones

| Milestone | 对应长期阶段 | 主要交付 | 阻塞后续 |
|---|---|---|---:|
| M0 | Engineering Stage 0 | 干净基线和可比较数据 | 是 |
| M1 | Engineering Stage 1 | Target-based CMake | 是 |
| M2 | Engineering Stage 2 | Build-tree incremental Shader | 是 |
| M3 | Engineering Stage 3 | ProjectContext 开发资产路径 | 是 |
| M4 | Engineering Stage 4A + Toolchain Stage 0 | Runtime dispatcher、Presets、BuildInfo、诊断配置 | 是 |
| M5 | Toolchain Stage 1A | Vulkan screenshot/readback core | 是 |
| M6 | Toolchain Stage 1B | Runtime automation protocol 和唯一 pipe | 是 |
| M7 | Toolchain Stage 1C | RenderTest runner、比较器和初始 suite | 终点 |

任何 milestone 未达到 exit criteria 时不得继续。M1 至 M4 只允许结构和基础设施变化，不能通过修改 Shader、材质或 golden threshold 掩盖回归。

## Global Invariants

- Vulkan、GLFW、ImGui、Scene 和 GPU resource ownership 继续在主线程。
- worker 只处理 CPU 数据、文件 IO、PNG 编码和进程监督。
- screenshot 不调用 `vkQueueWaitIdle()` 或 `vkDeviceWaitIdle()`。
- capture completion 由已有 frame fence 的 submission serial 推进，不创建每截图 queue-idle 路径。
- scene/import/capture task 都使用单调 ID；状态终结后不可回退。
- resize、scene switch、取消和退出不会释放仍被 GPU 使用的 readback buffer。
- Runtime Control 查询必须快速返回；客户端等待通过轮询完成，不让主线程阻塞未来帧。
- 开发自动化能力不进入 Cook package 默认能力集合。
- 构建和运行不得修改源码树或 tracked asset。

## M0: Baseline And Documentation Commit

### Scope

- 将以下文档作为一个 docs-only commit 提交：
  - `doc/development/development_toolchain_plan.md`
  - `doc/development/engineering_refactor_plan.md`
  - 本文
  - `doc/development/README.md`
- 确认没有模型、cache、build、log 或 SPIR-V 变化混入提交。
- 创建本轮 Implementation Record 表，记录每个 milestone commit、构建和测试结果；可以直接追加在本文末尾，实施前保持空表。

### Baseline Measurements

记录 Debug 和 Release：

- clean configure/build command、wall time 和输出 target。
- no-op rebuild time。
- CTest 4/4 结果。
- 15 个 tracked SPIR-V SHA-256。
- runtime output 下 `shader/`、`textures/`、`models/` 文件数和字节数。
- POST_BUILD 资源 staging 时间。
- Release Cook/package verify 的 protected file count 和 bytes。
- Viking Room、Sheen Chair、Main Sponza 1024 的 Runtime Control load result。
- Main Sponza texture/mesh/material/object、cache hit 和 VMA delta。

基线原始日志放到 ignored `artifacts/engineering-baseline/`；文档只记录摘要，不提交本机绝对路径。

### Exit Criteria

- 工作树在 docs commit 后干净。
- Debug、Release 构建通过。
- Debug/Release CTest 均为 4/4。
- package verify 通过。
- 无残留 VulkanLab、AssetTool 或 ktx 进程。

## M1: Target-Based CMake Foundation

### M1.1 Data Boundary

- 新增 `src/scene/SceneTypes.h`，提取 `CameraPose` 和 `Bounds`。
- `Scene.h`、`SceneCatalog.h`、`PreparedSceneData.h`、GltfPreparer 和 Cook 使用 data-only header。
- `SceneTypes.h` 只依赖标准库/GLM，不包含 Scene、Material、Device 或 Vulkan handle。
- 增加 CPU compile/test，确认 Catalog camera round trip 和 Bounds 默认值不变。

### M1.2 Target-Scoped Dependencies

建立：

```text
vkl_build_options                 INTERFACE
vkl_glm/json/spdlog/...           INTERFACE
vkl_glfw                          IMPORTED/INTERFACE
vkl_image_codecs                  STATIC
vkl_gltf_parser                   STATIC
vkl_obj_parser                    STATIC
vkl_vma_impl                      STATIC
vkl_imgui                         STATIC
```

- 移除全局 `include_directories()` 和 `link_directories()`。
- 当前 GLFW `lib-vc2022/glfw3.lib` 只通过 imported target 暴露，不在本阶段换库。
- stb、tinygltf、tinyobj 和 VMA implementation translation unit 各有唯一 target owner。
- KTX 4.4.2 feature 和 ASTC runtime workaround 迁移到 `cmake/Dependencies.cmake`，行为不变。

### M1.3 Project Libraries

建立：

```text
vkl_foundation
vkl_shader_catalog
vkl_asset_core
vkl_asset_runtime
vkl_control
vkl_engine
vkl_asset_tool_core
```

- `vkl_foundation` 只包含 Log/BuildInfo 等轻量能力。
- `vkl_shader_catalog` 共享 Shader variant ID、display name 和相对路径。
- `vkl_asset_core` 包含 Catalog、manifest、index、package、project context 和 import transaction。
- `vkl_asset_runtime` 包含 import/load coordinator、runtime derived cache。
- `vkl_control` 包含 protocol、queue 和 NamedPipe server。
- `vkl_engine` 第一版包含 core/render/scene/window/platform，避免过度拆分。
- `vkl_asset_tool_core` 包含可测试的 cache/cook/process 实现。
- VulkanLab、AssetTool、Ctl 和 tests 只链接库，不重新列生产 `.cpp`。

### M1.4 Directory CMake

目标布局：

```text
CMakeLists.txt
cmake/Dependencies.cmake
cmake/ProjectOptions.cmake
src/CMakeLists.txt
shader/CMakeLists.txt
tools/CMakeLists.txt
tests/CMakeLists.txt
```

- 根 CMake 只负责 project、options、dependency entry 和 `add_subdirectory()`。
- 移除 `GLOB_RECURSE src`。
- source list 在所属目录显式维护。
- `VulkanLabCpuTests` 暂时可以保持一个 executable，但必须链接生产库。

### Required Verification

- clean Debug/Release build。
- 4/4 CTest。
- 使用 build log 确认共享资产 `.cpp` 每 config 只编译到所属静态库一次。
- VulkanLab、AssetTool、Ctl 启动/帮助正常。
- Runtime ping、scene list/load、stats、quit 正常。
- package verify 和 protected file 集合不变。

### Exit Criteria

- 项目 CMake 不存在全局 include/link directory 或 `GLOB_RECURSE src`。
- test/tool CMake 不重复列出生产实现。
- 第三方 implementation macro 所有权唯一。
- Shader hash、场景资源数量和运行行为无变化。

### Suggested Commits

1. `refactor: extract shared scene data types`
2. `build: add target-scoped dependency wrappers`
3. `build: create reusable VulkanLab libraries`
4. `build: split project CMake by target`

## M2: Build-Tree Incremental Shader Pipeline

### Build Layout

```text
shader/*.vert|*.frag
  -> <build>/generated/<config>/shader/**/*.spv
  -> VulkanLabShaders
  -> <runtime>/shader/**/*.spv
  -> Cook package shader/**/*.spv
```

### Implementation

- `shader/CMakeLists.txt` 显式登记 15 个 source。
- 每个 Shader 使用独立 `add_custom_command(OUTPUT ...)`。
- multi-config output 包含 config，避免 Debug/Release 覆盖。
- VulkanLab/runtime staging/Cook 的 build dependency 指向 `VulkanLabShaders`。
- 删除 `PRE_BUILD` 全量 glslc。
- runtime 相对路径保持 `ShaderVariant` 当前值，不改变协议或 UI 名称。
- `compile.bat` 删除，或短期改成调用 CMake target；不能继续维护第二份命令列表。
- 新链路验证后，从 Git 删除 15 个 `shader/**/*.spv`，并在 `.gitignore` 禁止回写。

### Required Verification

- clean build 生成 15 个 SPIR-V。
- no-op build 不调用 glslc。
- 修改一个 `.frag` 只重建对应输出。
- Debug/Release runtime 各自取得正确 config 输出。
- 移除源码树 SPIR-V 后 Viking Room、Sheen Chair 和 shader switching 正常。
- Cook package 仍只包含 15 个实际 SPIR-V，verify 通过。
- 构建后 `git status` 不出现生成文件。

### Exit Criteria

- 源码树不含 tracked/generated SPIR-V。
- C++ rebuild 不触发全量 Shader 编译。
- Runtime 与 Cook 没有两套 Shader 来源。

### Suggested Commits

1. `build: generate shaders incrementally in the build tree`
2. `chore: remove tracked generated SPIR-V`
3. `docs: update shader build workflow`

## M3: ProjectContext Developer Asset Layout

### Runtime Path Contract

定义并集中解析：

```text
projectRoot   Catalog、glTF/GLB、external buffers/images、builtin source asset
runtimeRoot   executable、generated shader、runtime tools、locator
cacheRoot     derived manifests/blobs/index
captureRoot   后续自动化输出，独立受限
```

- 开发模式 Catalog source 从 projectRoot 解析。
- Viking Room texture/OBJ 从 projectRoot 解析。
- SPIR-V 从 runtimeRoot 解析。
- Cooked package 中 projectRoot/runtimeRoot 都是 package root，cacheRoot 为 package runtime_assets。
- 不允许 subsystem 继续隐式依赖 current working directory。

### Migration

1. 增加 `RuntimePaths` 或等价 ProjectContext 字段，并先保留当前复制。
2. 从仓库根、runtime output 和任意 CWD 启动并验证相同解析结果。
3. 加强 builtin scene Cook closure 测试。
4. 删除 POST_BUILD 对完整 `models/` 的复制。
5. 删除完整 `textures/` 复制；若确有 runtime staging 需要，只复制明确的最小集合。
6. 保留 locator、generated Shader、VulkanLabAssetTool/ktx 等必要 runtime 文件。

### Required Verification

- runtime output 删除 `models/` 后 Viking Room、Sheen Chair 和 Main Sponza 仍可加载。
- 显式 `--project`、developer locator、ancestor Catalog 三种发现方式正常。
- Cook package 移出仓库并删除 source/shared cache 后独立运行。
- no-op build 不扫描 Main Sponza，POST_BUILD 时间明显下降。
- Catalog/import 的 Copy/Reference 行为不变。

### Exit Criteria

- 默认开发构建不复制完整 `models/`。
- CWD 不影响资产解析。
- package closure 没有意外引用源码项目。

### Suggested Commits

1. `refactor: resolve runtime assets through project context`
2. `build: stop copying the full model directory`
3. `test: cover developer and packaged asset roots`

## M4: Application Boundary And Diagnostic Foundation

M4 是工具链 Stage 1 的直接前置，只实现所需边界，不完成全部 Application 长期拆分。

### M4.1 RuntimeCommandDispatcher

- 将 Runtime method switch、参数校验和 result/error JSON 组装移到 `control/RuntimeCommandDispatcher`。
- 定义主线程 `RuntimeControlHost` action interface。
- Application 实现 scene、shader、texture、asset、stats、quit actions。
- Dispatcher 不持有 Window、Device、Scene 或 worker；所有 action 在主线程调用。
- 现有协议 JSON 和错误 code 保持兼容。
- 新 camera/capture method 后续只扩展 dispatcher/host，不再扩展 Application 大型 switch。

### M4.2 Presets And BuildInfo

新增：

```text
windows-msvc-debug
windows-msvc-release
windows-msvc-test
```

- preset 封装 multi-config 的 configure/build/test 参数。
- 生成 BuildInfo：Git revision/dirty、config、compiler、Vulkan SDK/glslc version。
- 无 Git 环境时明确为 `unknown`。
- `system.info` 增加 build/diagnostics，但保留既有字段。

### M4.3 DiagnosticsConfig

新增轻量配置：

- automation mode
- fixed simulation delta
- fixed/non-resizable window size
- GUI visibility
- runtime pipe suffix
- capture root

普通启动默认值保持当前行为。Cooked package 默认禁用 automation/capture；是否允许显式开发 capture 由 M6 capability policy 决定。

### M4.4 Guide Skeleton

新增 `doc/guides/diagnostics.md`，先记录：

- presets/build info
- runtime automation 启动参数
- 输出根目录和安全限制
- 尚未实现的 capture 命令明确标记为 pending，M7 完成后再改为 Current

Current guide 不得提前宣称截图已经存在。

### Required Verification

- Runtime Control v2 全部命令 response snapshot 测试通过。
- UI scene/shader/profile 行为不变。
- `--help`、默认启动、`--runtime-control`、CookedOnly 启动正常。
- preset clean build 和 CTest 通过。
- `system.info` 返回准确 revision/config。

### Exit Criteria

- Application 不再包含完整 Runtime method switch。
- 自动化配置有单一 Config/BuildInfo 来源。
- 后续 CaptureService 可以独立接入。

### Suggested Commits

1. `refactor: extract runtime command dispatcher`
2. `build: add development presets and build metadata`
3. `feat: add diagnostic runtime configuration`

## M5: Toolchain Stage 1A, Vulkan Capture Core

### Capture Data Model

新增 `src/diagnostics/CaptureService.*` 和 data-only types：

```text
CaptureTaskState:
  Queued
  Recording
  WaitingForGpu
  Encoding
  Completed
  Failed
  Cancelling
  Cancelled

CaptureRequest:
  taskId
  relativeOutputPath
  includeGui

CaptureResult:
  width/height
  format
  frameSerial
  outputPath
  sha256
  timings
  error
```

- taskId 单调递增，不复用 scene/import 的高位 ID namespace。
- v1 同时只允许一个 Recording/WaitingForGpu capture；队列上限固定，例如 8。
- task history 有固定上限，避免自动化长跑无限增长。

### Swapchain Capability

- `SwapChain::createSwapChain()` 检查 `supportedUsageFlags`。
- 支持时在 image usage 增加 `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`。
- 记录 `captureSupported` 和实际 format。
- v1 只接受 `R8G8B8A8`/`B8G8R8A8` UNORM/SRGB。
- usage 或 format 不支持时，capture task 返回确定错误；本轮不引入 offscreen fallback。

### Command Recording

截图记录点位于：

```text
Renderer::renderFrame()
  pipeline.execute()              # MainForwardPass，结束后 swapchain=PRESENT
  captureService.recordCopy()     # 若本帧有请求

FrameSync::endFrame()
  vkEndCommandBuffer
  vkQueueSubmit
  vkQueuePresentKHR
```

`recordCopy()`：

1. barrier `PRESENT_SRC_KHR -> TRANSFER_SRC_OPTIMAL`；
2. `vkCmdCopyImageToBuffer` 到 host-visible transfer-dst buffer；
3. barrier `TRANSFER_SRC_OPTIMAL -> PRESENT_SRC_KHR`；
4. 记录 task 与本次 submission serial 的关联。

readback size 使用 checked arithmetic：`width * height * 4`，拒绝 overflow/超限。`bufferRowLength=0`、`bufferImageHeight=0` 产生 tightly packed copy。

### Frame Completion Serial

不能让 CaptureService直接等待或在 fence reset 后猜测完成状态。扩展 FrameSync：

- 每次 graphics submit 分配单调 `submissionSerial`。
- 每个 frame slot 记录 lastSubmittedSerial。
- `beginFrame()` 等待 slot fence 成功后，在 reset 前推进 `completedSubmissionSerial`。
- `endFrame()` 返回/公开本次 serial，CaptureService 将 recorded task 标记 WaitingForGpu。
- CaptureService 每帧查询 `completedSubmissionSerial`；达到 task serial 后才 map/read。
- 同一 graphics queue 保证 submit 顺序，因此 completed serial 可单调推进。

该机制后续可复用于 screenshot、timestamp query 和 deferred destruction，但本轮不改造其他资源生命周期。

### CPU Readback And Encoding

- readback buffer 使用 host-visible memory；非 coherent 时显式 invalidate。
- GPU 完成后主线程只复制到 CPU-owned RGBA8 vector，并立即释放/复用 Vulkan readback slot。
- BGRA 转 RGBA，保留 alpha；验证图像上下方向，只有确认 Vulkan 输出需要时才翻转。
- PNG 编码和 SHA-256 在单独 worker 中执行，不阻塞主循环。
- worker 不访问 Vulkan、Window、ImGui、Scene 或 task container；通过线程安全结果队列返回。
- shutdown 先停止接收，等待已提交 GPU work，再取消/完成 encode，并 join worker。

### GUI Exclusion

当前 ImGui 在 MainForwardPass 内绘制。`includeGui=false` 时目标 capture frame 的 `RenderFrameContext.gui=nullptr`，只跳过该帧 GUI draw；Application 可以继续构建 ImGui frame，不能破坏下一帧 backend 状态。

### Resize And Cancellation

- Queued task 在 resize 后可继续等待新 swapchain。
- Recording/WaitingForGpu task 在旧 swapchain 提交后必须等 fence，再标记 Failed/Cancelled 并安全释放。
- resize 不得销毁 task 使用的 readback buffer。
- `cancel` 对 Queued 立即生效；Recording/WaitingForGpu 只设置 cancellation，GPU 完成后不编码。
- Device lost 或 shutdown 将所有非 terminal task 完成为明确错误。

### Required Tests

- Capture task state transition 和 queue/history bound CPU tests。
- path/byte-size/format validation CPU tests。
- BGRA->RGBA 和 image orientation fixture tests。
- submission serial 单调和 frame-slot reuse tests。
- Debug GPU smoke：800x600 Viking Room 截图尺寸正确且非全黑。
- resize、取消、连续两次 capture、scene switch、退出无 core validation error。
- 使用 Vulkan Configurator 手动启用 synchronization validation 跑一次 screenshot；本轮不实现 `--validation sync` 参数。

### Exit Criteria

- 本地 API 能异步生成正确 PNG。
- capture 期间窗口继续响应并持续 present。
- 没有 queue/device idle 截图路径。
- readback/worker 在 resize、取消和退出无泄漏或悬挂。

### Suggested Commits

1. `feat: track frame submission completion serials`
2. `feat: add asynchronous swapchain capture service`
3. `test: cover capture state and pixel conversion`

## M6: Toolchain Stage 1B, Runtime Automation Protocol

### Unique Pipe Endpoint

新增：

```text
VulkanLab.exe --runtime-control-pipe <suffix>
VulkanLabCtl.exe --pipe <suffix> ...
```

- 默认 endpoint 仍为 `\\.\pipe\VulkanLab`，兼容现有使用。
- 指定 suffix 后使用 `\\.\pipe\VulkanLab.<suffix>`。
- suffix 只允许 ASCII 字母、数字、`-`、`_`，长度受限。
- 保持 `PIPE_REJECT_REMOTE_CLIENTS`、64 KiB message limit 和一连接一请求。
- Runner 每次生成随机/进程唯一 suffix，不能抢占人工实例。

### Capture Root

新增：

```text
--capture-root <directory>
```

- 默认开发路径为 runtime working root 下 `artifacts/captures`。
- Runtime 请求只接受相对路径。
- 拒绝绝对路径、`..`、空文件名、非法扩展和解析后逃逸 root。
- 使用临时文件写 PNG，成功后原子替换；失败不留下看似成功的目标。
- CookedOnly 默认 capability 为 capture disabled；后续若需要开放，必须单独定义 package-local writable root。

### Protocol v3

新增快速方法：

```text
camera.get
camera.set
render.status
capture.screenshot
capture.status
capture.cancel
```

语义：

- `camera.set` 接收有限浮点 position/yaw/pitch，主线程更新 Camera。
- `render.status` 返回 scene/load state、frameSerial、completedSubmissionSerial、presented frame count、pending upload、capture queue 和 GUI visibility。
- `capture.screenshot` 校验请求后立即返回 taskId，不等待未来帧或 PNG。
- `capture.status` 返回状态/result/error。
- `capture.cancel` 按 M5 取消语义执行。

`render.wait` 定义为 VulkanLabCtl/RenderTest 的**客户端命令**，不是一个在主线程长期等待的协议调用：

```powershell
VulkanLabCtl render wait --stable-frames 8 --timeout-ms 30000
```

客户端轮询 `load.status` 和 `render.status`，直到：

- scene/import task terminal success；
- pending upload 为 0；
- scene generation 未变化；
- 连续 N 个 presented frame；
- 没有 swapchain recreate/minimized stall。

这样不需要 server-side wait task，也不会让管道线程或主线程持有跨帧 promise。

### Deterministic Runtime Options

Runner 启动 VulkanLab 时使用：

```text
--automation
--window-size <width>x<height>
--fixed-delta <seconds>
--runtime-control
--runtime-control-pipe <suffix>
--capture-root <isolated-directory>
```

- automation 禁用相机用户输入对测试状态的影响。
- fixed delta 同时推进 `dt` 和 simulation time，不读取 wall-clock 作为 Scene update 输入。
- v1 使用可见、固定大小窗口；不自动最小化。
- GUI 默认可见但 capture 可以逐请求排除；Runner 默认 `includeGui=false`。
- viewport 由 swapchain/window 决定，不在 Runtime Control 中创建第二套尺寸状态。

### VulkanLabCtl Commands

```powershell
VulkanLabCtl --pipe <suffix> camera get
VulkanLabCtl --pipe <suffix> camera set --position 2,2,2 --yaw -135 --pitch -30
VulkanLabCtl --pipe <suffix> render status
VulkanLabCtl --pipe <suffix> render wait --stable-frames 8
VulkanLabCtl --pipe <suffix> capture screenshot viking.png --no-gui
VulkanLabCtl --pipe <suffix> capture status <task-id>
VulkanLabCtl --pipe <suffix> capture cancel <task-id>
```

默认文本输出可读，`--json` 返回原协议 response；退出码保持 0/1/2 约定。

### Required Tests

- pipe suffix、capture path 和有限浮点参数 parser tests。
- 默认 pipe backward compatibility。
- 两个 VulkanLab 自动化实例使用不同 suffix，不互相响应。
- malformed/oversized request、未知 capture task、非法路径和 disabled capability error。
- scene load + camera set + wait + capture + quit 完整 smoke。
- quit 必须在客户端收到响应后关闭，且 join pipe/capture worker。

### Exit Criteria

- 终端可控制任意自动化实例且不影响人工实例。
- 所有等待由客户端轮询，主线程每个 command 都快速返回。
- capture 路径无法逃逸 root。
- UI 和 Runtime Control 显示的 scene/shader/camera 状态一致。

### Suggested Commits

1. `feat: support isolated runtime control endpoints`
2. `feat: add camera and render automation commands`
3. `feat: expose asynchronous screenshot commands`
4. `docs: document runtime automation protocol`

## M7: Toolchain Stage 1C, RenderTest Runner And Visual Regression

### Executable

新增独立 target：

```text
tools/vulkan_lab_render_test/
  CMakeLists.txt
  main.cpp
  RenderTestSpec.*
  RenderTestRunner.*
  ImageComparator.*
  ManagedProcessWin32.*
```

`VulkanLabRenderTest.exe` 不链接 Renderer/Application/Vulkan，只链接：

- Runtime Control client/protocol
- JSON、ContentHash
- stb image/read/write
- Win32 process/job helper

它必须能够在没有 Vulkan SDK validation layer 的普通目标机器上控制 Release runtime；真正渲染能力仍由 VulkanLab 决定。

### Process Lifecycle

- Runner 创建唯一 pipe suffix、capture root、result root 和日志路径。
- 使用 Win32 Job Object 启动 VulkanLab，设置 kill-on-close。
- 工作目录显式设置为 runtime directory，不依赖调用者 CWD。
- 轮询 ping，启动超时输出进程 exit code/stdout/log。
- 测试完成先发送 `app.quit`；超时或协议失败再终止 Job。
- 所有路径使用宽字符 Win32 API，支持空格和非 ASCII project path。
- 正常/失败/取消后不得残留 VulkanLab 进程。

现有 AssetTool `ProcessRunner` 是阻塞式子进程执行器，不直接适合交互式长进程。可以复用其 quoting/Job Object 实现思路，但本轮不要强行用不匹配的接口；后续再评估抽取通用 ManagedProcess。

### Test Spec v1

```json
{
  "schemaVersion": 1,
  "name": "viking-legacy",
  "sceneId": "viking-room",
  "profileId": "desktop_2048",
  "shader": "Legacy Forward",
  "camera": {
    "position": [2.0, 2.0, 2.0],
    "yaw": -135.0,
    "pitch": -30.0
  },
  "viewport": [800, 600],
  "fixedDelta": 0.016666667,
  "stableFrames": 8,
  "includeGui": false,
  "mode": "smoke",
  "thresholds": {
    "minimumNonBlackRatio": 0.05,
    "maximumSolidColorRatio": 0.98
  }
}
```

Golden mode 额外记录：

- baseline PNG
- baseline metadata JSON
- per-channel absolute threshold
- MAE/RMSE limit
- bad-pixel ratio limit
- reference GPU vendor/device/driver
- baseline commit、shader hash、profile 和 viewport

### Runner Flow

1. 解析并严格验证 spec。
2. 启动 VulkanLab automation instance。
3. `system.ping/info`，检查 protocol/capability/build。
4. 按 stable scene ID 加载，等待 import/load terminal。
5. 设置 Shader 和 Camera。
6. 轮询 render status，等待稳定帧。
7. 请求无 GUI screenshot，轮询 capture status。
8. 加载 PNG 并执行 smoke/golden comparison。
9. 写 report JSON、actual PNG；失败时写 diff PNG。
10. 请求 quit、等待进程退出；必要时 Job kill。

任何一步失败都保留已有诊断，不用后续错误覆盖首个 root cause。

### Image Comparator

v1 使用 RGBA8 确定性指标：

- width/height/channel match
- per-channel absolute difference
- MAE
- RMSE
- maximum error
- bad-pixel count/ratio
- non-black、non-white 和 dominant-solid-color ratio

diff PNG：

- RGB 放大绝对差异，alpha 固定 255。
- 无尺寸匹配时生成带 metadata 的失败报告，不伪造 diff。
- comparator 是纯 CPU library，使用 8x8/fixture 单元测试覆盖已知值、overflow 和 alpha。

暂不实现 SSIM。只有 MAE/坏像素对真实场景不够稳定时再评估，不提前增加图像库。

### Baseline Policy

- 所有 GPU 执行 smoke mode。
- Golden mode 只在 metadata 匹配 reference GPU family 时 blocking；不匹配时报告 skipped golden，但 smoke 仍必须通过。
- `--accept` 是唯一允许写 baseline 的入口，默认命令不能覆盖。
- accept 前保存 previous/actual/diff，并要求人工视觉确认。
- baseline 使用小场景和合理 PNG；不提交 Main Sponza 全尺寸截图。

### Initial Suite

第一批：

1. Viking Room + Legacy Forward，smoke + reference golden。
2. Sheen Chair + PBR-lite NormalMapped，smoke。
3. Sheen Chair + Debug BaseColor，smoke，验证 shader switching。

Extended local：

- Main Sponza 1024 + PBR-lite NormalMapped，只做 smoke 和 LoadStats report。
- 连续加载/取消/resize/capture stress，不进入快速默认 suite。

### CTest Integration

- CPU spec/comparator/process argument tests 加入普通 CTest。
- GPU render tests 使用 label `gpu;visual`，默认 Hosted CI 不执行。
- 本地命令：

```powershell
ctest --preset windows-msvc-test -L unit
ctest --preset windows-msvc-test -L visual
```

- visual test 发现没有 Vulkan device、窗口无法 present 或 reference mismatch 时，错误/skip 原因必须结构化；不能静默成功。

### Required Verification

- 连续两次相同 Viking spec 在 reference GPU 阈值内。
- 故意改变临时测试 Shader 输出会让 golden 确定失败；恢复后通过，测试修改不提交。
- 实际 PNG、report 和 diff 路径均位于隔离 output root。
- 未启动 renderer、renderer crash、load fail、capture fail、compare fail、quit timeout 都有不同错误 code。
- 两个 Runner 并行使用不同 pipe/output，不冲突。
- 默认运行不启动 Runtime Control/capture worker。
- Cook package 文件集合不包含 RenderTest、golden 或 capture output。

### Exit Criteria

- 一条命令完成启动、加载、固定状态、截图、比较和清理。
- 自动化失败可以仅凭 report/log/actual/diff 初步定位，不依赖实时观察窗口。
- 至少三项快速 smoke 和一项 reference golden 可重复运行。
- Debug/Release、全部 CPU/asset/package tests 和 visual local suite 通过。
- 用户人工确认第一份 golden 与当前正确画面一致。

### Suggested Commits

1. `test: add render test specification and image comparator`
2. `test: add managed VulkanLab render test runner`
3. `test: add deterministic render smoke coverage`
4. `test: add approved reference visual baseline`
5. `docs: document automated visual regression workflow`

## Final Verification Matrix

### Build And Static

- clean Debug preset build。
- clean Release preset build。
- no-op rebuild，不调用 glslc，不复制 models。
- single Shader rebuild，只编译一个 SPIR-V。
- build 后工作树干净。
- `git diff --check` 和 Markdown local links。
- CMake 无全局 include/link 和 src glob。
- source tree/tracked files 无 SPIR-V。

### CPU And Asset Tests

- 所有既有 CPU tests。
- Catalog/SceneTypes boundary。
- Runtime dispatcher response compatibility。
- pipe/path/automation parser。
- capture state/serial/pixel conversion。
- RenderTest spec/image comparator/process lifecycle。
- KTX2 CatalogImport、TextureCache、Cook/package verify。

### Runtime

- Default startup，不创建 pipe/capture worker。
- Runtime Control default endpoint backward compatibility。
- unique endpoint 两实例隔离。
- Viking/Sheen/Main Sponza scene load。
- shader switching、camera set、resize、capture、quit。
- load/capture cancellation 和 renderer crash cleanup。
- core validation 无新增 error。
- 手动 synchronization validation screenshot run。

### Package

- Release cook 和 package verify。
- package 移出仓库、删除 source/cache 后运行。
- package 不含 RenderTest、golden、capture、developer docs 或 source SPIR-V。
- CookedOnly 对 capture 返回明确 capability error。

## Manual Work Required

开发代理可以完成代码、构建、CPU tests、Runtime Control 自动化、PNG 生成和差异计算。需要用户完成：

- M0 记录/确认当前代表场景画面没有已知回归。
- M5 使用 Vulkan Configurator 启用 synchronization validation，运行截图流程并提供日志。
- M7 检查并批准第一份 Viking Room golden。
- 在目标 GPU 上确认窗口后台运行不会因最小化/远程桌面停止 present。
- 最终检查 Main Sponza 画面和任务管理器显存曲线。

如果目标机器不支持 swapchain `TRANSFER_SRC` 或只提供非 8-bit format，本轮记录 capability failure，由后续 offscreen capture 计划处理，不在本轮扩大范围。

## Risks And Mitigations

| 风险 | 缓解措施 |
|---|---|
| 工程重构与截图同时造成难以定位回归 | M1-M4 每阶段独立提交并保持 Shader hash/场景行为不变。 |
| 静态库形成循环依赖 | 使用 SceneTypes/foundation/shader_catalog data boundary，第一版保持粗粒度 vkl_engine。 |
| 删除 models copy 后开发运行找不到资产 | 先实现 RuntimePaths 并保留复制，三种 CWD 验证后下一提交删除。 |
| Multi-config Shader 输出互相覆盖 | generated path 包含 config，runtime staging 显式依赖对应输出。 |
| Frame fence reset 使 screenshot 错过完成信号 | FrameSync 在 wait 成功、reset 前推进单调 completedSubmissionSerial。 |
| PNG 编码卡主线程 | 主线程只复制 CPU bytes，编码/hash 放 worker。 |
| 截图 barrier 或生命周期错误 | 同一 frame command buffer 记录 copy，使用 frame serial，手动 sync validation。 |
| Named Pipe 长等待阻塞命令处理 | 所有协议快速返回，Ctl/Runner 轮询 status。 |
| 路径穿越写入任意文件 | capture root + 规范化相对路径 + 原子发布。 |
| 跨 GPU golden 不稳定 | smoke 全平台，golden 只对 metadata 匹配 reference GPU blocking。 |
| 最小化窗口不再 present | Runner 保持可见窗口并检测 frameSerial stall，返回 window_not_rendering。 |
| Runner 异常留下 VulkanLab | 独立 Job Object kill-on-close，正常先走 app.quit。 |

## Assumptions

- 本轮目标是 Windows x64、MSVC 2022、GLFW 和 Vulkan SDK。
- 当前 swapchain 在主要目标 GPU 支持 `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` 和 8-bit BGRA/RGBA format。
- v1 自动化允许创建可见窗口，不要求 CI headless GPU。
- Graphics submit 全部进入同一 graphics queue，submission serial 可以按 queue 顺序判定完成。
- 当前 Scene update 可以使用 fixed delta/time；没有外部随机源必须在本轮统一 seed。
- Viking Room 和 Sheen Chair 在 fresh checkout 可用，适合作为快速 suite。
- Golden 接受需要人工审核，不能由自动任务自行批准。
- RenderTest 是开发工具，不进入 Cook package。
- 当前计划文档先单独提交，再开始 M0 代码基线。

## Completion Criteria

以下全部满足，才视为“已完成到工具链 Stage 1”：

- M0-M7 每个 exit criteria 达成并有 commit/test record。
- Target-based CMake、build-tree Shader、ProjectContext asset layout 已投入使用。
- Application 的 Runtime method switch 已提取，后续诊断命令有稳定 host boundary。
- Presets、BuildInfo、DiagnosticsConfig 和 diagnostics guide 可用。
- CaptureService 在目标 GPU 异步生成正确 PNG，无 queue/device idle。
- Runtime Control v3 支持 unique pipe、camera、render status 和 capture task。
- VulkanLabCtl 支持 render wait 和截图命令。
- VulkanLabRenderTest 完成端到端自动化和进程清理。
- 至少三项 smoke 和一项人工批准 golden 通过。
- Debug、Release、全部 CTest、Runtime smoke 和 package verify 通过。
- 构建/测试后工作树干净，Cook package 不含开发测试产物。
- 当前 guides/architecture 已更新，本文 Implementation Record 完整。

完成后归档本文。下一阶段按[开发诊断与自动化工具链计划](development_toolchain_plan.md)进入 RenderDoc labels/Validation profiles，而不是立即扩大 screenshot 架构。

## Implementation Record

| Milestone | Status | Commit(s) | Verification | Notes |
|---|---|---|---|---|
| M0 | Not started | | | |
| M1 | Not started | | | |
| M2 | Not started | | | |
| M3 | Not started | | | |
| M4 | Not started | | | |
| M5 | Not started | | | |
| M6 | Not started | | | |
| M7 | Not started | | | |
