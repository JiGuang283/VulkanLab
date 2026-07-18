# 系统架构概览

> Status: Current
> Last verified: 2026-07-18
> Verified against: `0516951`

VulkanLab 是一个 Windows Vulkan Forward Renderer。当前架构以 `Application` 为组合根，场景、渲染提交、GPU 资源和调试控制之间保持显式所有权，不使用全局引擎服务定位器。

## 模块边界

| 目录 | 职责 |
|---|---|
| `src/app/` | 应用生命周期、场景注册与切换、相机、ImGui 面板、全局 UBO 和命令执行。 |
| `src/control/` | Windows Named Pipe 服务、运行时命令队列和 JSON 协议。 |
| `src/core/` | Vulkan instance/device、SwapChain、FrameSync、Buffer/Image、Descriptor、Pipeline、VMA 和 UploadContext。 |
| `src/render/` | Mesh、Texture、材质、glTF 加载、RenderQueue、PipelineCache、Renderer 和 Shader variant。 |
| `src/render/pass/` | RenderPipeline 中的具体 pass；当前只有 `MainForwardPass`。 |
| `src/scene/` | Scene、SceneObject、SceneLight、Camera、SceneFactory 和内建场景。 |
| `src/window/` | GLFW 窗口和输入状态。 |
| `src/diagnostics/` | 场景加载耗时、资源上传量和 VMA 快照数据结构。 |

## 启动与所有权

`main()` 解析 `--help` 与 `--runtime-control`，注册固定场景和存在于磁盘的可选 glTF 场景，然后调用 `Application::run()`。

初始化顺序为：

1. Window 和 InputManager。
2. VulkanContext、Device 和 DescriptorAllocator。
3. SwapChain 和 FrameSync。
4. Renderer、全局 UBO、RenderPipeline 和 MainForwardPass。
5. PipelineCache 和初始 Scene。
6. GuiSystem。
7. 可选的 Runtime Control 命令队列和 Named Pipe 线程。

这些对象由 `Application` 持有，并通过成员声明与显式停止逻辑按相反方向销毁。Vulkan 资源的创建、使用、场景替换和销毁都发生在主线程。

## 主线程与控制线程

主线程拥有 GLFW、ImGui 和全部 Vulkan 对象。Named Pipe 线程只负责读取一条带长度前缀的 JSON 请求，把 `RuntimeCommand` 放入受 mutex 保护的队列，并等待主线程填写响应。它不能读取 Scene、Shader、统计数据，也不能调用 Vulkan 或 GLFW。

主循环在处理窗口事件后最多执行一条 runtime command。场景加载命令会一直占用主线程，完成后才返回响应，因此当前控制接口不是异步任务系统。

## 每帧数据流

1. 轮询窗口和输入，执行一条待处理控制命令。
2. 应用待切换场景，更新计时、输入模式、相机和 Scene tick。
3. 构建 ImGui 界面。
4. `FrameSync::beginFrame()` 获取 frame index、swapchain image 和 command buffer。
5. Application 把相机、环境光和 SceneLight 写入当前帧 GlobalUBO。
6. Scene 生成 RenderCommand，RenderQueue 分别排序 opaque 与 transparent 命令。
7. Renderer 组装 RenderFrameContext，RenderPipeline 执行 MainForwardPass，并在同一 render pass 末尾绘制 ImGui。
8. `FrameSync::endFrame()` 提交和 present；需要时重建 SwapChain 相关资源。

详细渲染行为见 [渲染流程](rendering.md)，场景创建与上传见 [资源加载](resource_loading.md)。
