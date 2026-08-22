# Vulkan 运行时性能优化执行计划

> Status: Active
> Last verified: 2026-08-22
> Verified against: `efb5a94`

## Review Summary

本计划已按当前代码重新核对并重写。Review 覆盖了帧构建、RenderGraph、资源池、Forward/Deferred Surface 数据流、三类阴影、Descriptor、Pipeline、屏幕空间金字塔、GPU-driven 前置条件、资产上传和 FrameSync。

主要修正如下：

- RenderGraph 已按 topology key 缓存编译结果，资源池也已有 Resident/Retiring 和 submission serial 退役机制。因此不再把“新增编译缓存”和“新增资源退役”列为任务，改为优化每帧 graph setup、执行期临时分配、同步粒度和首次资源实例化。
- 当前 `Pipeline.cpp` 对所有 graphics pipeline 无条件开启 sample shading，并且渲染目标默认采用设备可用的最高 MSAA。这是原计划遗漏的直接 GPU 成本，已提升到前置优化。
- Descriptor 的问题不是缺少集中式 allocator，而是多个 Pass 在稳定帧重复调用 `vkUpdateDescriptorSets()`。方案改为由各 Pass 保持 descriptor 所有权，并共享 generation-aware binding state，避免再引入中央 descriptor 管理器。
- Deferred 下的重复几何来自 `OpaqueRenderProducts` 仍把屏幕空间数据固定映射到 SurfacePrepass，而 GBuffer 已经生成同类数据。方案改为扩展现有 render products contract，由 Forward/Deferred 各自提供 canonical surface，不新增一套平行抽象。
- 当前深度层级已经支持 combined min/max；Bloom 也不是普通 mip reduction。删除“一套通用 single-pass pyramid 覆盖所有功能”的过度设计，改为先量化 dispatch/barrier 成本，再决定 graph-native batching 或单独引入 SPD 类实现。
- `ShadowSystem` 当前负责灯光选择、稳定 slot 和矩阵规划，但它看不到最终 caster queue、物理 image generation 和提交结果。阴影缓存改为 Renderer 侧的 `ShadowRenderCache`，并通过提交事务提交缓存状态，避免把资源生命周期职责塞进 `ShadowSystem`。
- 资产上传已经使用双槽、持久映射 staging 和 fence polling。后续重点改为复用跨 Model builder 的 staging page，并将 transfer copy 与 BLAS build 分开评估，而不是重复实现批量上传。
- GPU-driven 被拆成 DrawData、Geometry Arena、capability、GPU compaction 和 Shadow stream 五个有依赖关系的子阶段。当前逐 Mesh 独立 Buffer、per-draw model push constant 和 Ray Query BLAS 都是必须先处理的前置条件。
- 阶段顺序调整为：Release 基线和计数器 -> 稳定帧 driver/CPU 开销 -> Surface 数据流 -> Shadow 缓存 -> 显存与带宽 -> 金字塔调度 -> GPU-driven -> 条件式多 Queue/并行。

## Current Architecture And Evidence

### 当前帧数据流

当前主路径已经具备清晰的单队列 Frame Graph：

```text
Application
  -> RuntimeWorld update and editor changes
  -> RenderWorldFrameSnapshot
  -> ShadowSystem::build()
  -> buildRenderView()
  -> VisibilitySystem::build()
  -> Renderer::renderFrame()
       -> resolveFrameFeatures()
       -> RenderGraph::prepareGraph()
       -> RenderGraph::prepareResources()
       -> descriptor preparation
       -> RenderGraph::execute()
  -> FrameSync::endFrame()
  -> VisibilitySystem::commit()
```

帧内使用一个 graphics primary command buffer、一次 graphics submit 和一次 present。`RenderGraph` 负责活动节点、依赖、逻辑资源和 synchronization2 transition；`RenderResourcePool` 负责物理 image/sampler 的驻留和 serial-based retirement；`AssetRepository` 与 `IncrementalUploadQueue` 负责场景资产的异步准备和增量 GPU 上传。

本计划保留这些边界。帧内资源属于 RenderGraph/ResourcePool，场景资产属于 AssetRepository，灯光投影规划属于 ShadowSystem，实际 shadow image 缓存属于 Renderer。

### 已有 Debug 观察值

以下结果来自 1280x720、无 GUI、Validation Off、Bindless、`windows-msvc-dev-fast` Debug 构建。它们只能用于确认结构性热点，不能作为后续验收基线：

| 场景和配置 | CPU 估算 | GPU 中位数 | 观察到的热点 |
|---|---:|---:|---|
| Algorithm Playground 默认 | 4.36 ms | 0.99 ms | 43 active passes，158 automatic barriers，明显 CPU-bound |
| Main Sponza 默认 | 6.72 ms | 4.56 ms | Directional Shadow 2.52 ms，GBuffer 1.13 ms，SurfacePrepass 0.68 ms |
| Main Sponza 最小配置 | 3.65 ms | 1.25 ms | 6 passes，GBuffer 1.14 ms，CPU submission 仍占明显比例 |

### 经代码确认的问题

| 区域 | 当前事实 | 影响 |
|---|---|---|
| RenderGraph | topology 已缓存，但每帧仍执行各 Pass setup；executor 创建临时 vector，并按单个 image/buffer transition 调用 `vkCmdPipelineBarrier2()` | CPU 和 driver call 开销，barrier stage 偏宽 |
| Descriptor | Atmosphere、ScreenSpace、DeferredLighting、HDR Composite、ToneMap、TAA 及多个 temporal effect 在稳定帧重写未变化 binding | CPU/driver 开销，难以定位无效更新 |
| Pipeline | graphics/compute 创建使用 `VK_NULL_HANDLE` native cache；每次 miss 重读 SPIR-V 并创建 shader module | 启动和功能切换 hitch，不是稳定帧主因 |
| Raster policy | 所有 graphics pipeline 无条件 `sampleShadingEnable = VK_TRUE`；MSAA 自动取设备支持最大值 | 高分辨率/复杂场景 GPU 成本不可控 |
| Deferred Surface | GBuffer 与 SurfacePrepass 可能同时生成 depth、normal、motion、albedo 类数据 | opaque geometry 重复 raster 和 draw recording |
| Occlusion | 当前遮挡剔除依赖本帧 SurfacePrepass 的 depth hierarchy | Deferred 很难在剔除前取消重复 prepass |
| Shadows | CSM/Point/Spot 在活动时完整重绘；`ShadowSystem::contentRevision` 不包含最终 caster queue、material alpha 和物理 image generation | 静态场景浪费 GPU；现有 revision 不能安全作为缓存键 |
| ResourcePool | `Renderer` 初始化时调用 `realize()` 创建全部已注册 image，随后才按活动 feature 退役 | 首次显存峰值和无用创建；已有驻留机制未充分利用 |
| Pyramids | depth hierarchy 已支持 combined min/max；color 和 Bloom 使用不同算法与资源形态 | 不适合强行合并为一套通用 single-dispatch 实现 |
| Draw submission | Occlusion compute 只写每项 `instanceCount=0/1`；Forward/GBuffer 仍逐项 bind/push/single indirect draw | CPU draw submission 未真正 GPU-driven |
| Geometry | 每个 `Mesh` 独立拥有 vertex/index buffer，并可直接基于它们创建 BLAS | Geometry Arena 会影响 upload、device address 和 Ray Query 生命周期 |
| Upload | `IncrementalUploadQueue` 已是双槽持久映射 staging；但每个 Model builder 独立创建 queue/staging allocation，BLAS build 与 copy 同 batch | 大模型切换可能有 staging churn；不能直接整体迁移到 transfer queue |

## Goals

- 建立可重复的 Release 性能基线，并能从计数器区分 CPU 构建、driver call、GPU raster/compute、显存和加载开销。
- 稳定帧只在资源身份或 descriptor 内容变化时更新 descriptor。
- 将 RenderGraph 每节点 transition 合并为少量 synchronization2 调用，同时保持精确、可验证的 hazard 和 layout 语义。
- Deferred 默认只进行一次完整 opaque surface raster；额外 depth-only prepass 必须由测量策略启用。
- 静态 shadow view 和静态 caster 不重复生成 Shadow Map，且缓存状态与实际提交和物理资源 generation 一致。
- 将 MSAA、sample shading、shadow format 和 GBuffer 带宽从“设备质量上限”改为显式质量策略。
- 为 GPU-driven 建立正确的 DrawData、Geometry ownership、capability 和 indirect compaction 路径，而不是继续叠加单 draw 特殊分支。
- 所有优化都有前后数据、回退路径和明确完成条件。

## Non-Goals

- 不新增渲染算法、材质节点图、场景格式或另一套 Renderer。
- 不引入 RHI，也不替换当前 RenderGraph。
- 不把 async compute、专用 transfer queue、secondary command buffer 并行录制当作默认答案；只有测量满足门槛后才实施。
- 不在本计划中实现 mesh shader、vertex pulling、virtual texturing 或资源 streaming。
- 不通过放宽同步、跳过 layout transition 或复用未完成资源来换取性能。
- 不要求所有阶段一次性实施；每一阶段应可独立提交、测量和停止。

## Engineering Principles

1. 先建立 Release 证据，再决定优化是否进入主线。
2. 先消除重复工作，再降低单次调用成本，最后才增加并行。
3. 复用现有抽象并补足契约，不创建职责重叠的新系统。
4. Graph 编译期只预计算与 topology 和逻辑 usage 有关的信息；跨帧物理资源旧状态仍由执行期解析。
5. Descriptor 由使用它的 Pass 管理，公共机制只负责 binding identity、generation 和 dirty 判断。
6. 缓存必须绑定稳定身份、内容 revision、物理 generation 和成功提交；不能仅比较矩阵或 raw pointer。
7. 质量策略必须显式，并分别报告 requested/effective 值。
8. 每个高风险阶段都保留 CPU 或旧路径，确认收益和正确性后再删除。

## Priority And Dependencies

```text
Stage 0  Release baseline and counters
   |
Stage 1  Stable-frame Vulkan/CPU overhead and raster policy
   |
Stage 2  Canonical surface products and deferred de-duplication
   |
Stage 3  Submission-aware shadow rendering cache
   |
Stage 4  Resource residency, MSAA and bandwidth tiers
   |
Stage 5  Pyramid scheduling, only when measurements justify it
   |
Stage 6  GPU-driven opaque submission
   |
Stage 7  Upload service and conditional parallel execution
```

- P0：Stage 0、1、2。它们解决测量、稳定帧 CPU 和重复几何，是后续工作的共同前置。
- P1：Stage 3、4。它们针对 Main Sponza 的主要 GPU/显存成本。
- P2：Stage 5、6。Stage 5 有数据门槛；Stage 6 是架构性优化。
- P3：Stage 7。只有 Tracy/queue 数据证明收益时实施对应子项。

## Stage 0: Release Baseline And Performance Contract

### 阶段目标

把当前 Debug 观察值升级为可重复的 Release 基线，并补齐能够验证后续每项优化的计数器。此阶段不改变渲染行为。

### 当前问题与根因

- `tools/performance/Measure-Renderer.ps1` 已能通过 Runtime Control 测量多种算法配置，但主要输出 frame/GPU 时间和 Graph 汇总。
- 当前缺少 descriptor update、实际 barrier API call、draw/dispatch、pipeline miss/create time 等计数，无法判断 CPU 时间下降来自哪里。
- 现有 Tracy preset 是 Debug，不应与正式 Release 基线混用。
- Forward/Deferred、MSAA 和场景矩阵没有统一纳入采样组合。

### 推荐方案

#### 0.1 构建配置

新增两个用途明确的 preset：

- `windows-msvc-perf`
  - Release。
  - Runtime Control 和轻量 GPU profiler 开启。
  - Validation、Tracy、Capture 和 Editor 默认关闭，除非脚本明确需要。
  - 作为所有数值对比的 canonical executable。
- `windows-msvc-perf-tracy`
  - RelWithDebInfo 或 Release with symbols。
  - Tracy 开启，其他设置尽量与 perf preset 一致。
  - 只用于定位 CPU zone，不与 canonical 数值直接比较。

保留 `windows-msvc-dev-fast` 作为开发构建，不再用它设定性能验收阈值。

#### 0.2 扩展现有性能脚本

扩展 `tools/performance/Measure-Renderer.ps1`，不要另建重复工具：

- 场景至少包括 Algorithm Playground、Main Sponza、GI Calibration Lab。
- Render path 包括 Forward、Deferred。
- 配置包括 Minimal、Default、Shadow、SSAO/GTAO、SSR、SSGI、TAA、DDGI 和组合路径。
- 固定 viewport、相机、warm-up 帧数、采样帧数和 GPU idle 前后策略。
- 输出 JSON 原始结果和 Markdown 汇总。
- 记录 median、p95、min/max，而不是只记录平均 FPS。
- 对 topology key、active/culled pass、资源 active/resident bytes 进行采样。

#### 0.3 增加低成本计数器

计数器由现有 Diagnostics/Runtime Control 暴露，不依赖 Tracy：

- RenderGraph：compile hit/miss、prepareGraph/compile/execute CPU 时间、active nodes、dependency edges。
- Barrier：`vkCmdPipelineBarrier2` 调用次数、image barrier 数、buffer barrier 数、layout-only/hazard 原因。
- Descriptor：`vkUpdateDescriptorSets` 调用次数、write 数、descriptor set bind 数、因 generation/mode 变化触发的更新数。
- Pipeline：lookup/hit/miss、graphics/compute create 次数和耗时、native cache hit 可用时的统计。
- Commands：direct draw、single indirect draw、multi indirect draw、dispatch、copy、rendering scope 数量。
- Resources：active/resident/retiring bytes，按 Shadow/Surface/ScreenSpace/PostProcess/Atmosphere 分组。
- Upload：staging allocated/high-water、batch count、main graphics queue upload submit、builder pump CPU 时间。

统计包装应集中在 Vulkan call wrapper 或 owning subsystem，不能要求每个 Pass 手工维护不一致的定义。

### 涉及模块

- `tools/performance/Measure-Renderer.ps1`
- `CMakePresets.json`
- `src/render/Renderer.cpp`
- `src/render/graph/RenderGraph.cpp`
- `src/render/graph/RenderResourcePool.cpp`
- `src/render/pipeline/PipelineCache.cpp`
- `src/core/DescriptorAllocator.cpp`
- `src/core/FrameSync.cpp`
- Diagnostics 与 Runtime Control status serialization

### 风险与兼容性

- 计数器本身不能在 Release 稳定帧造成可见回退。时间采样采用已有 profiler 或低频聚合，避免每次 Vulkan call 写日志。
- Tracy-on 数据只用于归因，不能与 Tracy-off 基线混合。
- Runtime Control JSON 保持向后兼容，只增加字段。

### 完成条件

- 同一命令可重复产生 Forward/Deferred 场景矩阵和 Markdown 报告。
- 连续三次测量的稳定 GPU median 和 CPU p95 波动范围被记录。
- 后续阶段涉及的 descriptor、barrier、draw、pipeline、memory 和 upload 指标都可查询。
- 未改变画面、Graph topology 或默认 RenderSettings。

## Stage 1: Stable-Frame Vulkan And CPU Overhead

### 阶段目标

消除稳定帧中可确认的无效 driver 调用、临时分配和过宽同步，同时把 raster sample 策略从隐式设备上限改成显式状态。此阶段不改变 Surface 数据来源。

### 1.1 显式 MSAA 与 Sample Shading 策略

#### 当前问题与根因

- `Pipeline.cpp` 无条件设置 `sampleShadingEnable = VK_TRUE` 和 `minSampleShading = 0.2f`。
- `RenderResourcePool.cpp` 中的 `chooseSamples()` 使用 format/depth 支持范围内不高于设备最大值的最高 sample count。
- Device 把 `sampleRateShading` 当作硬启用功能，导致所有 MSAA graphics pipeline 默认承担 sample shading 成本。

#### 推荐方案

- `PipelineConfig` 和 `PipelineKey` 增加：
  - `rasterSamples`
  - `sampleShadingEnabled`
  - `minSampleShading`
- 默认关闭 sample shading；只有明确的质量模式才开启。
- Device 将 `sampleRateShading` 改为可选 capability：支持时可启用 device feature，但不再影响设备适配资格；不支持时 UI 中禁用该选项。
- Render settings 增加 `MsaaMode { Off, 2x, 4x, DeviceMax }` 和可选 sample shading advanced setting，分别报告 requested/effective。
- 第一个提交保持当前 sample count 默认语义，只关闭无条件 sample shading，以便隔离收益；Stage 4 再根据基线调整产品默认值。
- 删除 Dynamic Rendering 后已无意义的 `subpass` pipeline key/config 字段。

#### 为什么这样做

这是代码中明确存在的成本，不需要先引入新算法。把 sample count 与 sample shading 纳入完整 pipeline state，也避免后续 Forward/Deferred 质量档依赖隐式 Device 行为。

### 1.2 Descriptor Binding State

#### 当前问题与根因

多个 Pass 已拥有 per-frame descriptor set，但在每帧 record/prepare 中重新写入稳定 image binding。问题不是 descriptor allocator，而是缺少统一的内容身份和 dirty 判断。

#### 推荐方案

- 新增轻量 `DescriptorSetBindingState`，由各 Pass 内嵌使用：
  - 保存每个 binding 的 image view、sampler、layout、buffer、offset、range 和资源 physical generation。
  - `updateIfChanged()` 只为变化的 binding 生成 `VkWriteDescriptorSet`。
  - 返回此次 update 的原因用于统计。
- `RenderResourcePool` 为每个物理 image instance 暴露单调递增 generation；create/recreate 后递增，普通 frame slot 切换不递增。
- current/previous history 资源为每个 frame slot 预建稳定映射；history valid、camera cut 等数值改放 UBO/push constant，避免通过切换 fallback image 每帧改 descriptor。
- Buffer 只在实际 handle/range 变化时更新；mapped 内容变化不触发 descriptor write。
- 保持 Pass ownership：Atmosphere、ScreenSpace、DeferredLighting、HDR Composite、ToneMap、TAA、SSR、SSGI、GTAO 等逐个接入公共 helper。
- 不把所有 descriptor set 移到 Renderer，也不改变 Bindless Material 的独立生命周期。

#### 完成条件

- Algorithm Playground 和 Main Sponza 稳定帧 `vkUpdateDescriptorSets` 调用数接近零。
- resize、资源重新驻留、history swap、environment reload 和 feature topology 切换仍会正确更新。
- 不再出现因 descriptor set 未分配或资源重建后保留旧 view 产生的 validation error。

### 1.3 RenderGraph 执行计划和 Barrier 批处理

#### 当前问题与根因

`RenderGraph` 已缓存 compiled topology，因此不需要再新增 compile cache。真正的执行期开销包括：

- 每帧调用所有 Pass 的 graph setup 以生成 topology/signature。
- executor 创建 remaining/profiler/attachment 临时 vector。
- 每个 image 或 buffer transition 单独调用一次 `vkCmdPipelineBarrier2()`。
- usage 到 stage 的映射偏宽，例如 sampled/uniform 经常覆盖 all graphics + compute。
- Buffer 状态按整个 `VkBuffer` 跟踪，offset/range 只存在于 barrier 中。

#### 推荐方案

- 扩展 `CompiledRenderGraph`，预计算：
  - active node 的稳定执行序列和静态 attachment 元数据。
  - 每节点的逻辑 transition request 列表。
  - resource lookup index、subresource 范围和可合并组。
- 将当前“先让全部 Pass 填充 Builder、再得到 topology key”的流程拆开：
  - `FrameFeatureResolver` 提供全局 feature bits。
  - 每个 Pass 提供廉价、无副作用的 `topologySignature()`，只包含节点数量、resource variant 和 attachment contract。
  - signature 命中 compiled cache 时跳过完整 graph declaration/setup，只执行活动 Pass 的 `prepareFrame()`。
  - cache miss 时才调用 `setup()` 填充 Builder 并编译；Debug 构建可低频重新构建并比较 signature，发现遗漏的 topology 输入。
- 数值参数、相机矩阵、descriptor内容和资源物理handle不得进入topology signature；shadow dirty mask、pyramid mip数量、capture source等真正改变节点集合的值必须进入。
- executor 保留跨帧 scratch arrays，按最大节点/attachment 数扩容后复用，不在稳定帧重复分配。
- 每个节点执行前：
  1. 根据物理资源当前状态解析所有 transition request。
  2. 将 image/buffer barrier 收集到一个 `VkDependencyInfo`。
  3. 最多调用一次 pre-node `vkCmdPipelineBarrier2()`。
  4. 节点 final/imported transition 同样批量处理。
- `RenderGraphImageUse`/`RenderGraphBufferUse` 增加实际 shader stage intent，Pass setup 明确 Fragment/Compute/Vertex/Indirect，而不是由通用 usage 推导最大范围。
- 相同 layout 且相同或兼容只读 access 的连续使用不生成 barrier。
- Buffer range-aware tracking 分两步：
  - 首先保留 whole-buffer conservative state，但收紧 stage/access 并批量调用。
  - 只有计数器证明同一 buffer 的不重叠 range 造成大量假依赖时，再增加 interval state map；不能简单把 offset 加入 hash 后忽略重叠。
- 保留跨帧物理旧状态在 executor 中解析。编译期不能把旧 layout 固化，因为 Single shadow、history 和 imported swapchain 的实际状态跨帧变化。

#### 风险与兼容性

- Barrier 数量下降不等于正确性。必须同时报告 API call 数和 barrier struct 数。
- `managesDeclaredTransitionsInternally` 的 external path 暂时保留，迁移前不能由 Graph 重复插入依赖。
- stage intent 是契约变化，遗漏某个 consumer stage 可能引入真实 hazard，应逐 Pass 迁移。

### 1.4 Native Pipeline Cache And Shader Byte Cache

#### 当前问题与根因

当前 C++ `PipelineCache` 只缓存本进程中已创建对象。`vkCreateGraphicsPipelines`/compute 使用 `VK_NULL_HANDLE` native cache，pipeline miss 还会重复读取 SPIR-V 和创建 shader module。

#### 推荐方案

- Device 持有一个 `VkPipelineCache`，传给 graphics 和 compute pipeline 创建。
- 持久化文件 key 包含 vendor ID、device ID、driver version 和 `pipelineCacheUUID`；不匹配时丢弃。
- 启动读取，退出或显式 flush 时原子写回，不把驱动 cache 提交到 Git。
- ShaderRegistry 或 PipelineCache 增加 SPIR-V byte cache，避免同一程序重复磁盘读取。
- native cache 只作为功能切换和启动 hitch 优化，不把它计入稳定帧 FPS 收益。

### 1.5 条件式 CPU Draw Packet Cache

仅当 Stage 0 显示 pipeline config 构建/hash lookup 或 draw state 解析占显著 CPU 时实施：

- 为稳定 `RenderItemKey + pass family + pipeline signature` 缓存 immutable draw packet：mesh binding、material handle、pipeline variant bucket 和 index metadata。
- world transform、previous world、可见性和排序仍是每帧数据，不缓存到 packet。
- Scene/model generation、material family、front-face/double-sided/alpha 或 graph attachment signature 变化时使 packet 失效。
- 该缓存是 Stage 6 GPU-driven 的 CPU fallback，不提前改变 shader draw ABI。

### 涉及模块

- `src/render/pipeline/PipelineConfig.h`
- `src/render/pipeline/Pipeline.cpp`
- `src/render/pipeline/PipelineCache.cpp`
- `src/core/Device.cpp`
- `src/render/graph/RenderGraph.cpp`
- `src/render/graph/RenderResourcePool.cpp`
- `src/render/Renderer.cpp`
- 各 feature Pass 的 descriptor preparation

### 阶段完成条件

- sample shading 默认关闭且 PipelineKey 完整区分状态。
- 稳定帧 descriptor writes、barrier API calls 和 executor allocation 显著下降，并由 Stage 0 报告证明。
- topology 切换、resize、history invalidation 和资源重新驻留行为正确。
- native pipeline cache 降低第二次启动/功能切换的 pipeline create 时间，但不虚报为稳定帧收益。

## Stage 2: Canonical Surface Products And Deferred De-duplication

### 阶段目标

让 Forward 和 Deferred 各自产生唯一、权威的 opaque surface 数据，解除 Deferred 对完整 SurfacePrepass 的结构性依赖，并为上一帧遮挡剔除建立正确 history。

### 当前问题与根因

- `FrameFeatureResolver` 可以同时令 `gBufferRequired` 和 Surface depth/normal/motion/albedo required 为 true。
- `Renderer::opaqueRenderProducts()` 在 Deferred 下仍把屏幕空间输入映射到 SurfacePrepass attachments。
- `GBufferPass` 已写入 base color/metallic、normal/roughness/occlusion、emissive/flags、motion 和 depth，但这些资源没有被声明为 canonical screen-space surface。
- OcclusionCull 依赖当前帧 Surface depth hierarchy，使 Deferred 在开始 GBuffer 前必须 raster 一次几何。
- GBuffer 和 Surface 的 normal/aux encoding 不完全相同，不能仅替换 handle 而不传递 decode contract。

### 推荐架构

扩展现有 `ScreenSpaceSurfaceProducts` 和 `OpaqueRenderProducts`，不再创建平行的 Surface Registry：

```cpp
enum class OpaqueSurfaceProducer {
    None,
    ForwardSurfacePrepass,
    DeferredGBuffer
};

struct ScreenSpaceSurfaceProducts {
    RenderImageHandle depth;
    RenderImageHandle normalRoughness;
    RenderImageHandle motion;
    RenderImageHandle albedoMetallic;
    RenderImageHandle emissiveFlags;
    SurfaceEncoding encoding;
    bool historyAvailable;
};

struct OpaqueRenderProducts {
    OpaqueSurfaceProducer producer;
    RenderImageHandle hdrColor;
    RenderImageHandle baselineDiffuse;
    RenderImageHandle baselineSpecular;
    RenderImageHandle geometryDepth;
    ScreenSpaceSurfaceProducts screenSpace;
};
```

`SurfaceEncoding` 明确 normal 编码、channel 语义、depth convention 和 motion convention。消费者继续通过现有 `OpaqueRenderProducts::screenSpace` 访问产品语义，而不是依赖具体 Pass 名称。

### 实施步骤

#### 2.1 统一产品契约

- `FrameFeatureResolver` 先解析消费者需要的 product bits，再由 render path 选择 producer。
- Forward：只有 SSAO/SSR/SSGI/TAA/visibility 等确实需要时才启用 SurfacePrepass。
- Deferred：GBuffer 是 canonical opaque surface producer；不再额外请求等价 Surface attachments。
- 所有 screen-space Pass 通过 `OpaqueRenderProducts::screenSpace` 获取资源和 encoding。
- Shader/compute helper 根据 encoding 解码；不能在每个 Pass 复制 format-specific 分支。

#### 2.2 Provider-aware Descriptor

- SSAO/GTAO、SSR、SSGI、TAA、DepthHierarchy、debug view 等改为绑定产品 handle。
- descriptor binding state 将 producer/encoding 和物理 generation 纳入 key。
- Debug View 明确显示当前 producer，避免调试时误以为仍在读取 SurfacePrepass。

#### 2.3 Previous-frame Occlusion History

要取消 Deferred 当前帧 prepass，遮挡剔除改为读取上一已完成帧的 canonical depth hierarchy：

- 将 canonical surface depth 或其 hierarchy 标记为 history-capable，按 frame slot 保存 current/previous。
- camera cut、viewport resize、scene generation、projection mode 变化时，将所有 candidate 保守设为 visible。
- 新出现、刚移动或 bounds revision 变化的对象保守可见至少一帧。
- 对动态对象使用 velocity/bounds expansion，或在 v1 直接跳过 history occlusion，避免旧深度误剔除。
- 只有 completed frame history 才能作为 occluder；不得采样正在写入的 frame slot。
- 当前 Visibility Hi-Z 的 max-depth occlusion 语义保持独立，不与 screen min-depth debug pyramid混淆。

#### 2.4 删除重复 Surface Raster

- Deferred 正常路径只执行 GBuffer 全量 surface raster。
- SurfacePrepass 在 Deferred 下只允许作为显式 depth-only policy 出现，不再写重复 normal/motion/albedo。
- 是否启用 depth-only prepass由场景 overdraw/GBuffer early-z 的 Release 测量决定，默认关闭。
- GBuffer 与 Forward SurfaceDrawRecorder 共享材质 alpha、front-face、mesh 和 indirect visibility 逻辑，避免修复只落在一条路径。

### 涉及模块

- `src/render/frame/FrameFeatureResolver.cpp`
- `src/render/Renderer.cpp`
- `src/render/features/surface/SurfacePrepass.*`
- `src/render/features/surface/GBufferPass.*`
- `src/render/features/surface/GBufferContract.h`
- `src/render/features/surface/DepthHierarchyPass.*`
- `src/render/features/shadows_visibility/OcclusionCullPass.*`
- SSAO/GTAO/SSR/SSGI/TAA consumers

### 风险与兼容性

- Surface/GBuffer encoding 错配会产生 AO、SSR、TAA 的系统性错误，encoding 必须是可校验契约。
- Previous-frame occlusion 会引入一帧延迟，必须优先保守可见，不能追求激进剔除率。
- Forward 仍可能需要 SurfacePrepass；本阶段不是删除该 Pass。
- RenderGraph topology key 必须包含 producer 和 encoding，但普通 camera/lighting 数值不应触发重新编译。

### 完成条件

- Deferred + TAA/SSR/SSGI/AO 组合中不再同时执行完整 SurfacePrepass 与 GBuffer。
- Deferred opaque full-surface draw/raster 次数从两次降到一次。
- Forward、Deferred 的 screen-space debug 输出语义一致。
- camera cut、快速移动对象、resize 和场景切换时无错误 occlusion 缺失。

## Stage 3: Submission-Aware Shadow Rendering Cache

### 阶段目标

在不改变现有 CSM、Point Cube Array、Spot Array 和 lighting descriptor ABI 的前提下，跳过未变化 shadow layer 的重绘，并将缓存状态与最终 caster、物理资源和提交结果绑定。

### 当前问题与根因

`ShadowSystem` 已负责稳定 Point/Spot slot、方向光规划、矩阵、content hash 和 TAA reactive 状态，但它在 Visibility 之前执行，因此不知道：

- 每个 cascade/face/slot 最终包含哪些 caster。
- caster 的 world/mesh/material alpha revision。
- Shadow image 是否因容量切换或 residency 被重新创建。
- 本帧 command buffer 是否真正提交成功。

现有 `contentRevision` 还混合了灯光颜色/强度、矩阵和 allocation。颜色变化会影响 temporal lighting，但不会改变 shadow map 内容；它不能直接作为 render cache key。

### 推荐架构

保持 `ShadowSystem` 为 CPU 规划器，在 Renderer/Visibility 之后新增：

```text
ShadowSystem
  -> ShadowFramePlan          // light selection, slots, matrices

ShadowVisibilityBuilder
  -> ShadowCasterQueues       // final draw membership

Renderer-owned ShadowRenderCache
  -> ShadowRenderSchedule     // dirty/clean per cascade/face/slot
  -> Shadow Pass graph nodes
  -> pending cache commits
  -> commit after successful queue submit
```

不把 Vulkan image、serial 或 RenderGraph handle 放进 `ShadowSystem`。

### 3.1 分离 Revision

- `ShadowSystem` 输出至少三个独立 revision/hash：
  - `projectionRevision`：方向/位置/range/far/splits/matrix/slot。
  - `lightingTemporalRevision`：color/intensity 等影响 TAA 但不改变 shadow depth 的值。
  - `allocationRevision`：stable key 到 slot/layer 的映射。
- TAA reactive 可以同时考虑 lighting 与 shadow projection，但 shadow render cache只考虑实际 depth 内容。
- 移除“任意 light 参数变化都使所有 Shadow Map dirty”的全局逻辑。

### 3.2 Renderer-owned Cache Identity

每个 CSM cascade、Point face 和 Spot slot 保存独立 cache record：

```cpp
struct ShadowLayerCacheRecord {
    uint64_t physicalGeneration;
    uint64_t contentKey;
    uint64_t lastSubmittedSerial;
    bool valid;
};
```

`contentKey` 至少包含：

- 实际用于 shader 和 raster 的最终 view-projection/far plane。
- shadow resolution、format、bias-relevant raster policy 和 layer mapping。
- caster 数量与稳定顺序。
- 每个 caster 的稳定 RenderItem identity、world transform revision、ModelAsset generation、primitive/mesh identity。
- material handle generation、MASK cutoff/texture identity、double-sided/front-face 等会改变 depth draw 的状态。
- Shadow resource physical generation。

`ShadowVisibilityBuilder` 在生成每个确定性 caster queue 时同步滚动计算 `ShadowCasterQueueSignature`，包含上述 caster identity 和 draw-relevant state；`ShadowRenderCache` 直接消费该签名，不能为了构建 key 再遍历一遍 caster。若未来为 `RenderWorldFrameSnapshot` 增加 geometry revision，可用它跳过未变化世界的 queue membership重建，但这不是首版缓存的正确性前提。

不能把 raw pointer 当作持久 identity。未来可编辑 Material 必须提供 content revision；当前 immutable `MaterialHandle` generation 可作为过渡。

### 3.3 提交事务

- `ShadowRenderCache::buildSchedule()` 在 graph build 前比较 candidate key，输出 dirty mask。
- clean layer 不注册 clear/draw node，保留 image 原内容和 read-only layout。
- dirty layer录制后只产生 pending commit，不能立刻标记 valid。
- 扩展 `FrameSync::endFrame()` 或提交接口，返回/通知实际 submitted serial。
- Renderer 在 `vkQueueSubmit` 成功后提交 pending cache record；present failure 不应回滚已经提交的 shadow writes。
- command recording/submit 失败时丢弃 pending record。
- 资源 retirement/recreation 通过 physical generation 自动使所有关联 cache record 失效。

### 3.4 Directional CSM First

先只实现严格的 per-cascade CSM cache：

- sampled matrix 必须与生成缓存内容的 matrix 完全相同。
- camera、sun、shadow settings 或 cascade caster key 变化时只重绘对应 cascade。
- 静止相机、静止 sun、静态 caster 时四个 cascade 都应保持 clean。
- 任一 cascade 构建非法时沿用现有整帧 CSM disable 规则，不能错误复用不兼容旧 map。
- RenderGraph diagnostics显示每层 dirty reason、cache age、last submitted serial 和 draw saved。

不要在这一子阶段实现“远 cascade 每 N 帧更新”。节流会要求发布缓存矩阵而不是最新规划矩阵，属于后续独立策略。

### 3.5 Point/Spot Cache

CSM 经过稳定验证后复用同一 cache record 机制：

- Point 以 cube face 为最小缓存单位，但 light position/far/slot 变化会使六面一起 dirty。
- Spot 以 slot/layer 为单位。
- stable slot eviction、容量 image 切换和 light type 变化必须立即失效对应 record。
- caster queue 变化只影响关联 light/face，不污染其他 slot。

### 3.6 Shadow Format And Quality Tier

缓存完成后再降低单次重绘成本：

- capability 检查必须覆盖 depth attachment、sampled comparison 和 linear comparison filter。
- 增加 `ShadowDepthFormatPolicy { Auto, D16, D32 }` 或质量 preset；Auto 优先满足目标质量和设备支持。
- CSM 4x2048 从 D32 切到 D16 时理论资源从约 64 MiB 降到约 32 MiB；Point/Spot 同样按实际 capacity 报告。
- D16/D32 使用独立 bias preset，因为量化和 slope 行为不同。
- 不在同一提交中同时改缓存、格式和 bias，便于定位视觉回归。

### 暂不纳入本阶段

- Tight caster Z range：需要 provisional volume -> caster queue -> tight Z -> final matrix -> revalidation 两阶段流程，先记录到 Future Improvements。
- Far cascade update rate：必须保证 shader 使用缓存矩阵并处理动态 caster，严格缓存稳定后再评估。
- Shadow atlas、按需分辨率、静态/动态 caster 分层。

### 涉及模块

- `src/render/features/shadows_visibility/ShadowSystem.*`
- `src/render/features/shadows_visibility/ShadowVisibilityBuilder.*`
- `src/render/features/shadows_visibility/DirectionalShadowPass.*`
- `src/render/features/shadows_visibility/PointShadowPass.*`
- `src/render/features/shadows_visibility/SpotShadowPass.*`
- `src/render/Renderer.*`
- `src/render/graph/RenderResourcePool.*`
- `src/core/FrameSync.*`

### 完成条件

- 静态 Main Sponza 稳定帧 Directional Shadow GPU 时间接近零或只剩极小调度成本。
- 只移动一个 caster 时，只重绘受影响的 cascade/light/face。
- resize 不重建固定 shadow image；容量切换或 image recreation 必然使 cache 失效。
- submit 失败不会把未提交内容标记为 valid。
- cache on/off 画面一致，且 diagnostics 能解释每次 dirty 原因。

## Stage 4: Resource Residency, MSAA And Bandwidth Tiers

### 阶段目标

在 Stage 2 建立统一 Surface encoding 后，降低首次显存峰值、GBuffer 带宽和不必要的 MSAA 成本，并建立可观测的质量档。

### 4.1 移除 ResourcePool Eager Bootstrap

#### 当前问题

`Renderer` 初始化时调用 `RenderResourcePool::realize()`，创建所有已注册 image；随后 `synchronizeResidency()` 才把不活跃资源转入 Retiring。已有状态机能够增量创建/退役，但首次启动没有利用它。

#### 推荐方案

- sampler 和少量 persistent fallback 资源继续 eager create。
- image 初始为 Unallocated；第一次 compiled graph 后只实例化：
  - 当前 active resource set。
  - 明确 `persistentResidency` 的资源。
  - imported/swapchain wrappers。
- 复用现有 Resident/Retiring/submission serial 机制，不新增第二套 allocator。
- `prepareResources()` 在 descriptor preparation 前确保本帧需要的物理资源已创建。
- 首次 feature 开启允许一次 allocation hitch，并在任务/diagnostics 中显示；后续 feature toggle 保留现有 residency policy。

#### 完成条件

- 启动时 resident bytes 接近默认 topology active bytes，而不是所有 feature 支持资源总和。
- 第一次启用 SSR/SSGI/DDGI 等资源后 descriptor generation 正确更新。
- feature 快速开关不会在 in-flight serial 完成前销毁 image。

### 4.2 Render Path-aware MSAA Policy

- 使用 Stage 1 的显式 MSAA 状态。
- Deferred 和 TAA 路径默认建议 1x；Forward 可提供 Off/2x/4x，`DeviceMax` 只作为诊断/质量选项。
- requested/effective sample count进入 RenderGraph topology key、Pipeline rendering signature 和 resource desc。
- 切换 MSAA 只重建依赖 sample count 的 HDR/depth/forward resources，不清空无关 compute pipeline。
- 先保持产品默认不变测量，再根据 Algorithm Playground/Main Sponza 的质量与耗时决定默认值，避免文档直接规定未经验证的 4x。

### 4.3 GBuffer Encoding And Packing

#### 当前问题

当前 GBuffer color + motion + nominal depth 约 28 B/pixel：

- BaseColor/Metallic：RGBA8，4 B。
- Normal/Roughness/Occlusion：RGBA16F，8 B。
- Emissive/Flags：RGBA16F，8 B。
- Motion：RG16F，4 B。
- Depth：约 4 B。

这是明确的带宽优化候选，但不能在 canonical Surface contract 之前直接换格式。

#### 推荐方案

- 在 `GBufferContract` 中定义 versioned `GBufferEncoding`，列出每 channel 编码、format 和 decode helper。
- 对候选格式做 device feature 检查：color attachment、sampled、storage/linear filtering仅在消费者确实需要时要求。
- 优先评估：
  - octahedral normal 两通道编码。
  - 将 roughness、AO、metallic 和 flags 合理打包到 8-bit channel。
  - emissive 使用适合 HDR 范围的 packed float format或保持独立高精度 target。
  - Motion 保持 RG16F，除非 TAA/SSR 误差数据支持进一步压缩。
- 所有 Deferred、screen-space debug 和 temporal consumer 只调用共享 decode helper。
- 通过 RenderGraph resource bytes 和 GPU pass timing比较，不以“附件数量更少”代替真实收益。

不要在计划中预先锁死最终 format。完成标准是有实测支持的 encoding，而不是达到任意理论 Bpp。

### 4.4 Transient Aliasing Decision Gate

当前 ResourcePool 已报告 active/resident/retiring bytes，但没有 transient physical aliasing。只有满足以下条件才进入实现：

- Stage 0 能输出 logical lifetime 和峰值并证明同 frame 不重叠资源占显著显存。
- 资源的 format、extent、samples、usage、memory type 和 alignment兼容。
- History、Persistent、Imported、shared Shadow 和外部 SDK 资源禁止 alias。

如果满足，再增加基于 compiled lifetime interval 的 alias group；否则只保留驻留优化，不为代码形式完整而实现 allocator。

### 风险与兼容性

- format/MSAA 变化会影响 visual baseline 和 Cooked runtime pipeline signature，应分提交验证。
- eager create 删除后，Pass 不得缓存无 generation 的 raw image view。
- aliasing 一旦错误会导致跨 Pass 数据破坏，默认不纳入首轮交付。

### 完成条件

- 默认 topology 启动显存峰值下降且无无用 feature image 创建。
- MSAA/sample shading requested/effective 状态可查询，Forward/Deferred 选择明确。
- 新 GBuffer encoding 有带宽、GPU 时间和视觉误差数据；没有数据则保留旧格式。

## Stage 5: Pyramid Scheduling With A Measurement Gate

### 阶段目标

在不破坏 RenderGraph subresource 可见性的前提下，降低 depth/color pyramid 的 CPU node、descriptor、dispatch 和 barrier 开销。Bloom 保持独立。

### 当前问题与根因

- DepthHierarchy 已支持 combined min/max RG32F 或 split R32F，并按 mip 注册 Graph node。
- SceneColorPyramid 也是逐 mip node。
- Bloom 使用六张独立 image、亮部提取、downsample 和 tent upsample，不是普通 mip reduction。
- 原方案将所有 pyramid 合并为通用 single-pass，会隐藏真实差异并增加难以维护的 shader 分支。

### 决策门槛

Stage 0 必须分别报告：

- pyramid graph setup CPU。
- node 数、dispatch 数、descriptor writes、barrier API calls。
- DepthHierarchy、SceneColorPyramid、Bloom GPU 时间。

只有当 pyramid CPU/driver 开销在目标场景中占 CPU frame 的显著比例，或 GPU dispatch/barrier 固定成本明显时，才实施后续子项。

### 推荐方案

#### 5.1 Shared Pyramid Execution Plan

- 为 DepthHierarchy 和 SceneColorPyramid 提取共享的 mip extent、src/dst subresource、descriptor mapping 和 dispatch size plan。
- 保留各自 reduction semantic 和 shader program。
- descriptor set按 resource physical generation创建/更新，不在每帧逐 mip重写。
- odd dimensions、1xN、Nx1 和最后 1x1 由共享 helper处理。

#### 5.2 Graph-native Iterative Node Batching

如果主要瓶颈是 CPU node 开销：

- 为 RenderGraph 增加显式 `IterativeSubresourceNode` 或 node group metadata。
- Compiler 仍能看到每级 mip 的 read/write 和顺序，但 executor可在一个 group内复用 pipeline、descriptor和标签层级。
- Group内部 barrier由 compiler生成的 substep plan执行，不能退回 Pass 私有的不可见 barrier。
- Diagnostics仍展开显示每个 mip 的逻辑 transition。

#### 5.3 Optional Single-dispatch SPD-like Path

只有 GPU dispatch/barrier 数据证明值得时，再基于经过验证的 SPD 类算法实现 color/min/max的专用路径：

- capability、最大 mip、atomic/scratch需求和 odd extent必须明确。
- 保留现有逐 mip fallback。
- Depth min/max 和 color average使用不同 shader contract。
- Bloom 不接入此路径。

### 完成条件

- 优化路径在相同输入下产生一致的 mip 语义。
- node/dispatch/barrier/descriptor 数量和 CPU/GPU 时间有可量化下降。
- 如果未达到决策门槛，本阶段以“保留现状并记录数据”完成，而不是强制增加复杂度。

## Stage 6: GPU-Driven Opaque Submission

### 阶段目标

将当前“GPU 判定 visible、CPU 仍逐 item bind/push/draw”的路径升级为按 pipeline/material bucket 的 draw compaction 和少量 multi-draw。Transparent 继续 CPU 排序和提交。

### 当前前置事实

- Bindless Material 已提供稳定 material index。
- `RenderItem` 已有稳定 identity、material index、bounds、current/previous world。
- Occlusion compute 已写 indirect command，但每个 candidate 只用 `instanceCount=0/1`。
- MainForward/GBuffer 仍逐项绑定 Mesh、push 128B draw block并执行 `vkCmdDrawIndexedIndirect(..., 1, ...)`。
- Mesh vertex/index buffer彼此独立；Ray Query BLAS 可直接引用这些 buffer。
- Device 尚未完整查询/启用 `multiDrawIndirect`、`drawIndirectCount` 等 GPU-driven capability。

因此不能直接把 single indirect count 改大；必须先解决 DrawData 索引和 geometry ownership。

### 6.1 Capability And Fallback Contract

- 新增 `GpuDrivenSupport`，查询并报告：
  - `multiDrawIndirect`
  - `drawIndirectFirstInstance`
  - Vulkan 1.2 `drawIndirectCount`
  - storage buffer范围与 alignment
  - buffer device address 与 Ray Query兼容情况
- `Auto` 在能力完整时启用，缺失时保留 CPU draw path。
- Diagnostics区分 requested/active/fallback reason。

### 6.2 DrawData SSBO

- 将 model/previousModel/material/renderItem/flags 等 per-draw 数据移到 per-frame `GpuDrawData[]`。
- shader通过 `gl_DrawID` 或 indirect `firstInstance`索引 DrawData；选择一种后固定到 ABI。
- Push constant只保留 pass级或 bucket级小数据，不再逐 draw push完整 model matrix。
- Surface、GBuffer、Forward和 MASK Shadow共享 draw data contract。
- CPU fallback仍可生成相同 DrawData并逐条 direct/indirect draw，确保 shader ABI只有一套。

### 6.3 Geometry Arena And Mesh Table

- 引入 ModelAsset-local 或全局 append-only geometry arena：
  - 公共 vertex/index buffer。
  - `GpuMeshRecord { firstIndex, indexCount, vertexOffset, bounds, addresses }`。
  - indirect command通过 mesh record构造。
- 选择 ModelAsset-local 还是全局 arena前，先比较资源回收、碎片和重复实例需求；v1 推荐 ModelAsset-local，避免跨资产 compaction和大范围 relocation。
- BLAS 构建改为使用 arena buffer + offset/device address。
- arena发布后不移动；需要重导入时创建新 generation，旧 generation按 AssetRepository serial retirement销毁。
- 不能在 GPU 使用期间原地压缩或覆盖 arena。

### 6.4 Bucket Build And GPU Compaction

- CPU 在 scene/model/material/pipeline topology变化时建立稳定 bucket：
  - pipeline rendering signature
  - shader family
  - alpha/cull/front-face
  - geometry arena/material table compatible group
- 每帧只上传 transform/bounds和活动 item列表。
- GPU culling写 compacted `VkDrawIndexedIndirectCommand[]` 和 per-bucket count buffer。
- 通过 `vkCmdDrawIndexedIndirectCount` 每 bucket提交一次或少量调用。
- Compute -> DrawIndirect 和 DrawData/mesh table shader read由 RenderGraph buffer usage自动同步。
- Opaque/MASK先迁移；Transparent保持 CPU back-to-front。

### 6.5 Shadow Streams

主视图稳定后再迁移 Shadow：

- Shadow caster queue复用同一 DrawData/Mesh table，但按 cascade/face/slot产生独立 compacted stream。
- MASK仍需 material/texture alpha路径。
- 与 Stage 3 cache结合：clean layer不执行 cull或draw；dirty layer才生成 stream。

### 涉及模块

- `src/render/geometry/RenderItem.h`
- `src/render/geometry/Mesh.*`
- `src/render/features/shadows_visibility/OcclusionCullPass.*`
- `src/render/features/forward/MainForwardPass.*`
- `src/render/features/surface/GBufferPass.*`
- `src/render/features/surface/SurfaceDrawRecorder.*`
- `src/scene/AssetRepository.*`
- Model GPU builder、RayTracingScene 和 shader ABI

### 风险与兼容性

- Geometry Arena 直接影响 BLAS、debug naming、upload和资源退役，是本阶段最大风险。
- `gl_DrawID`/`firstInstance`约定必须覆盖 direct fallback、shadow和不同 draw APIs。
- bucket过细会把 multi-draw收益抵消；bucket数量必须纳入诊断。
- 动态材质编辑需要 copy-on-write material handle，不能破坏在途 DrawData。

### 完成条件

- Main Sponza opaque draw API call从逐 primitive降为与 bucket数量同量级。
- GPU compaction后不可见 item不产生实际 draw command。
- CPU fallback与GPU-driven画面一致并可启动时选择。
- 重复 ModelInstance共享 geometry/material，只有 transform DrawData不同。
- BLAS/Ray Query路径继续引用有效 arena地址。

## Stage 7: Upload Service And Conditional Parallel Execution

### 阶段目标

根据 Stage 0/Tracy 数据减少大型模型加载的 staging churn和 graphics queue竞争；只有在确有收益时再引入 transfer queue、secondary recording或 async compute。

### 当前实现边界

- `IncrementalUploadQueue` 已有两个 128 MiB slot、持续映射 staging、command pool、fence polling和 batch upload。
- `AssetRepository` 使用一个 CPU worker和最多一个活动 ModelGpuBuilder，避免峰值失控。
- 每个 ModelGpuBuilder拥有自己的 upload queue/staging allocation，生命周期结束后释放。
- Mesh copy后可在同一 command buffer执行 BLAS build；BLAS不是纯 transfer workload。
- Device当前只选择 graphics/present family，并为每个 family请求一条 queue。

### 7.1 Shared Upload Service

在不改 Queue ownership前先解决可确认的 allocation churn：

- 将 builder-local `IncrementalUploadQueue`演进为 AssetRepository/Renderer生命周期的 `GpuUploadService`。
- 使用可复用 mapped staging pages或受预算约束的 ring，而不是永久固定一个更大的单 buffer。
- page只有在其 fence/serial完成后才能复用。
- 纹理/vertex/index copy与BLAS build作为同一 upload job中的不同阶段显式记录。
- ModelGpuBuilder只持有 job token，不拥有 Vulkan command pool/staging allocation。
- 提供 staging resident bytes、high-water、page reuse、allocation count和queue submit time。

如果 Stage 0 显示现有 builder创建频率/分配成本可忽略，则保留现状，不实施服务重构。

### 7.2 Dedicated Transfer Queue Decision

只有 graphics queue上传等待或大型加载期间 frame GPU时间明显恶化时实施：

- Device queue family discovery增加 dedicated/preferred transfer family。
- 把 texture和vertex/index copy从BLAS build拆开。
- transfer完成后通过 semaphore/timeline和必要 queue-family ownership transfer通知graphics queue。
- BLAS build继续在支持 AS build的 graphics/compute queue执行。
- concurrent sharing与explicit ownership在目标硬件上比较后选择，不能默认 concurrent更快。
- 没有独立transfer family或收益不足时继续graphics queue。

### 7.3 Timeline Semaphore And Submit2

- Timeline semaphore只在 shared upload service或multi-queue确实需要跨queue job dependency时引入。
- 迁移到 `vkQueueSubmit2`可以统一 synchronization2语义，但它本身不作为性能优化宣传。
- 帧级 acquire/present binary semaphore可继续保留。

### 7.4 Parallel Command Recording Decision

只有 Tracy显示 primary command recording在GPU-bound之外仍占显著CPU且节点足够重时实施：

- RenderGraph compiler为无依赖 graphics node生成recording batch。
- 每worker使用独立per-frame command pool和secondary command buffer。
- Dynamic Rendering secondary inheritance必须包含完整attachment format/sample/view mask。
- 不并行录制细小mip node，避免调度成本超过收益。
- Descriptor/pipeline/resource对象在recording期间必须只读或具备线程安全cache。

### 7.5 Async Compute Decision

只在设备有真正可并行compute queue且GPU timeline显示可重叠空隙时评估：

- 先选择与graphics数据依赖较弱的工作，而不是机械移动所有compute Pass。
- RenderGraph需增加queue ownership、cross-queue semaphore和resource state split。
- SSAO/SSR/SSGI/TAA等紧邻graphics producer/consumer的Pass可能没有实际overlap窗口。
- 本计划不预先承诺实现。

### 完成条件

- 每个实施的子项都由实施前数据触发，并有独立前后结果。
- scene loading期间frame pacing改善，且没有增加常驻staging到不可接受水平。
- multi-queue路径没有隐式queue idle、错误ownership或BLAS依赖。
- 若数据不满足门槛，记录结论后保留单graphics queue是有效完成结果。

## Cross-Cutting Diagnostics

Diagnostics 和 Runtime Control 最终应能回答以下问题：

### CPU And Driver

- 本帧 graph是否重新编译，原因是什么。
- prepareGraph、resource sync、descriptor prepare、visibility、record和submit分别耗时多少。
- 调用了多少次 descriptor update、pipeline create、barrier API、draw和dispatch。
- 每个 draw bucket包含多少item。

### GPU

- Shadow、Surface/GBuffer、DepthHierarchy、AO、SSR、SSGI、DDGI、Bloom、TAA和Present耗时。
- Shadow layer dirty/clean、draw saved和cache age。
- Pyramid node/dispatch/barrier数量。
- GPU-driven cull input/visible/compacted/bucket count。

### Memory

- active/resident/retiring bytes，按资源域分类。
- shadow format/capacity、GBuffer Bpp和MSAA sample count。
- upload staging resident/high-water。
- ModelAsset geometry/material/texture和BLAS bytes。

### Correctness State

- descriptor resource physical generation。
- canonical surface producer和encoding。
- previous-frame occlusion history valid/invalid reason。
- shadow cache invalidation reason和last submitted serial。
- requested/effective quality和capability fallback reason。

所有计数都应有稳定定义；日志只在状态变化时输出，不能每帧刷屏。

## Verification Strategy

遵循项目级开发策略，默认只构建和实际启动，不运行 CTest、Golden、视觉回归或 Validation smoke。每阶段至少执行：

1. 构建并启动 `windows-msvc-dev-fast`，确认开发路径可运行。
2. 对影响 runtime feature裁剪的阶段构建并启动 `windows-msvc-runtime`。
3. 使用 `windows-msvc-perf`运行固定场景矩阵，保留前后JSON和Markdown结果。
4. Algorithm Playground用于低draw基础成本，Main Sponza用于Shadow/Surface/draw规模，GI Calibration Lab用于多算法组合。
5. 手动切换Forward/Deferred、Shadow、AO、SSR、SSGI、TAA、DDGI、Atmosphere和Bloom。
6. 检查resize、scene reload、model generation替换、environment切换和Viewport隐藏/恢复。
7. 高风险同步/缓存阶段使用RenderDoc检查实际resource、layer、layout、descriptor和indirect buffer；Validation只在用户明确要求时运行。
8. 执行`git diff --check`。

性能结果必须同时报告：

- 构建preset、GPU/driver、分辨率、scene和camera。
- requested/effective RenderSettings。
- CPU median/p95、GPU median/p95。
- Graph topology和关键计数器。
- 画质或资源格式是否变化。

不能用Debug对Release、不同MSAA、不同camera或不同feature集合宣称优化收益。

## Suggested Delivery Order

建议按可独立回滚和测量的提交交付：

1. `perf: add release renderer benchmark profiles`
2. `perf: expose Vulkan submission counters`
3. `fix: make sample shading an explicit pipeline state`
4. `perf: avoid stable descriptor rewrites`
5. `perf: batch render graph resource barriers`
6. `perf: persist native Vulkan pipeline caches`
7. `refactor: define canonical opaque surface products`
8. `perf: use previous surface history for occlusion`
9. `perf: remove duplicate deferred surface raster`
10. `refactor: separate shadow planning and render cache state`
11. `perf: cache unchanged directional shadow cascades`
12. `perf: cache unchanged punctual shadow layers`
13. `perf: add explicit shadow depth quality tiers`
14. `perf: realize render resources on first use`
15. `perf: add measured GBuffer packing`
16. `perf: reduce pyramid scheduling overhead`，仅在门槛满足时。
17. `refactor: move per-draw state to GPU draw data`
18. `refactor: upload model geometry into stable arenas`
19. `perf: compact opaque draws on the GPU`
20. `perf: reuse incremental upload staging pages`，仅在门槛满足时。
21. `docs: record renderer performance architecture and results`

不要把 Pipeline/Descriptor/Surface/Shadow/GPU-driven 混在一个大提交中。每个性能提交必须附带对应报告变化，代码审查才能区分结构收益和质量变化。

## Risks And Mitigations

| 风险 | 缓解措施 |
|---|---|
| Barrier 合并后遗漏 hazard | Compiler保留逐usage逻辑计划；executor只合并Vulkan调用，不删除依赖 |
| stage/access收紧错误 | 逐Pass声明shader stage；保留diagnostics和RenderDoc检查 |
| Descriptor缓存引用旧image view | 物理resource generation进入binding key；recreate前清理/重写 |
| Previous-frame occlusion误剔除 | camera cut、动态对象、新对象和invalid history一律保守可见 |
| Shadow缓存复用错误内容 | key包含最终matrix、caster/material/resource generation；只在submit成功后commit |
| Shadow缓存导致TAA状态不一致 | 分离shadow content、lighting temporal和allocation revision |
| D16/GBuffer packing画质损失 | 独立quality tier、共享decode contract、分提交对比 |
| GPU-driven破坏Ray Query BLAS | Geometry Arena先设计稳定address/offset和generation retirement |
| Bucket数量过多抵消multi-draw | 记录bucket/item分布；保留CPU fallback |
| Staging service增加常驻显存 | page budget、high-water和idle回收；收益不足则不实施 |
| 多Queue复杂度高于收益 | 以graphics queue contention和可重叠窗口作为硬门槛 |

## Future Improvements

以下方向有价值，但不应扩大本计划当前实施范围：

- Tight caster Z range和稳定的far cascade更新频率。
- Dynamic shadow resolution、shadow atlas和静态/动态caster分层。
- `VK_EXT_memory_budget` admission、资源streaming和场景切换显存预算。
- RenderGraph transient aliasing的完整allocator。
- Dynamic resolution、VRS和按GPU frame budget的质量调节。
- Mesh shader、meshlet culling和bindless vertex pulling。
- `VK_EXT_descriptor_buffer`。
- Async compute、parallel secondary recording和多Queue frame scheduling。
- Persistent geometry defragmentation和跨ModelAsset全局arena。
- Pipeline library/shader object等更激进的pipeline创建策略。

这些项目只有在前述阶段完成、数据表明当前瓶颈已经转移后再立项。

## Related Documents

- `doc/architecture/rendering.md`
- `doc/architecture/render_graph.md`
- `doc/architecture/resource_loading.md`
- `doc/archive/plans/render_architecture/deferred_render_paths_and_material_shaders_plan.md`
- `tools/performance/README.md`
