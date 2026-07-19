# 大型场景响应式加载路线图

> Status: Active
> Last verified: 2026-07-19
> Verified against: `c3aa7eb`

## Summary

本计划将 VulkanLab 的场景加载从“主线程同步构建完整 Scene”演进为可观测、可增量推进、可取消的加载任务。目标是在加载 Main Sponza 等大型场景时持续处理窗口事件、刷新 ImGui 和响应 Runtime Control，同时控制 GPU 上传时间与显存峰值。

路线图包含已经完成的诊断与批量上传基线，以及四个后续阶段：CPU 后台准备、每帧增量 GPU 上传、任务控制、压缩纹理资产管线。更复杂的纹理 residency/streaming 只有在压缩纹理完成后仍不能满足目标时才进入实施。

阶段 0、1、2 和 3 已实现；本文件继续作为阶段 4 的活动路线图。当前能力以 [资源加载](../architecture/resource_loading.md) 为准。

## Current Baseline

### 已完成：阶段 0，加载诊断

- SceneLoadStats 记录场景切换、glTF 解析、图片读取/解码/缩放、材质、Mesh 和上传耗时。
- ResourceLoadStats 记录资源数量、上传字节、submit/wait 和 peak staging。
- VMA 快照记录加载前后的 allocation count、allocation bytes 与 block bytes。
- 日志、ImGui Stats 面板和 Runtime Control 可以读取最近一次加载结果。

### 已完成：阶段 1，批量 GPU 上传

- Texture、mipmap、vertex 和 index 上传共享 UploadContext。
- 默认批次上限为 128 MiB，使用独立 command pool、command buffer 和 fence。
- 场景资源上传路径不再为每个资源调用 `vkQueueWaitIdle()`。

### 已完成：阶段 2，响应式场景加载

- GltfPreparer 在 SceneLoadManager worker 中生成纯 CPU PreparedSceneData。
- SceneGpuBuilder 使用双 slot IncrementalUploadQueue 按帧创建和上传 GPU 资源。
- 普通加载只轮询 fence；显式 teardown/退出之外不无限等待上传 fence。
- generation、CPU/GPU 取消、Loading UI、失败收尾和 descriptor set 回收已经接入。
- Runtime Control v2 提供 taskId、`load.status`、`load.cancel` 和客户端 `--no-wait`。

阶段 3 已加入 KTX2 派生纹理资产管线：离线工具生成 UASTC + Zstd 缓存，worker 优先转码为 BC7 并保留 stb fallback，GPU 直接上传预生成 mip chain。Main Sponza 的最终画面和显存曲线仍需在目标 GPU 上手动验证。

## Goals

- 加载大型场景时窗口持续响应，ImGui 能显示阶段、进度、耗时和取消操作。
- CPU 重任务不接触 Vulkan、GLFW、ImGui 或当前 Scene，可以在 worker thread 独立执行。
- GPU 创建和上传由主线程按每帧预算推进，不进行无限期 fence 阻塞。
- 新场景只在全部资源可用后一次性发布，不能渲染半初始化对象。
- 快速连续切换场景时，旧任务不能覆盖较新的请求。
- 取消、失败和退出路径都能回收 CPU 数据、staging、半成品 Vulkan 资源和线程。
- Runtime Control 可以启动加载、查询状态、等待完成和取消任务。
- 保留阶段 0 的统计口径，并增加异步队列、逐帧上传和取消数据。

## Non-Goals

- 本计划不重构 Forward Pass、材质 descriptor layout 或 Shader variant 系统。
- 阶段 2 不引入专用 transfer queue、queue ownership transfer 或 bindless descriptor。
- 阶段 2 不解决渲染期 mesh/texture LOD，也不实现 virtual texturing。
- 不在运行时修改原始 glTF 或源图片；阶段 3 生成独立的派生资产和缓存。
- 不保证加载期间始终保留旧场景。显存稳定性优先于旧场景与新场景同时驻留。

## Target Architecture

### 数据分层

加载链路拆分为三种数据：

1. `PreparedSceneData`：纯 CPU 数据，包含解码后的图片、sampler 描述、材质参数、vertex/index、对象层级、bounds 和相机建议。不得包含 `Vk*` handle、VMA allocation 或 DescriptorSet。
2. `SceneBuildState`：主线程消费 PreparedSceneData 时的游标、资源映射、待上传操作、in-flight batch 和半成品 Scene。
3. `Scene`：所有 GPU 资源完成且 descriptor 可用后发布的运行时场景。

阶段 2 已把原先直接创建 Texture/Mesh 的 GltfLoader 拆为：

```text
GltfPreparer::prepare(path, options, cancellationToken)
    -> PreparedSceneData

SceneGpuBuilder::begin(preparedData)
SceneGpuBuilder::pump(frameBudget)
    -> InProgress | WaitingForGpu | ReadyToPublish | Failed | Cancelled
```

Viking Room 等内建场景可以先通过同步 adapter 接入相同任务状态机，不要求第一步立即重写 OBJ parser。

### 任务状态机

```text
Queued
  -> PreparingCpu
  -> ReadyForUpload
  -> ReleasingPreviousScene
  -> Uploading
  -> WaitingForGpu
  -> ReadyToPublish
  -> Completed

任意未完成状态 -> Cancelling -> Cancelled
任意执行状态   -> Failed
```

每个任务具有单调递增的 `taskId` 和 generation。只有仍是最新 generation 的任务可以发布 Scene。进度由阶段和已完成工作量共同表示，不用虚假的固定计时百分比。

### 线程所有权

| 操作 | Worker thread | Main thread |
|---|---:|---:|
| 文件读取、glTF 解析 | 是 | 否 |
| 图片解码与 resize | 是 | 否 |
| 顶点转换、normal/tangent、bounds | 是 | 否 |
| 创建/销毁 Vulkan 与 VMA 对象 | 否 | 是 |
| Descriptor 分配与更新 | 否 | 是 |
| command buffer 记录与 queue submit | 否 | 是 |
| Scene 发布和 UI 状态 | 否 | 是 |

worker 只通过带 mutex 的结果队列、atomic cancellation flag 和只读任务参数通信。Application 停止前必须发出取消并 join worker，不能让线程引用已销毁的 Application 或日志系统。

### 显存策略

CPU prepare 期间继续渲染当前 Scene。进入 GPU build 前需要处理旧场景驻留问题：

- v1 默认等待 device idle，清理 PipelineCache 并释放当前 Scene，然后显示轻量加载界面，再增量创建新场景。
- 这样不会让两个 Main Sponza 级别的场景同时占用显存，但加载失败后可能暂时没有可渲染场景。
- 后续启用 `VK_EXT_memory_budget` 并建立可靠的预计资源大小后，可以在预算充足时选择保留旧场景直到新场景发布。

不能在没有预算判断时默认同时保留两个大场景。

## Stage 2A: CPU Background Preparation

> Implementation: Completed on 2026-07-18

### Scope

- 新增 `SceneLoadManager`、`SceneLoadTask`、CancellationToken 和线程安全结果队列。
- 从原 GltfLoader 提取纯 CPU 的 `GltfPreparer`，使解析、图片读取/解码/缩放、材质转换、Mesh CPU 转换、tangent 和 hierarchy 不依赖 Device、UploadContext 或 DescriptorAllocator。
- PreparedTexture 保留像素格式语义、mip 需求和 sampler 参数；PreparedMaterial 通过稳定 index 引用 texture；PreparedObject 通过稳定 index 引用 mesh/material。
- Application 每帧轮询任务状态，继续渲染当前 Scene，并在 Loading 面板显示当前阶段、资源计数、已处理字节、耗时和错误。
- CPU prepare 完成后，第一版仍可一次性在主线程使用现有 UploadContext 构建 GPU Scene，以独立验证线程边界。

### Required Refactors

- `SceneFactory` 不再是所有场景唯一的即时 GPU 构造入口。增加 prepare factory，或给 SceneEntry 区分 synchronous 与 prepared source。
- Texture 增加接收已解码 RGBA8 的路径，避免主线程重复 decode/resize。
- Mesh 增加接收 prepared vertex/index 的路径，避免主线程重复 tangent 生成。
- ResourceLoadStats 区分 worker CPU 时间、主线程 GPU build 时间和 prepare result 排队时间。

### Acceptance

- Main Sponza 的 glTF parse、图片 decode/resize 和 mesh CPU 阶段不再出现在主线程 profile 中。
- CPU prepare 期间窗口能移动、重绘并响应 shader/status 命令。
- PreparedSceneData 单元测试不创建 Vulkan instance，也能验证材质索引、对象引用、bounds 和资源计数。
- worker 异常转换为 Failed 状态，不跨线程抛入 Application。

阶段 2A 完成后，GPU build 仍可能造成一段卡顿。这是该阶段的已知边界，不应宣称已经完成响应式加载。

## Stage 2B: Incremental GPU Upload

> Implementation: Completed on 2026-07-18

### Scope

- 新增 SceneGpuBuilder，把 Texture、Mesh、Material 和 SceneObject 创建拆成可恢复的小步骤。
- 将 UploadContext 的同步 `finish()` 路径扩展或替换为非阻塞 batch uploader：submit 后记录 fence，后续帧使用 `vkGetFenceStatus()` 轮询完成状态。
- 使用至少两个独立 batch slot。每个 slot 持有自己的 command buffer、fence、staging 生命周期和已引用的目标资源，fence 完成前不得 reset 或覆盖。
- Application 每帧调用 `pump()`，同时限制 staging bytes、资源数量和 CPU 记录时间。初始建议软预算为 `32 MiB/frame` 和 `2 ms` command recording，最终值通过 LoadStats 调整。
- 单个 Texture 或 Mesh 大于字节预算时允许作为原子任务超过软预算；不得因无法拆分而永久饥饿。
- mip upload、barrier 和最终 shader-read transition 必须在资源发布前完成。

### Frame Integration

每帧顺序调整为：

1. poll events、Runtime Control 和输入。
2. 轮询 worker result 与 upload fences。
3. 在预算内推进 SceneGpuBuilder。
4. 更新 loading UI。
5. 渲染当前 Scene 或空场景/loading 界面。
6. 新 Scene ReadyToPublish 时，在帧边界原子替换 `currentScene_`。

GPU upload 使用 graphics queue，暂不引入 transfer queue。独立上传 submit 和正常帧 submit 的顺序必须明确，Scene 发布前确认所有上传 fence 已完成。

### Descriptor And Resource Lifetime

- Image/ImageView/Sampler、Buffer 和 descriptor 可以在 build 期间创建，但不能进入 RenderQueue。
- SceneBuildState 持有所有半成品资源。失败或取消后，只在关联 fence 完成时销毁。
- DescriptorAllocator 必须支持释放任务使用的 descriptor，或为每个 build 使用可整体回收的 pool/arena。
- PipelineCache 只在 Scene 发布或旧场景释放时处理，不为每个上传资源清理。

### Acceptance

- Main Sponza GPU build 期间主循环持续推进，窗口不再被 Windows 标记为无响应。
- 主线程加载路径不调用无限期 `vkWaitForFences()` 或 `vkQueueWaitIdle()`；仅在明确 teardown/退出边界允许 device idle。
- 每帧 upload pump 耗时和字节数进入 Stats，可确认预算是否被遵守。
- 新场景首次进入 RenderQueue 前，validation layer 不报告未完成 copy、layout、descriptor 或资源生命周期错误。
- 与同步基线相比，纹理、mesh、material、object 数量和最终画面一致。

## Stage 2C: Task Control, Cancellation And Automation

> Implementation: Completed on 2026-07-18

### Scope

- Scene 面板显示 queued/preparing/uploading/waiting/completed/failed/cancelled，并提供取消按钮。
- 新的 scene 请求自动使旧 generation 进入 cancelling；最新请求优先。
- CPU worker 在文件、图片、primitive 等自然边界检查 cancellation token。
- 未提交上传立即丢弃；已提交 batch 等 fence 完成后回收，不使用 device idle 作为普通取消手段。
- `app.quit` 依次停止接收新任务、取消 worker、等待/回收 in-flight batch、join thread，再销毁 Vulkan。

### Runtime Control v2

协议增加：

- `load.status`：查询当前或指定 taskId。
- `load.cancel`：取消指定任务。
- `scene.load`：快速返回 taskId，不再让 Named Pipe 请求占用整个加载过程。

为保持命令行使用体验，`VulkanLabCtl scene load` 默认在客户端轮询直至完成；增加 `--no-wait` 立即打印 taskId。JSON 协议需要提高版本号，并在 system.info 中暴露 capability，避免旧脚本误判同步语义。

### Failure Semantics

- CPU prepare 失败：保留当前 Scene，返回结构化错误。
- 释放旧 Scene 后 GPU build 失败：进入可恢复的空场景，UI 和控制服务继续工作，可加载其他 Scene。
- 取消成功只代表不会发布该任务；已经提交的 GPU 工作可能在后台完成后才释放。
- 任务历史保留有限条目，避免长时间运行时无限增长。

### Acceptance

- 连续发起多个场景切换，只有最后一个 generation 能发布。
- 在 CPU prepare、GPU upload 和 waiting fence 三个阶段取消均不崩溃、不泄漏、不发布旧任务。
- Runtime Control 可以在加载期间执行 ping、status、cancel 和 quit。
- 自动化测试能通过 taskId 等待 Main Sponza 完成并读取对应 SceneLoadStats。

## Stage 3: KTX2 And Derived Asset Pipeline

> Implementation: Completed on 2026-07-19; final visual acceptance remains manual

阶段 2 解决响应性，不会减少 PNG/JPEG decode 成本和 RGBA8 纹理常驻体积。阶段 3 引入离线派生资产，优先采用成熟的 KTX-Software/libktx 与 Basis Universal，不自行设计压缩格式。

### Scope

- 增加离线 asset preparation tool，将源 glTF 图片转换为带完整 mip chain 的 KTX2。
- cache key 至少包含源文件内容 hash、颜色语义、normal map 语义、目标尺寸和编码设置。
- BaseColor/Emissive 保持 sRGB；Normal、MetallicRoughness、Occlusion 保持 linear。
- 桌面 Vulkan 优先选择设备支持的 BC 格式，或使用 BasisU supercompression 在加载时转码到设备格式。
- runtime 直接上传预生成 mip，不再对这些纹理执行 CPU image decode、bilinear resize 或 GPU blit mip generation。
- 原始 `.gltf/.glb` 保持不变，通过 manifest/cache 映射到派生纹理；缓存缺失或无效时可回退现有加载路径。

### 固定设计

- KTX-Software 固定为 v4.4.2，通过 `external/ktx` submodule 递归初始化；运行时只使用 libktx 读取/转码，不使用其 Vulkan upload helper。
- `VulkanLabAssetTool texture-cache build --scene ... --texture-limit ...` 显式生成缓存；运行时不自动编码。
- 默认缓存根目录为 `derived_assets`，profile 只接受 `0/512/1024/2048` 并要求精确匹配。
- manifest 位于 `manifests/<scene-key>/<profile>.json`，内容寻址 KTX2 位于 `blobs/<cache-key>.ktx2`。manifest schema v1 由纯 CPU 类型负责读写和快速 file-stamp 校验。
- KTX2 使用 UASTC LDR 4x4 + Zstd 和完整 mip chain。runtime cache hit 优先转码 BC7；设备不支持 BC7 时转为 RGBA32；任何 miss/invalid 都回退 stb。
- 第一版统一 BC7，不修改 shader 以支持 BC5/BC4，也不引入 streaming、residency 或自动重建缓存。

### 开发与验证流程

1. 初始化 `external/ktx` submodule，并确保 Debug/Release 都能构建 libktx、`ktx` CLI 和 `VulkanLabAssetTool`。
2. 用纯 CPU 单元测试覆盖 manifest path/profile、semantic/wrap 查找、save/load round trip、file-stamp 失效和未知 schema 拒绝。
3. 在 worker 中接入 manifest lookup 和 KTX2 transcode，保留现有 stb fallback；不得从 worker 创建 Vulkan 对象。
4. 扩展 prepared texture payload 和 Texture 上传，使 prebuilt mip chain 不再走 GPU blit。
5. 把 cache 与 transcode 字段接入日志、Stats UI 和 Runtime Control JSON。
6. 分别生成并验证 Main Sponza profile：

```powershell
VulkanLabAssetTool texture-cache build --scene models/main_sponza/NewSponza_Main_glTF_003.gltf --texture-limit 1024
VulkanLabAssetTool texture-cache build --scene models/main_sponza/NewSponza_Main_glTF_003.gltf --texture-limit 2048
```

生成命令的工作目录决定默认 `derived_assets` 位置；自动化验证应显式设置 `--cache-root`，确保它与 `VulkanLab.exe` 的运行目录一致。

### Acceptance

- Main Sponza 纹理 CPU decode/resize 时间显著下降。
- texture upload bytes、VMA allocation delta 和专用显存明显低于 RGBA8 2048 基线。
- sRGB、normal、MR、AO、alpha 和 emissive 视觉回归通过。
- cache key 改变时正确重建，不复用不同语义或不同尺寸的旧纹理。
- cache hit 统计与实际材质引用一致，cache miss/invalid 原因可见；无 BC7 设备走 RGBA32 prebuilt mip 路径且不崩溃。

### Main Sponza 1024 验证记录

2026-07-19 的 Debug 验证生成了 72 个实际使用的 KTX2 blob，第二次资产构建全部复用。运行时命中结果为 `72/72`，全部转码为 BC7 并使用预生成 mip，`textureDecodes=0`、`resizedTextures=0`。

- 无缓存回退：总加载 `81.57 s`，纹理 GPU 估算 `384.00 MiB`，VMA allocation delta `567.71 MiB`。
- KTX2/BC7 命中：总加载 `7.20 s`，纹理 GPU 估算 `96.00 MiB`，VMA allocation delta `279.74 MiB`。
- 加载期间 Runtime Control `ping` 能立即返回；BC7 路径使用 3 个增量 batch，没有 legacy queue wait。
- Windows GPU process counter 在加载完成后记录约 `592.82 MiB` dedicated usage；它包含驱动和非 VMA 开销，不能与 VMA allocation 直接等同。
- 最终 sRGB、normal、MR、AO、alpha、emissive 画面验收仍需人工完成。

## Stage 4: Residency And Streaming, Conditional

只有阶段 3 后仍存在超预算场景或需要无缝大世界切换时才实施本阶段。

候选能力：

- 启用 `VK_EXT_memory_budget`，记录 heap budget/usage，并建立加载 admission policy。
- 纹理 LRU residency、按需 mip streaming 和逐级提升清晰度。
- Mesh LOD、可见性驱动加载和后台 eviction。
- 稳定 descriptor indirection；资源不驻留时绑定 fallback。
- 需要大量动态纹理时再评估 descriptor indexing/bindless。

进入条件必须由真实统计驱动，不能仅因为实现看起来更完整。若压缩纹理和 2048/1024 档位已经满足目标，则不应提前承担 streaming 的同步、descriptor 和回收复杂度。

## Diagnostics Changes

后续阶段在现有 SceneLoadStats 上增加：

- `taskId`、generation、最终状态和取消原因。
- worker queue wait、CPU prepare wall time 与各子阶段 CPU time。
- prepared CPU bytes 和 peak prepared memory。
- 每帧 upload bytes、pump time、submitted/completed batch、in-flight peak。
- time to responsive、time to first upload、time to publish。
- 旧 Scene 与新 build 是否重叠驻留，以及释放旧 Scene 的时间点。
- KTX2 cache hit/miss、transcode format/time 和 compressed GPU bytes。

总 wall time 与并行子阶段不再要求互斥相加。Stats UI 和 JSON 必须明确 wall time、CPU accumulated time 与 GPU waiting time 的不同含义。

## Verification Matrix

每个阶段至少覆盖：

- Viking Room、Sheen Chair、CarConcept、ChronographWatch、AnisotropyBarnLamp 和 Main Sponza。
- Texture Limit `2048` 与 `1024`；Main Sponza 不强制测试 Full。
- Debug 和 Release 构建。
- scene/shader/texture limit 切换、resize、退出和加载失败。
- Vulkan validation、VMA allocation 前后差值和 Windows 专用显存曲线。
- baseColor、normal、AO、alpha、transmission、double-sided 和 mipmap 视觉回归。

重点压力场景：

1. 加载 Main Sponza 后立即取消。
2. Main Sponza CPU prepare 中连续切换三个小场景。
3. Uploading 中关闭窗口或执行 `VulkanLabCtl quit`。
4. 外部图片缺失、损坏、超大或解码失败。
5. GPU allocation 失败和 descriptor allocation 失败。
6. 加载完成恰逢 SwapChain resize/recreate。

## Delivery And Commit Strategy

每个阶段独立合并，不把 2A、2B 和 2C 堆在一个不可验证的大提交中：

1. CPU prepared data types 与无 Vulkan 单元测试。
2. GltfPreparer 与同步 GPU adapter。
3. SceneLoadManager、worker 和 loading UI。
4. 非阻塞 batch slots 与独立测试。
5. SceneGpuBuilder 和逐帧 pump。
6. publish、failure、cancellation 与 shutdown 生命周期。
7. Runtime Control v2 和自动化脚本。
8. KTX2 tool/cache/runtime support，作为单独阶段开发。

每个提交都应保持小场景可运行。涉及资源生命周期的提交必须先通过 validation，再进行 Main Sponza 手动显存与响应性检查。

## Assumptions And Decisions

- 工程继续使用 C++17，因此线程停止使用 atomic token、condition variable 和显式 join，不依赖 `std::jthread`。
- Vulkan、VMA、DescriptorAllocator 和 Scene 发布继续由主线程拥有。
- 阶段 2 使用 graphics queue，除非 profile 证明 queue contention 是主要瓶颈。
- 初始每帧上传预算是可调试常量，不立即加入普通用户 UI；统计稳定后再决定是否配置化。
- CPU prepare 可以占用较多内存，但必须记录 peak，并在 GPU build 消费后逐步释放图片和 Mesh 临时数据。
- Main Sponza 2048 是首要基线，1024 用于内存对照；Full 仍是显式高风险选项。
