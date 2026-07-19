# 系统架构概览

> Status: Current
> Last verified: 2026-07-19
> Verified against: `df02615`

VulkanLab 是一个 Windows Vulkan Forward Renderer。当前架构以 `Application` 为组合根，场景、渲染提交、GPU 资源和调试控制之间保持显式所有权，不使用全局引擎服务定位器。

## 模块边界

| 目录 | 职责 |
|---|---|
| `src/app/` | 应用生命周期、场景注册与切换、相机、ImGui 面板、全局 UBO 和命令执行。 |
| `src/assets/` | ProjectContext、Scene Catalog、场景导入事务、派生资产身份、manifest 和 KTX2 cache 读取。 |
| `src/control/` | Windows Named Pipe 服务、运行时命令队列和 JSON 协议。 |
| `src/core/` | Vulkan instance/device、SwapChain、FrameSync、Buffer/Image、Descriptor、Pipeline、VMA、同步与增量上传。 |
| `src/render/` | Mesh、Texture、材质、纯 CPU glTF prepare、RenderQueue、PipelineCache、Renderer 和 Shader variant。 |
| `src/render/pass/` | RenderPipeline 中的具体 pass；当前只有 `MainForwardPass`。 |
| `src/scene/` | Scene、SceneObject、SceneLight、Camera、prepared data、加载任务、GPU builder、SceneFactory 和内建场景。 |
| `src/window/` | GLFW 窗口和输入状态。 |
| `src/platform/` | Win32 原生文件选择等平台适配。 |
| `src/diagnostics/` | 场景加载耗时、资源上传量和 VMA 快照数据结构。 |

## 启动与所有权

`main()` 解析 `--help`、`--project` 与 `--runtime-control`，通过 `ProjectContextResolver` 找到源码项目和 `assets/catalog.json`。Catalog schema、稳定 ID、profile、路径和必需源文件会在创建 Window/Vulkan 前验证。随后 `SceneRegistryBuilder` 把 Catalog 条目适配为现有 `SceneEntry`，`main.cpp` 不再逐个登记 glTF 场景。

初始化顺序为：

1. ProjectContext、SceneCatalog 和 SceneRegistry，此时尚未创建窗口。
2. Window 和 InputManager。
3. VulkanContext、Device 和 DescriptorAllocator。
4. SwapChain 和 FrameSync。
5. Renderer、全局 UBO、RenderPipeline 和 MainForwardPass。
6. PipelineCache、SceneLoadManager worker 和初始 Scene。
7. GuiSystem。
8. 可选的 Runtime Control 命令队列和 Named Pipe 线程。

这些对象由 `Application` 持有，并通过成员声明与显式停止逻辑按相反方向销毁。Vulkan 资源的创建、使用、场景替换和销毁都发生在主线程。

## 线程模型

主线程拥有 GLFW、ImGui、DescriptorAllocator 和全部 Vulkan/VMA 对象。它每帧轮询 upload fence、按预算记录上传命令，并在资源全部可用后发布 Scene。

SceneLoadManager 持有一个长期 worker。worker 只执行 glTF 文件读取、解析、图片解码/缩放、顶点转换、tangent、bounds 和 hierarchy，输出不包含 Vulkan handle 的 `PreparedSceneData`。它通过 atomic 进度/取消标记和受 mutex 保护的结果与主线程通信。

Scenes 面板的文件对话框在主线程打开；选定文件后的依赖 preflight 和 `SceneImportService` 事务由独立 `std::async` worker 执行。该 worker 可以读取/复制源文件并原子更新源码项目 Catalog，但不能访问 GLFW、ImGui 或 Vulkan。Application 只轮询 future 和进度；退出时先请求取消并等待导入 worker 收束。

Named Pipe 线程只读取带长度前缀的 JSON 请求，把 `RuntimeCommand` 放入队列并等待主线程填写响应。它不能读取 Scene、Shader、统计数据，也不能调用 Vulkan 或 GLFW。`scene.load` 快速返回 taskId，等待行为由 VulkanLabCtl 客户端轮询 `load.status` 实现。

## 每帧数据流

1. 轮询窗口和输入，执行一条待处理控制命令。
2. 轮询 worker 结果与 upload fence，在软预算内推进 SceneGpuBuilder。
3. 应用待切换场景，更新计时、输入模式、相机和 Scene tick。
4. 轮询场景导入 future，构建 Scenes、Loading 和最近一次 LoadStats 等 ImGui 界面。
5. `FrameSync::beginFrame()` 获取 frame index、swapchain image 和 command buffer。
6. Application 把相机、环境光和 SceneLight 写入当前帧 GlobalUBO。
7. Scene 生成 RenderCommand，RenderQueue 分别排序 opaque 与 transparent 命令。
8. Renderer 组装 RenderFrameContext，RenderPipeline 执行 MainForwardPass，并在同一 render pass 末尾绘制 ImGui。
9. `FrameSync::endFrame()` 提交和 present；需要时重建 SwapChain 相关资源。

详细渲染行为见 [渲染流程](rendering.md)，场景创建与上传见 [资源加载](resource_loading.md)。
