# RenderGraph 架构

> Status: Current
> Last verified: 2026-08-15
> Verified against: `62f6cc4`

## 目标与边界

RenderGraph 是 VulkanLab 帧内渲染工作、逻辑资源版本和同步的唯一权威。它替代了旧的固定 `RenderPipeline` 顺序执行器、Pass-owned `VkRenderPass`/`VkFramebuffer` 和各 Pass 手工 image layout barrier。

当前版本刻意保持以下边界：

- 所有节点仍录制到一个 primary command buffer。
- Graphics、Compute 和 Transfer queue class 当前都映射到 Graphics Queue。
- 不做 transient resource aliasing、pass merging、并行录制或自动 queue ownership transfer。
- AssetRepository、ModelGpuBuilder、EnvironmentGpuBuilder 和 IncrementalUploadQueue 不属于 Frame Graph。
- RayTracingScene 的 AS build 作为 External side-effect 节点接入；AS 构建实现内部保留 Vulkan 要求的专用 barrier。
- CACAO SDK 作为 External compute 节点接入；SDK 私有资源的内部同步不由 Graph 推导。

## Build、Compile 与 Execute

每帧先由各 Pass 的 `prepareFrame()` 固化动态资源和状态，再执行：

```text
FrameRenderFeatures
  -> RenderGraphBuilder
  -> RenderGraphCompiler
  -> CompiledRenderGraph cache
  -> RenderGraphExecutor
```

`setup()` 可以注册一个或多个 node。node 声明 pass type、queue class、条件、side effect、image/buffer read/write 和 attachment 信息。Hi-Z、Color Pyramid、Bloom、Atmosphere、AO、SSR、SSGI、TAA、CSM cascade、Point face 与 Spot light 都拆为独立 node；未请求的功能在 compile 阶段裁剪，不在 `recordNode()` 中依赖 early return 隐藏拓扑。

Compiler 负责：

- 校验非法 handle、cycle 和恰好一个 Present writer。
- 校验当前帧 read-before-write、未初始化 attachment Load、extent/sample/usage 契约和 history 访问。
- 从 RAW、WAR、WAW 和显式依赖生成边。
- 以注册顺序作为无依赖节点的稳定排序条件。
- 生成可缓存的 topology hash。

`RenderGraphTopologyKey` 包含 feature bits、Pass 动态拓扑签名、resource format/sample/mip/layer 等 attachment contract。普通曝光、bias、强度等数值不触发重编译。会替换 buffer 的 Occlusion 和 DDGI 把实际 handle/capacity 纳入 `topologySignature()`；viewport resize 会清空拓扑和物理状态缓存。

## 资源与同步

Graph image handle 带逻辑 version，image use 可以精确到 mip、array layer 和 aspect。lifetime 分为：

- `Transient`：只在对应 feature topology 活跃及安全 retirement 期间驻留；v1 不做 alias。
- `PerFrame`：按 frame slot 分配。
- `History`：支持 Current/Previous 访问。
- `Persistent`：跨帧共享，例如 Shadow 和 DDGI atlas。
- `Imported`：外部初始化或 Swapchain 资源。

`RenderResourcePool` 是物理 image/view/sampler 池。Graph executor 按实际 `VkImage + frame + mip + layer + aspect` 持久跟踪：

```text
stageMask2 + accessMask2 + layout + initialized
```

buffer 以实际 `VkBuffer` 跟踪 stage/access/initialized。Executor 只在 hazard 或 layout 改变时生成 `VkImageMemoryBarrier2`/`VkBufferMemoryBarrier2`，通过 `vkCmdPipelineBarrier2()` 提交。相同 layout 的只读到只读不产生 barrier。

典型自动依赖包括：

- Surface Depth write -> Hi-Z sampled read。
- Hi-Z storage write -> Occlusion sampled read。
- Occlusion SSBO write -> indirect command read。
- Shadow attachment write -> MainForward sampled read。
- HDR attachment/storage write -> ToneMap sampled read。
- Viewport/Workspace/HDR -> Screenshot transfer read -> 原 layout。
- Swapchain `PRESENT -> COLOR_ATTACHMENT -> PRESENT`。

`CompiledRenderGraph` 输出 active physical image handle 和 active owner Pass。每帧在 Graph 执行前调用：

```text
compile/reuse topology
  -> synchronize active resources at the current frame-slot safe point
  -> refresh descriptors for newly created resources
  -> execute nodes
```

物理状态为：

- `Unallocated`：没有 Vulkan image，不计入 resident bytes。
- `Resident`：当前 topology 使用，或者设置了 persistent residency。
- `Retiring`：已经失活，但最后一次引用它的 submission 尚未完成。

新激活资源从 `Unallocated` 创建为 `Resident`。资源失活时记录
`lastSubmittedSerial` 并进入 `Retiring`；只有 `completedSerial` 到达后才销毁。
Retiring 期间重新激活会取消回收并复用原图像。资源创建后，Graph 只通知声明
使用它的 owner Pass 执行 `onResourceResidencyChanged()`，避免更新其他 frame slot
仍在使用的 descriptor set。

诊断语义固定为：

- `logical bytes`：所有已声明资源的理论总量。
- `active bytes`：当前 compiled topology 引用的资源量。
- `resident bytes`：实际持有 Vulkan image 的资源量。
- `retiring bytes`：resident 中正在等待 submission serial 的部分。

当前仍有一个明确的启动限制：`RenderResourcePool::realize()` 在 Renderer 构造期会
短暂创建全部注册图像，以满足部分 Pass 的初始 descriptor 构造；首帧
`prepareResources()` 会把 inactive 图像退役并在 serial 安全后释放。因此稳态是
feature-aware residency，但首次启动峰值还不是完全 lazy allocation。`Transient`
lifetime 也不表示内存 alias。

## Dynamic Rendering

Graphics node 用 attachment declaration 构造 `VkRenderingAttachmentInfo` 和 `VkRenderingInfo`。Executor 包围 node 调用 `vkCmdBeginRendering()`/`vkCmdEndRendering()`；Pass 只录制 pipeline、descriptor、push constant 和 draw。

Pipeline Cache 使用 `PipelineRenderingSignature`：

- color formats 与 attachment 数量。
- depth/stencil format。
- sample count。
- view mask。
- blend attachment 数量。

Pipeline key 不包含 `VkRenderPass` 或 subpass。ImGui Vulkan backend 同样启用 Dynamic Rendering。

## External 与 Side Effect

External node 用于 Graph 无法完全推导内部资源、但仍需参与帧顺序的工作。side-effect node会与相邻活动节点建立顺序边，防止编译器把不可见副作用移出预期位置。

当前 External 节点包括：

- RayTracingScene/TLAS build。
- CACAO SDK dispatch。
- Occlusion indirect-ready sequence boundary。

新增 External 节点时必须尽量声明其公开 image/buffer 输入输出；只有 SDK 私有资源或 Vulkan 子系统内部资源可以留在节点内部同步。

## 诊断

`VulkanLab -> Diagnostics -> Render Graph` 显示：

- active/culled node 和执行顺序。
- dependency edge 和自动 barrier 数量。
- layout/hazard barrier 分类。
- image logical/active/resident/retiring bytes。
- image/buffer version、lifetime、producer 和 consumer。

`render.status.renderGraph` 返回同一数据。UI 可导出：

```text
logs/render_graph.json
logs/render_graph.dot
```

RenderDoc label、Tracy zone 和 GPU timestamp 由 Graph node/group 自动组织；Profiler 使用稳定 Pass ID，不依赖当前拓扑中的 vector index。

## 后续扩展

RenderGraph v1 已提供稳定的帧内依赖和 attachment contract。Bindless Material 已建立在该基础上；GPU-driven 或 Deferred Renderer 仍应复用同一 Graph。后续扩展优先级是：

1. 消除构造期全资源 bootstrap，实现首帧前完全 lazy realization。
2. 基于已验证 lifetime 的 transient resource aliasing。
3. secondary command buffer 并行录制。
4. 独立 Compute/Transfer queue 调度和 timeline semaphore。
5. pass merging 与 async compute overlap。

这些扩展应建立在 profiler 数据上，不改变当前 Pass 的 Shader ABI 或场景资产所有权。
