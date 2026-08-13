# 系统架构概览

> Status: Current
> Last verified: 2026-08-12
> Verified against: Vulkan 1.3 RenderGraph and Dynamic Rendering implementation

VulkanLab 是一个 Windows Vulkan Forward Renderer。当前架构以 `Application` 为组合根，场景、渲染提交、GPU 资源和调试控制之间保持显式所有权，不使用全局引擎服务定位器。

## 模块边界

| 目录 | 职责 |
|---|---|
| `src/app/` | 应用生命周期、SceneWorkflowController、模型预览切换、相机、RenderView 输入和命令执行。 |
| `src/editor/` | ImGui DockSpace、独立 Scene Viewport、SceneEditorSession、Outliner/Inspector/Scenes/Assets panel、主题、共享控件和纯 UI 状态。 |
| `src/assets/` | ProjectContext、Model/SceneDocument/Environment Catalog、类型化存储事务、RuntimePackage、ArtifactIndex/依赖校验、cache prune、资产工具进程监督、manifest 和 KTX2 cache 读取。 |
| `src/scene_data/` | 无 Renderer/ImGui 依赖的持久 Entity ID、SceneDocument DTO、严格验证和原子存储。 |
| `src/workflows/` | UI 与 Application 之间的只读 workflow snapshot 和 action DTO。 |
| `src/control/` | Windows Named Pipe 服务、运行时命令队列和 JSON 协议。 |
| `src/core/` | Vulkan instance/device、SwapChain、FrameSync、Buffer/Image、AccelerationStructure、Descriptor、Pipeline、VMA、同步与增量上传。 |
| `src/render/` | Mesh、Texture、材质、纯 CPU glTF prepare、Environment GPU build、RenderView、RenderQueue、RenderGraph、物理 RenderResourceRegistry、RayTracingScene、PipelineCache、Renderer、GPU profiler 和 Shader variant。 |
| `src/render/pass/` | RenderGraph 的 graphics/compute/transfer/external pass，包括 Atmosphere、Shadow、Surface/Visibility、AO、DDGI、Forward、SSR/SSGI、TAA、Bloom、ToneMap、Present 和 Capture。 |
| `src/scene/` | IRenderWorld、RuntimeWorld、兼容 Scene facade、ModelAsset/ModelInstance、AssetRepository、ModelGpuBuilder、Native Scene 加载任务、Camera、SceneFactory 和内建场景。 |
| `src/window/` | GLFW 窗口和输入状态。 |
| `src/platform/` | Win32 原生文件选择等平台适配。 |
| `src/diagnostics/` | 场景加载耗时、资源上传量、VMA 快照、截图与自动化诊断。 |
| `tools/` | 独立资产、Runtime Control 和视觉回归程序；不拥有渲染器内部对象。 |

## 编译期模块装配

`cmake/BuildFeatures.cmake` 是可选开发基础设施的唯一构建期开关入口，生成的 `BuildFeatures.h` 同时提供 `0/1` 宏和 `vkr::build` 常量。CMake 在 target/source 边界选择真实实现或空实现：Editor 可移除 ImGui，Runtime Control 可移除 Named Pipe 控制库，Capture 可移除截图后端并停止请求 swapchain transfer-source usage，Asset Authoring 可替换进程 supervisor，GPU Debug Utils 和 GPU Profiler 可替换为 no-op 实现。

这些开关不参与渲染算法选择。Directional Shadow、HDR/Tone Mapping、Compute Bloom、IBL、Skybox、程序化 Atmosphere、Global UBO、descriptor layout、push constant、材质布局和 Shader Manifest 在所有 preset 中保持同一契约。运行时算法仍由 `RenderSettings`、SceneDocument component 和 Shader variant 控制，避免不同二进制产生不兼容资产或 Shader ABI。

## 启动与所有权

`wmain()` 解析 `--help`、`--project`、`--runtime-control`、`--runtime-control-pipe`、`--asset-mode`、确定性诊断参数与可选 cache/tool override，入口和路径全程保留 Windows Unicode。`--build-info-json` 在日志、Window 和 Vulkan 初始化前输出机器可读的构建配置。`ProjectContextResolver` 优先识别 executable 旁的 runtime package；否则定位源码项目和 `assets/catalog.json`，并一次性确定 `projectRoot`、`runtimeRoot`、`cacheRoot` 与 `captureRoot`。Catalog/glTF/GLB、外部依赖和 builtin 源资产从 `projectRoot` 解析；executable、运行时工具、locator 和 SPIR-V 从 `runtimeRoot` 解析。package file hash、Catalog schema、稳定 ID、profile、路径和必需源文件都会在创建 Window/Vulkan 前验证。开发项目中 `SceneRegistryBuilder` 先把 `models[]` 适配为单模型预览，再追加 Catalog v3 `scenes[]` 对应的 Native Scene Entry，保持旧预览索引稳定；`main.cpp` 不再逐个登记 glTF 模型。

schema v3 Cooked package 中 `projectRoot == runtimeRoot == package root`，cache 固定为包内 `runtime_assets`；它使用只读最小 Catalog、强制 CookedOnly，只注册 Native Scene，并自动加载 manifest 的 `startupSceneId`。Model 各自使用 Catalog import profile，Environment 由 SceneDocument 选择。`windows-msvc-runtime` 在编译期移除 Editor、Runtime Control、Capture、Asset Authoring 和开发诊断；外部 project/cache/asset tool override 会在初始化前被拒绝。旧 schema v1/v2 package 继续注册 Model Preview。开发运行默认 OnDemand，保留 CMake locator、writable Catalog 和共享用户 cache。当前工作目录不参与 subsystem 的资源拼接。

初始化顺序为：

1. ProjectContext、SceneCatalog、SceneWorkflowController 和模型预览 SceneRegistry，此时尚未创建窗口。
2. Window 和 InputManager。
3. VulkanContext、Device 和 DescriptorAllocator。
4. SwapChain 和 FrameSync。
5. Renderer、全局 UBO、Lighting descriptor generation、物理 render resource registry、Vulkan 1.3 RenderGraph、GPU timestamp profiler 和开发模式 CaptureService。
6. PipelineCache、AssetRepository/EnvironmentAssetRepository worker、SceneLoadManager 操作 facade、ArtifactIndex 和初始 Scene/environment admission；只有 OnDemand 创建 AssetImportManager supervisor。
7. GuiSystem。
8. 可选的 Runtime Control 命令队列和 Named Pipe 线程。

这些对象由 `Application` 持有，并通过成员声明与显式停止逻辑按相反方向销毁。Vulkan 资源的创建、使用、场景替换和销毁都发生在主线程。

## UI 所有权

`Application::drawGui()` 是 DockSpace 与编辑器窗口的组合入口。`EditorDockWorkspace`
创建 Viewport、Outliner、Inspector、Scenes、Assets、Render、Materials 和 Diagnostics 顶层窗口，并
提供 Viewport、Debugging 和 Compact 三种 preset。`EditorTheme` 统一字体、DPI、
颜色和间距，`EditorWidgets` 提供属性表、语义状态、路径显示和模式选择。
`EditorUiState` 只保存筛选、选中 index 和短期性能历史；`EditorViewportState`
只报告图像矩形、逻辑/物理尺寸以及 visible、hovered、focused，不持有 Scene、
Device 或图像所有权。窗口位置、尺寸和 docking 状态继续由 ImGui ini 管理。

`ScenesPanel`、`AssetsPanel`、`OutlinerPanel` 与 `InspectorPanel` 位于 `src/editor/panels/`，只消费 snapshot 和 action callback，不持有 Catalog、future、Application 或 Vulkan 对象。`SceneEditorSession` 与 `EditorCommandStack` 维护文档路径、Dirty/Undo 状态和 UUID selection；实际编辑数据只存在于共享 `RuntimeWorld`。Catalog、registry、模型导入和资产任务状态由 `SceneWorkflowController` 统一持有；Application 只组装需要窗口、相机或 Vulkan 生命周期的 action。

具体页面入口见 [编辑器 UI 工作区](../guides/editor_ui.md)，场景数据边界见[场景数据与 Catalog](scene_documents.md)。

## 线程模型

主线程拥有 GLFW、ImGui、DescriptorAllocator 和全部 Vulkan/VMA 对象。它每帧轮询 upload fence、按预算记录上传命令，并在资源全部可用后发布 Scene 或新的 Environment descriptor generation。

AssetRepository 持有一个长期 FIFO worker。worker 只执行 glTF 文件读取、解析、图片解码/缩放、顶点转换、tangent、bounds 和 hierarchy，输出不包含 Vulkan handle 的 `PreparedModelData`。主线程按预算推进唯一活动 `ModelGpuBuilder`，并把完成结果发布为共享 `ModelAsset`。相同 `(modelId, profileId)` 请求会 Ready hit 或合并到同一 generation。

SceneLoadManager 的单 worker 负责解析和验证 Native SceneDocument；主线程随后请求文档中的唯一 ModelAsset 集合并等待 environment。全部依赖 Ready 后构造 `RuntimeWorld` 并原子替换当前 `IRenderWorld`。加载失败、取消或发布前出现新的未保存编辑时保留旧 World；旧 World 按 submission serial 延迟销毁。

EnvironmentAssetRepository 持有独立 FIFO worker，只读取和校验已经离线 bake 的四个浮点 KTX2，输出 `PreparedEnvironment`。主线程使用唯一活动 EnvironmentGpuBuilder 和增量上传队列创建 cubemap/LUT；全局环境与 Reflection Probe 对相同 `(environmentId, profileId)` 共享 generation 和 consumer lease。完整新 generation 发布前保留旧环境，旧 descriptor/resources 按 submission serial 延迟销毁。

`VulkanLab -> Scene -> Scenes` 的文件对话框在主线程打开；选定文件后的依赖 preflight 和 `ModelImportService` 事务由独立 `std::async` worker 执行。该 worker 可以读取/复制源文件并通过 `SceneCatalogStore` 原子更新源码项目 Catalog，但不能访问 GLFW、ImGui 或 Vulkan。Application 只轮询 controller 中的 future 和进度；退出时先请求取消并等待导入 worker 收束。

AssetImportManager 持有一个 supervisor thread，串行监督资产工具进程；scene texture 工具内部再按 worker/内存预算并行启动 `ktx.exe`，environment 工具执行确定性 CPU bake。supervisor 只读取 NDJSON、日志和进程状态，不解析 glTF/HDR、不访问 Application Scene，也不创建 Vulkan 对象。Windows Job Object 拥有完整子进程树，取消和退出会终止工具及其编码子进程。主线程轮询任务状态，并由 `AssetLoadCoordinator` 保证只有最新 operation generation 可以从 import 接续到 scene load。

ArtifactIndex 由 Application 主线程持有；Fast/Admission 查询和 UI 快照不跨线程访问。独立资产工具负责 manifest/blob 和索引核心记录的发布，Application 只合并访问/失败遥测。不同进程通过短时 index mutex 原子更新 JSON；改变 cache 内容的工具命令还持有覆盖完整事务的 cache mutation mutex。

Named Pipe 线程只读取带长度前缀的 JSON 请求，把 `RuntimeCommand` 放入队列并等待主线程填写响应。它不能读取 Scene、Camera、Shader、统计数据，也不能调用 Vulkan 或 GLFW。Runtime Control v3 的 scene/capture 请求快速返回 taskId；加载等待和稳定帧等待都由 VulkanLabCtl 使用短连接轮询 `load.status`、`render.status` 或 `capture.status`，服务端不会阻塞等待未来帧。每个自动化实例可以使用独立 pipe suffix。

CaptureService 的主线程部分按请求为最终 Swapchain Workspace、per-frame Viewport Color 或 HDR source 创建 readback buffer；Renderer 把实际 image copy 注册为条件 RenderGraph Transfer 节点，并按 FrameSync completed submission serial 收割 GPU 结果。惰性启动的编码 worker 只处理已复制到 CPU 的 RGBA bytes、PNG 和 SHA-256，不访问 Vulkan、GLFW、ImGui 或 Scene。

`VulkanLabRenderTest` 是渲染器进程外的测试工具，不链接 Application、Renderer 或 Vulkan。它使用唯一 Named Pipe 和 Win32 Job Object 启动并监督 `VulkanLab.exe`，通过 Runtime Control 完成场景加载、稳定帧等待和异步截图，再在 CPU 上比较 PNG。Runner 崩溃或超时关闭 Job 时会终止完整子进程树。

## 每帧数据流

1. 轮询窗口和输入，收割 asset import 结果，执行一条待处理控制命令。
2. 轮询 worker 结果与 upload fence，在软预算内推进 AssetRepository/ModelGpuBuilder 和 EnvironmentGpuBuilder；收割达到 submission serial 的旧 Scene/ModelAsset。
3. 应用待切换场景，更新计时、输入模式、相机和 Scene tick。
4. 轮询场景导入 future，构建全屏 DockSpace 及 Viewport、Scenes、Assets、Render、Materials 和 Diagnostics 窗口；Viewport 报告内容区尺寸和交互状态，并显示对应 frame slot 的 Viewport Color。
5. `FrameSync::beginFrame()` 获取 frame index、swapchain image 和 command buffer。
6. Application 组装 `RenderViewInput`；纯函数 `buildRenderView()` 完成默认 Sun、灯光截断/GPU 打包、阴影拟合、Atmosphere Sun 选择、大气 frame data 和 DDGI Probe Volume frame data，生成不可变 `RenderView`。
7. 当前 `IRenderWorld` 从 legacy SceneObject/预览 ModelInstance 或 RuntimeWorld Entity 生成 RenderCommand；Native Scene 实例矩阵使用 `entityWorld * localToAsset`，RenderQueue 分别排序 opaque 与 transparent 命令。
8. Renderer 上传 Global UBO、Scene Light SSBO 和 Atmosphere UBO，按需从 canonical Render Items 构建当前 frame slot TLAS，并组装 RenderFrameContext；RenderGraph 根据 FrameRenderFeatures 构建/复用拓扑，自动生成 Synchronization2 barrier，并通过 Dynamic Rendering、Compute、Transfer 和 External 节点执行 Atmosphere/Shadow/Surface/Visibility、AO、可选 DDGI、SkyBackground、Forward、SSR/SSGI composite、Transparent、TAA、Bloom、ToneMap 与 Present + ImGui。每个活动节点由稳定 Graph Pass ID 的 timestamp query 包围。
9. 若有截图任务，条件 ScreenshotCopy Transfer 节点在同一个 frame command buffer 中复制最终 Swapchain Workspace、Viewport Color 或 HDR source；资源 layout 由 Graph 恢复。
10. `FrameSync::endFrame()` 提交和 present；操作系统窗口变化只重建 Swapchain/Present 资源，稳定后的 Viewport 内容区变化只重建 viewport-dependent Registry 资源。后续帧推进 completed submission serial，并把已完成截图交给 CPU worker。

详细渲染行为见 [渲染流程](rendering.md)，场景创建与上传见 [资源加载](resource_loading.md)。
