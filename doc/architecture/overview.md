# 系统架构概览

> Status: Current
> Last verified: 2026-08-22
> Verified against: `48f0c5b`

VulkanLab 是一个开发中的 Windows Vulkan 1.3 实时渲染器。当前架构以
`Application` 为组合根，以类型化 service、snapshot、action 和 submission
serial 明确场景、渲染、编辑器与开发工具之间的所有权；不存在全局引擎 service
locator。

## 运行时所有权

```text
Application
  |-- PlatformServices
  |     Window / Input / VulkanContext / Device / FrameSync
  |     DescriptorAllocator / MaterialSystem / Renderer / PipelineCache
  |-- RuntimeServices
  |     SceneWorkflowController
  |     SceneRuntimeCoordinator
  |     RenderSettingsController
  |-- OptionalTooling
  |     EditorController              [optional]
  |     RuntimeControlAdapter         [optional]
  |     CaptureService                [optional]
  `-- FrameState
        Camera / ShadowSystem / VisibilitySystem / current frame snapshots
```

`Application` 只负责依赖装配、平台生命周期、主循环顺序、少量跨 service
协调和显式逆序关闭。它不绘制具体 ImGui 页面、不解析 Runtime Control JSON、
不执行 scene load phase 状态机，也不持有 Catalog/ArtifactIndex 的第二份可变状态。

三个主要 Controller 的职责固定为：

- `SceneWorkflowController`：Catalog、Scene Registry、验证、导入、派生资产、
  Artifact Index、任务状态和统一 UI/Runtime Control workflow action。
- `SceneRuntimeCoordinator`：Asset/Environment Repository、SceneLoadManager、当前
  和退役 World、异步发布、environment generation、加载统计和 GPU 生命周期。
- `RenderSettingsController`：Render Path、View Mode、requested settings、参数校验、设备
  capability fallback、active runtime state 和 `FrameRenderFeatures` 解析。

`EditorController` 只在 `VKL_ENABLE_EDITOR_UI=ON` 时存在，拥有 Docking workspace、
Viewport、authoring session、Undo/Redo、panels 和纯 UI 状态。
`RuntimeControlAdapter` 只在编译支持且启动参数显式开启时存在，负责 Named Pipe
协议与 typed service/action 的边界映射。两者复用相同的 workflow、scene runtime
和 render settings action，不复制业务规则。

## 源码模块

| 目录 | 当前职责 |
|---|---|
| `src/app/` | `Application` 组合根、启动配置和 canonical frame scheduling。 |
| `src/core/` | Vulkan instance/device、SwapChain、FrameSync、Buffer/Image、descriptor、VMA、上传和底层同步。 |
| `src/render/` | Renderer facade、feature graph 和只读 world contract。 |
| `src/render/frame/` | Render settings、feature resolution、RenderView、frame GPU ABI 和灯光 DTO。 |
| `src/render/graph/` | RenderGraph、`IRenderPass`、External nodes 与物理 `RenderResourcePool`。 |
| `src/render/geometry/` | Mesh、Vertex、bounds、RenderItem、RenderQueue 和 tangent。 |
| `src/render/material/` | Texture、Material、fallback resources 和全局 MaterialSystem。 |
| `src/render/pipeline/` | Graphics/Compute pipeline config、cache 和完整 state key。 |
| `src/render/shader/` | Shader Manifest registry、Material Shader Family、View Mode 和 Renderer program catalog。 |
| `src/render/features/` | 按功能族组织的全部 Graph Pass 和算法实现。 |
| `src/scene/` | `IRenderWorld` 实现、RuntimeWorld、ModelAsset、Repository、GPU builder 和场景加载。 |
| `src/scene_data/` | 无 Vulkan/Renderer/ImGui 依赖的 SceneDocument DTO、持久 ID、验证和原子存储。 |
| `src/assets/` | Project/Catalog、manifest、artifact/cache、验证、导入和 package 数据层。 |
| `src/workflows/` | 项目级 Scene/Asset workflow controller 与 typed snapshots/actions。 |
| `src/editor/` | Editor controller、Docking UI、Viewport、authoring session 和 panels。 |
| `src/control/` | Named Pipe、命令队列、协议、客户端与可选 runtime adapter。 |
| `src/diagnostics/` | BuildInfo、加载统计、Capture、Tracy wrapper 和 submission diagnostics。 |
| `src/window/`, `src/platform/` | GLFW 输入/窗口和 Win32 平台适配。 |
| `tools/` | AssetTool、VulkanLabCtl 和 RenderTest 等进程外开发工具。 |

Renderer 的功能目录直接对应维护边界：

```text
src/render/features/
  forward/                  Opaque/Transparent Forward drawing
  deferred/                 Deferred Lighting and path outputs
  surface/                  SurfacePrepass, GBuffer and Depth Hierarchy
  lighting/                 Shared Clustered Light Culling
  post_process/             HDR Composite, ToneMap and Present
  shadows_visibility/       CSM, Point/Spot Shadow, Visibility, Hi-Z
  ambient_occlusion/        SSAO, GTAO, CACAO adapter/backend
  reflections/              SSR
  global_illumination/      SSGI, DDGI, Ray Query scene
  atmosphere_environment/   Atmosphere LUTs and sky background
  temporal_post_process/    TAA, Bloom and shared screen pyramids
```

这些目录属于同一个 `vkl_renderer_runtime` target。没有把每个算法强拆成静态库，
因为它们共享稳定的 RenderGraph、frame ABI、Pipeline 和 resource handle contract；
物理目录和 CMake source group 用于明确所有权，而不是制造 target 间循环依赖。

## CMake 模块边界

根 `src/CMakeLists.txt` 只声明模块顺序并组装 `VulkanLab` executable。各 target
由其主要 owner 目录中的 `CMakeLists.txt` 定义：

```text
src/core/          foundation + GPU runtime
src/platform/      window/input platform runtime
src/render/        shader catalog + renderer runtime
src/assets/        asset core + asset runtime
src/diagnostics/   capture
src/scene_data/    scene data
src/scene/         scene runtime
src/workflows/     scene workflow
src/control/       protocol/server + optional runtime adapter
src/editor/        optional editor
src/CMakeLists.txt Application executable assembly
```

这些 owner 文件以真实依赖建立以下 target：

- `vkl_foundation`, `vkl_gpu_runtime`, `vkl_platform_runtime`
- `vkl_shader_catalog`, `vkl_renderer_runtime`
- `vkl_scene_data`, `vkl_asset_core`, `vkl_asset_runtime`
- `vkl_scene_runtime`, `vkl_scene_workflow`
- `vkl_capture`

开发者命令入口位于 `tools/dev/`。`Configure-Project.ps1`、
`Build-Developer.ps1` 和 `Build-Runtime.ps1` 只选择上述 CMake preset/target；
`Cook-Package.ps1` 和 `Verify-Package.ps1` 只串联 Runtime BuildInfo 检查与
`VulkanLabAssetTool`。场景闭包、Validator、派生资产 admission、package hash 和原子
发布仍由 AssetTool 数据层独占，PowerShell 不维护第二套 package 规则。
- `vkl_editor` 和 `vkl_runtime_control_adapter`（条件构建）

`VulkanLab` executable 只直接编译 `main.cpp` 与 `app/Application.cpp`，再链接所需
模块。测试和工具直接链接其实际使用的模块，不再经过一个隐藏依赖关系的
`vkl_engine` 聚合 target。面向开发者的构建入口使用无链接语义的 custom aggregate
targets，例如 `VulkanLabRuntimeImage`、`VulkanLabDeveloper` 和 `VulkanLabFull`。

项目编译接口按职责拆分为：

- `VulkanLab::ProjectOptions`：C++ language/ABI 和项目通用编译定义。
- `VulkanLab::ProjectWarnings`：项目代码 warning policy。
- `VulkanLab::RuntimeFeatures`：生成的 `RuntimeFeatures.h` include 路径。
- `VulkanLab::BuildOptions`：前三者的运行时聚合接口。

进程外 host tools 和测试只按需链接 ProjectOptions/ProjectWarnings，不继承运行时
feature header。第三方 target 不继承项目 warning policy。第三方依赖配置按
`Core`、`Rendering`、`Editor`、`AssetTools` 和 `Diagnostics` 分文件管理；Editor、
DirectXTex、Tracy、CACAO 和 SPIR-V Reflect 只在对应 feature 或 product 需要时进入
生成图。KTX 的 read-only runtime 部分仍是所有配置都需要的派生纹理读取依赖。

Shader构建分为`VulkanLabShaderCompile`和`VulkanLabShaders`。前者依据Manifest生成
去重后的variant job，使用`glslc` depfile跟踪真实include闭包，并在`spirv-val`成功后
发布canonical generated SPIR-V；后者只在全部编译成功后集中装配runtime shader镜像
并清理陈旧输出。Shader源码、generated产物和runtime镜像因此具有明确的所有权与失败
边界，单个编译失败不会替换上一次可运行镜像。

Developer runtime镜像使用owner级声明装配静态payload。Renderer owner管理project
locator、Editor字体与可选CACAO许可；AssetTool owner管理外置glTF Validator。每个
owner保存per-config的desired、known和previously-owned清单，构建时只清理明确登记的
陈旧路径，并用`ONLY_IF_DIFFERENT`同步当前文件。Executable仍由其target直接输出，
Shader仍由专用publisher管理，因此runtime装配不会形成重复复制链，也不会触碰日志、
截图、Workspace或Cooked package。

构建开关只裁剪开发基础设施，不切换渲染算法 ABI。Editor、Runtime Control、
Capture、Asset Authoring、Validation、Debug Utils、GPU Profiler 和 Tracy 可以独立
移除；Global UBO、Material ABI、Shader Manifest 和 SceneDocument 不因这些开关
产生不同版本。

## 初始化与关闭

`wmain()` 在窗口和 Vulkan 初始化前解析 project/package、构建能力和启动参数。
`Application::init()` 随后按三个阶段装配：

1. `initPlatformAndRenderer()`：Window/Input、VulkanContext、Device、descriptor、
   MaterialSystem、SwapChain、FrameSync、Renderer、RenderSettings capability 和
   PipelineCache。
2. `initSceneRuntime()`：SceneLoadContext、SceneRuntimeCoordinator、workflow
   callbacks、Catalog/Artifact workflow 和初始异步 scene/environment request。
3. `initOptionalTooling()`：Capture、Editor 和显式启用的 Runtime Control。

`shutdown()` 是幂等的。它先停止外部控制和后台 workflow/runtime，再等待最后的
GPU submission，收割退役 World/asset/capture，最后按
Optional Tooling -> Scene Runtime -> frame state -> Vulkan platform 的逆依赖顺序
销毁。普通场景切换和 feature 切换不使用 `vkDeviceWaitIdle()`。

## Canonical Frame Order

每帧顺序由 `Application::mainLoop()` 固定：

```text
Window::pollEvents
RuntimeControlAdapter::processOne                 [optional]
SceneWorkflowController::pump
SceneRuntimeCoordinator::pump
EditorController::beginFrame/draw                [optional]
apply editor/runtime actions
FrameSync::beginFrame
IRenderWorld::buildRenderSnapshot
ShadowSystem::buildPlan
RenderSettingsController::resolve
buildRenderView
VisibilitySystem::build
Renderer::renderFrame
FrameSync::endFrame / present
capture completion and serial retirement
```

UI 修改完成后才构建一次不可变 `RenderWorldFrameSnapshot`。RenderView、Shadow、
Visibility、Renderer 和 diagnostics 使用相同 generation，避免同一帧混用编辑前后
的 Transform、灯光或 bounds。Renderer 内部由 `FrameRenderFeatures` 选择 Graph
拓扑和物理资源；Pass 不回到 Application 推导执行条件。

## 线程模型

主线程拥有 GLFW、ImGui、所有 Vulkan/VMA 对象、MaterialSystem、descriptor 更新、
GPU builder pump 和资源发布。后台线程只产生 CPU 数据或监督进程：

- AssetRepository worker：glTF 解析、图片准备、geometry/tangent/bounds。
- SceneLoadManager worker：SceneDocument 解析和引用解析。
- Environment worker：派生 KTX2 读取和校验。
- Asset import supervisor：Validator/AssetTool 子进程和 NDJSON 进度。
- Capture worker：已经完成 readback 的 PNG 编码与哈希。
- Named Pipe thread：请求 I/O 和排队，不读取 Renderer/World 或调用 Vulkan。

GPU 对象只在主线程发布。ModelAsset、Environment、World、Material slot 和 Graph
resource 都使用 FrameSync submission serial 延迟回收，不依赖后台线程猜测 GPU
完成状态。

## 相关文档

- [渲染流程](rendering.md)
- [RenderGraph](render_graph.md)
- [资源加载](resource_loading.md)
- [场景数据与 Catalog](scene_documents.md)
- [编辑器 UI](../guides/editor_ui.md)
- [Runtime Control](../guides/runtime_control.md)
