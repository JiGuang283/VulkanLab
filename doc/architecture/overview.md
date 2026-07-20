# 系统架构概览

> Status: Current
> Last verified: 2026-07-20
> Verified against: `4bcabe9`

VulkanLab 是一个 Windows Vulkan Forward Renderer。当前架构以 `Application` 为组合根，场景、渲染提交、GPU 资源和调试控制之间保持显式所有权，不使用全局引擎服务定位器。

## 模块边界

| 目录 | 职责 |
|---|---|
| `src/app/` | 应用生命周期、场景注册与切换、相机、ImGui 面板、全局 UBO 和命令执行。 |
| `src/assets/` | ProjectContext、Scene Catalog/编辑事务、RuntimePackage、ArtifactIndex/依赖校验、cache prune、资产工具进程监督、manifest 和 KTX2 cache 读取。 |
| `src/control/` | Windows Named Pipe 服务、运行时命令队列和 JSON 协议。 |
| `src/core/` | Vulkan instance/device、SwapChain、FrameSync、Buffer/Image、Descriptor、Pipeline、VMA、同步与增量上传。 |
| `src/render/` | Mesh、Texture、材质、纯 CPU glTF prepare、RenderQueue、PipelineCache、Renderer 和 Shader variant。 |
| `src/render/pass/` | RenderPipeline 中的具体 pass；当前只有 `MainForwardPass`。 |
| `src/scene/` | Scene、SceneObject、SceneLight、Camera、prepared data、加载任务、GPU builder、SceneFactory 和内建场景。 |
| `src/window/` | GLFW 窗口和输入状态。 |
| `src/platform/` | Win32 原生文件选择等平台适配。 |
| `src/diagnostics/` | 场景加载耗时、资源上传量和 VMA 快照数据结构。 |
| `tools/` | 独立资产、Runtime Control 和视觉回归程序；不拥有渲染器内部对象。 |

## 启动与所有权

`wmain()` 解析 `--help`、`--project`、`--runtime-control`、`--runtime-control-pipe`、`--asset-mode`、确定性诊断参数与可选 cache/tool override，入口和路径全程保留 Windows Unicode。`ProjectContextResolver` 优先识别 executable 旁的 runtime package；否则定位源码项目和 `assets/catalog.json`，并一次性确定 `projectRoot`、`runtimeRoot`、`cacheRoot` 与 `captureRoot`。Catalog/glTF/GLB、外部依赖和 builtin 源资产从 `projectRoot` 解析；executable、运行时工具、locator 和 SPIR-V 从 `runtimeRoot` 解析。package file hash、Catalog schema、稳定 ID、profile、路径和必需源文件都会在创建 Window/Vulkan 前验证。随后 `SceneRegistryBuilder` 把 Catalog 条目适配为现有 `SceneEntry`，`main.cpp` 不再逐个登记 glTF 场景。

Cooked package 中 `projectRoot == runtimeRoot == package root`，cache 固定为包内 `runtime_assets`；它使用只读 Catalog、强制 CookedOnly 和固定 profile，并关闭开发 validation layer。外部 project/cache/asset tool override 会在初始化前被拒绝。开发运行默认 OnDemand，保留 CMake locator、writable Catalog 和共享用户 cache。当前工作目录不参与 subsystem 的资源拼接。

初始化顺序为：

1. ProjectContext、SceneCatalog 和 SceneRegistry，此时尚未创建窗口。
2. Window 和 InputManager。
3. VulkanContext、Device 和 DescriptorAllocator。
4. SwapChain 和 FrameSync。
5. Renderer、全局 UBO、RenderPipeline、MainForwardPass 和开发模式 CaptureService。
6. PipelineCache、SceneLoadManager worker、ArtifactIndex 和初始 Scene/admission；只有 OnDemand 创建 AssetImportManager supervisor。
7. GuiSystem。
8. 可选的 Runtime Control 命令队列和 Named Pipe 线程。

这些对象由 `Application` 持有，并通过成员声明与显式停止逻辑按相反方向销毁。Vulkan 资源的创建、使用、场景替换和销毁都发生在主线程。

## 线程模型

主线程拥有 GLFW、ImGui、DescriptorAllocator 和全部 Vulkan/VMA 对象。它每帧轮询 upload fence、按预算记录上传命令，并在资源全部可用后发布 Scene。

SceneLoadManager 持有一个长期 worker。worker 只执行 glTF 文件读取、解析、图片解码/缩放、顶点转换、tangent、bounds 和 hierarchy，输出不包含 Vulkan handle 的 `PreparedSceneData`。它通过 atomic 进度/取消标记和受 mutex 保护的结果与主线程通信。

Scenes 面板的文件对话框在主线程打开；选定文件后的依赖 preflight 和 `SceneImportService` 事务由独立 `std::async` worker 执行。该 worker 可以读取/复制源文件并原子更新源码项目 Catalog，但不能访问 GLFW、ImGui 或 Vulkan。Application 只轮询 future 和进度；退出时先请求取消并等待导入 worker 收束。

AssetImportManager 持有一个 supervisor thread，串行监督资产工具进程；每个工具内部再按 worker/内存预算并行启动 `ktx.exe`。supervisor 只读取 NDJSON、日志和进程状态，不解析 glTF、不访问 Application Scene，也不创建 Vulkan 对象。Windows Job Object 拥有完整子进程树，取消和退出会终止工具及其编码子进程。主线程轮询任务状态，并由 `AssetLoadCoordinator` 保证只有最新 operation generation 可以从 import 接续到 scene load。

ArtifactIndex 由 Application 主线程持有；Fast/Admission 查询和 UI 快照不跨线程访问。独立资产工具负责 manifest/blob 和索引核心记录的发布，Application 只合并访问/失败遥测。不同进程通过短时 index mutex 原子更新 JSON；改变 cache 内容的工具命令还持有覆盖完整事务的 cache mutation mutex。

Named Pipe 线程只读取带长度前缀的 JSON 请求，把 `RuntimeCommand` 放入队列并等待主线程填写响应。它不能读取 Scene、Camera、Shader、统计数据，也不能调用 Vulkan 或 GLFW。Runtime Control v3 的 scene/capture 请求快速返回 taskId；加载等待和稳定帧等待都由 VulkanLabCtl 使用短连接轮询 `load.status`、`render.status` 或 `capture.status`，服务端不会阻塞等待未来帧。每个自动化实例可以使用独立 pipe suffix。

CaptureService 的主线程部分创建 readback buffer、记录 image copy 并按 FrameSync completed submission serial 收割 GPU 结果。惰性启动的编码 worker 只处理已复制到 CPU 的 RGBA bytes、PNG 和 SHA-256，不访问 Vulkan、GLFW、ImGui 或 Scene。

`VulkanLabRenderTest` 是渲染器进程外的测试工具，不链接 Application、Renderer 或 Vulkan。它使用唯一 Named Pipe 和 Win32 Job Object 启动并监督 `VulkanLab.exe`，通过 Runtime Control 完成场景加载、稳定帧等待和异步截图，再在 CPU 上比较 PNG。Runner 崩溃或超时关闭 Job 时会终止完整子进程树。

## 每帧数据流

1. 轮询窗口和输入，收割 asset import 结果，执行一条待处理控制命令。
2. 轮询 worker 结果与 upload fence，在软预算内推进 SceneGpuBuilder。
3. 应用待切换场景，更新计时、输入模式、相机和 Scene tick。
4. 轮询场景导入 future，构建 Scenes、Loading 和最近一次 LoadStats 等 ImGui 界面。
5. `FrameSync::beginFrame()` 获取 frame index、swapchain image 和 command buffer。
6. Application 把相机、环境光和 SceneLight 写入当前帧 GlobalUBO。
7. Scene 生成 RenderCommand，RenderQueue 分别排序 opaque 与 transparent 命令。
8. Renderer 组装 RenderFrameContext，RenderPipeline 执行 MainForwardPass，并在同一 render pass 末尾按需绘制 ImGui。
9. 若有截图任务，在同一个 frame command buffer 中把 swapchain image 复制到 readback buffer，然后恢复 present layout。
10. `FrameSync::endFrame()` 提交和 present；需要时重建 SwapChain 相关资源。后续帧推进 completed submission serial，并把已完成截图交给 CPU worker。

详细渲染行为见 [渲染流程](rendering.md)，场景创建与上传见 [资源加载](resource_loading.md)。
