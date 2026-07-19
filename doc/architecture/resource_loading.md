# 资源加载

> Status: Current
> Last verified: 2026-07-19
> Verified against: `fa30693`

## 项目、Catalog 与导入

`ProjectContextResolver` 先检查可执行文件旁的 `package_manifest.json`。存在时完整校验 package 并返回只读 package root、Catalog、cache root 和 profile，禁止 `--project` 覆盖。否则按开发规则优先使用 `--project <path>`，再读取 CMake 生成的 `vulkanlab_project.json`；Debug、Release 和资产工具因此指向同一个源码 `assets/catalog.json`。Catalog 使用稳定 `projectId`、scene `id` 和 import profile ID，具体 glTF 场景不再硬编码在 `main.cpp`。

Scenes 面板的 `Import Scene...` 通过 Win32 `IFileOpenDialog` 选择 `.gltf/.glb`。可测试的 `SceneImportService` 在 worker 中执行以下事务：

1. 解析 glTF JSON（GLB 只读取 JSON chunk），收集本地 buffer/image URI。
2. 接受 data URI，拒绝远程 scheme、绝对路径、缺失文件、路径逃逸和 symlink 逃逸。
3. Copy 模式把主文件与依赖闭包写入 `models/imported/.staging-*`，保持 URI 相对结构并从 staging 二次 preflight。
4. 原子重命名为 `models/imported/<scene-id>/`。
5. 用临时文件验证新 Catalog，检查磁盘版本未变化，再以 atomic replace 发布。

任何步骤失败或取消都会删除本次 staging；目录已发布但 Catalog 未发布时会回滚新目录。项目内源文件也可选择 Reference Existing，只在 Catalog 中保存项目相对路径。`VulkanLabAssetTool catalog add` 调用相同服务，供自动化使用。

## 加载入口

SceneRegistry 的 `SceneEntry` 可以提供两种入口：

- `SceneFactory`：同步创建完整 GPU Scene，当前用于 Viking Room 和初始化兼容路径。
- `ScenePrepareFactory`：只产生 `PreparedSceneData`，当前用于全部 glTF 场景。

`PreparedSceneData` 保存纹理 payload、sampler/format 描述、材质参数、vertex/index、对象引用和相机建议，不包含 Vulkan handle、VMA allocation 或 descriptor set。纹理 payload 可以是运行时解码的 RGBA8 base level，也可以是 KTX2 转码后带完整 mip chain 的数据。主线程随后由 `SceneGpuBuilder` 把它转换为运行时 Scene。

## 异步 glTF Prepare

`SceneLoadManager` 持有一个 C++17 worker thread。新的 glTF 请求获得递增的 taskId 和 generation，并取消旧的 pending/active 请求。worker 调用 `GltfPreparer` 完成：

- 解析 `.gltf`/`.glb`、node hierarchy 和 world transform。
- 读取 position、normal、UV0、UV1、tangent、COLOR_0 和 index；缺少 tangent 时生成稳定 fallback。
- 转换 metallic-roughness、alpha、double-sided、normal scale、AO、emissive strength、transmission 和 volume 基础参数。
- 读取 embedded image 或 `.gltf` 相对路径下的本地图片，使用 stb 解码为 RGBA8。
- 按 `SceneLoadContext::maxTextureSize` 在 CPU 侧执行等比 bilinear 缩放。
- 生成 primitive、bounds、材质与对象的稳定索引关系。

worker 在文件、图片、材质、primitive 和 hierarchy 等自然边界检查 cancellation token。异常写入任务的 Failed 状态，不跨线程调用 Vulkan，也不向 Application 抛异常。

图片格式仍按语义选择：BaseColor/Emissive 使用 sRGB，Normal/MetallicRoughness/Occlusion 使用 linear。同一 glTF image 在不同语义下可以对应不同派生纹理，不能只按 image index 复用；sampler 映射 repeat、clamp、mirrored repeat、min/mag filter 和 mipmap mode。

## KTX2 派生资产

阶段三在原始 glTF 与运行时 loader 之间增加只读派生缓存。离线 `VulkanLabAssetTool texture-cache build` 扫描材质实际引用的图片，用 KTX-Software 生成 UASTC + Zstd KTX2 和完整 mip chain。源 glTF、GLB 和图片保持不变。

资产工具把一次构建分为 scan、schedule/worker 和 publish：scan 解析 glTF、读取源 hash、计算输出尺寸/cache key，并验证可复用 blob；未命中任务进入确定性调度器。默认 worker 数为 `min(4, max(1, logicalCpuCount / 2))`，同时受默认 `2048 MiB` 估算工作集预算约束。单任务超过预算时可以独占执行，多个任务不能绕过总预算。worker 只负责一张纹理的临时输入、`ktx create` 子进程、KTX2 验证和内容寻址 blob 原子发布。

Windows 下所有 `ktx.exe` 都由独立 Job Object 管理，并使用 `KILL_ON_JOB_CLOSE`。每个子进程只继承自己的输出管道；Ctrl+C、首个任务失败或资产工具退出会停止分发、终止完整子进程树并清理临时输入/输出。已经验证并原子发布的 blob 可以保留供重试复用，但只有全部任务成功后才通过 atomic replace 发布 scene manifest，因此取消和失败不会覆盖已有有效 manifest。

`development` preset 保留阶段三已有的 quality 2、RDO、Zstd 9 参数和 cache key，以继续复用现有缓存；`production` 使用 quality 4、RDO 和 Zstd 18，并产生不同 key。preset 名称和完整 encoder settings 写入 manifest。使用 `--progress ndjson` 时 stdout 只输出带 `protocolVersion`、task ID、artifact 状态、计数、耗时、峰值 worker 和估算保留字节的 UTF-8 NDJSON；子进程输出和普通错误写入 stderr。

默认缓存根目录为 `%LOCALAPPDATA%/VulkanLab/DerivedAssets/<projectId>`，Debug、Release 和资产工具共享：

```text
DerivedAssets/<projectId>/
  manifests/<scene-id>/<profile-id>.json
  blobs/<content-cache-key>.ktx2
  artifact_index.json
```

manifest schema v2 记录 project/scene/profile 稳定身份、scene、texture limit、源文件 size/write time/SHA-256，以及每个 image 的语义、mipmap wrap、输出尺寸、cache key 和 blob。schema v1 仍可读取用于显式 migration，但运行时只查询 v2 稳定路径。`TextureSemantic` 固定为 `SrgbColor`、`LinearData` 和 `Normal`；BaseColor/Emissive 使用 `SrgbColor`，MetallicRoughness/Occlusion 使用 `LinearData`，normal map 使用 `Normal`。同一图片以不同语义或 wrap 参与材质时必须生成独立条目。

运行时按 scene ID 和实际 texture limit 对应的 profile ID 查找精确 manifest，并用 size + write time 快速检查依赖。命中后由 worker 使用 libktx 读取 KTX2：支持 BC7 的设备转码到 BC7 SRGB/UNORM，不支持 BC7 时转为 RGBA32。两条路径都保留离线 mip chain，不执行 stb decode、bilinear resize 或 GPU blit mip generation。

`PreparedImage` 的 prebuilt mip payload 包含总字节数组、最终 `VkFormat` 和每级 mip 的 offset、size、width、height。`SceneGpuBuilder` 将所有 mip 写入 staging，并通过多个 `VkBufferImageCopy` region 上传到只需要 `TRANSFER_DST | SAMPLED` 的 image。原始 RGBA8 fallback 继续上传 base level，并由 GPU blit 生成 mip。

开发模式下，glTF prepare 遇到单个派生条目缺失或 KTX2 读取/转码失败时可记录 miss 并走 RGBA fallback；普通 scene load 之前仍会先执行 scene/profile 级 artifact admission。CookedOnly 设置 `SceneLoadContext::requireDerivedTextures`，manifest 身份、scene stamp、entry/source stamp、KTX2 读取、转码和 mip offset 任一失败都会终止加载，不会调用 stb 解码。

## Artifact Index 与依赖失效

`ArtifactIndex` 是 `%LOCALAPPDATA%` 共享 cache 中的可丢弃 JSON 加速层，不是 artifact 的事实来源。每条 scene/profile 记录保存 manifest identity、source dependency 的 size/mtime/SHA-256、blob 闭包、schema/encoder settings、最后成功 import task、访问时间和最近失败诊断。索引缺失、JSON 损坏、schema 或 project ID 不匹配时，会从 Catalog 和已发布 manifest 自动重建并原子替换。

状态查询分两级：

1. `Fast` 用于启动、Scenes/Assets 列表和普通 Runtime Control 查询。size 与 mtime 未变化时不读取文件内容；stamp 可疑时才计算共享 BCrypt SHA-256。内容 hash 相同会更新 stamp 并继续 Ready，内容变化只使引用该 dependency 的 scene/profile 返回 Stale。
2. `Admission` 在真正 scene load 前额外验证每个 blob 的文件大小和 KTX2 identifier。损坏或缺失 blob 返回 Invalid，不能进入正常 cache load。

资产工具只有在 manifest 成功发布后，才在同一个 cache mutation transaction 中刷新索引。Application 重新载入工具发布的记录，并单独合并访问时间、成功 task 和失败日志。索引写入使用短时命名 mutex、临时文件和 `MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH)`；不同进程只合并脏记录，旧 UI 遥测不会覆盖 importer 的新 manifest 记录。

所有会修改 manifest/blob 的 import、migration、index rebuild 和 prune execute 共享按 cache root 命名的 Windows mutex。`cache prune` 先从全部可读 manifest 建立保护闭包；默认只 dry-run，`--execute` 获得 mutation lock 后重新计算候选，只删除超过保留期且没有任何 manifest 引用的 `.ktx2`。损坏 manifest 或非法 blob 引用会拒绝清理，不会猜测引用关系。

当前没有接入 directory watcher。现有 UI 不逐帧查询索引或扫描目录，加载 admission 又会确定性复核依赖；在当前数十个 scene/profile 规模下 watcher 没有可测收益。后续需要检测渲染器外部的即时文件变化时，可以只将受影响记录标为 PossiblyStale，不能在回调中直接编码。

## 自动按需导入

Application 使用 ArtifactIndex 的 Fast 查询展示精确 scene/profile 的 `Ready`、`Missing`、`Stale` 或 `Invalid`，在提交 glTF `SceneLoadManager` 前执行 Admission 查询。索引不可用时保留 `inspectTextureArtifacts()` 完整扫描作为兼容回退。正在执行的同一 scene/profile task 在 UI 中投影为 `Importing`。

运行模式是显式 Config/启动参数，不依赖构建类型：

- `OnDemand`：非 Ready 时向 `AssetImportManager` 提交 import，然后继续渲染当前 Scene。
- `ReadOnly`：不创建编码进程；非 Ready 加载返回错误，但允许用户显式 source fallback。
- `CookedOnly`：不创建编码进程、禁止 source fallback，并要求包内派生 artifact 完整有效。

AssetImportManager 拥有一个 supervisor thread、任务队列和最多 64 条历史。相同 scene/profile 的未完成请求合并；不同 profile 保持独立。supervisor 通过精确继承的 stdout/stderr pipe 启动同目录 `VulkanLabAssetTool import scene`，stdout 只接受最大 64 KiB、protocol v1 的 NDJSON，stderr 写入每任务日志。工具进程与全部 `ktx.exe` 位于 `KILL_ON_JOB_CLOSE` Job Object；取消、非法协议、进程异常和 Application 退出都会终止完整进程树。AssetImportManager 不解析 glTF、不访问 Scene/Vulkan，工具内部并发继续由 Stage B worker 与内存预算控制。

资产 task 使用最高位为 1 的 operation ID，与 SceneLoadManager 的低位 task ID 隔离。`AssetLoadCoordinator` 为每次 scene 请求分配 generation：import 完成后可以保留有效缓存，但只有全局最新 generation 的 consumer 能继续提交 CPU prepare。import 失败或取消保留当前 Scene，不自动回退；显式 `Load Source Fallback` 使用不会命中 manifest 的专用 profile ID。

Stage A 的文件导入事务完成 Catalog 注册后会立即提交派生资产 import。勾选 `Load scene after import` 时，同一个 operation 连续覆盖 Catalog registration 后的 KTX2 import、CPU prepare、GPU upload 和 Scene publish。Scenes 面板只承担选择/命令，Assets 面板显示 artifact 状态、真实编码进度、worker、日志、错误和历史。

## 增量 GPU Build

CPU prepare 完成后，Application 在开始 GPU build 前执行一次明确的 teardown 边界：

1. `vkDeviceWaitIdle()`，确保旧 Scene 不再被帧命令引用。
2. 清空 PipelineCache，释放旧 Scene，避免两个 Main Sponza 级场景重叠占用显存。
3. 记录 VMA before 快照，创建 SceneGpuBuilder。

SceneGpuBuilder 每帧以默认 `32 MiB` 和 `2 ms` 软预算推进：fallback、Texture、Mesh、等待 GPU、Material、SceneObject 和 publish。单个不可拆分 Texture/Mesh 可以超过软预算，但不会因大于预算而饥饿。

`IncrementalUploadQueue` 使用 graphics queue 和两个延迟创建的 slot。每个 slot 拥有独立的 128 MiB staging、command pool/buffer 和 fence：

- copy、mipmap blit、barrier 记录到当前批次。
- 当前 slot 放不下时提交，并继续使用另一个空闲 slot。
- 普通帧只用 `vkGetFenceStatus()` 轮询，不调用无限期 `vkWaitForFences()` 或 `vkQueueWaitIdle()`。
- fence 完成前不覆盖 staging，也不销毁关联的半成品 Texture/Mesh。
- 全部上传完成后才创建材质 descriptor 并把 Scene 发布到 RenderQueue。

取消或 GPU build 失败会停止记录新资源，提交已经记录的命令，并逐帧轮询在途 fence。相关 fence 完成后才销毁半成品；正常取消路径不使用 device idle。程序退出和显式 teardown 可以 drain。

DescriptorAllocator 创建支持单独释放 set 的 pool。MaterialInstance 析构会归还 descriptor set，因此失败、取消和反复重载不会持续耗尽 pool 容量。

## 发布与失败语义

- CPU prepare 期间继续渲染当前 Scene。
- 开始 GPU build 后旧 Scene 已释放，期间渲染空场景和 ImGui Loading 面板。
- 只有最新 generation 且所有 upload fence 完成的任务可以发布。
- CPU prepare 失败时保留当前 Scene；旧 Scene 已释放后的 GPU build 失败会保留可操作的空场景。
- 同步 Viking Room 切换会先取消后台任务，旧 generation 不能在稍后覆盖同步场景。
- Application 关闭前先停止控制服务和 source import future，再取消并 join AssetImportManager/完整工具进程树，然后取消 builder、join SceneLoadManager，最后销毁 Vulkan 对象。

## LoadStats

日志、`Stats -> Last Scene Load` 和 Runtime Control JSON 会记录：

- taskId、generation、最终状态、worker queue、CPU prepare、GPU build 和总 wall time。
- glTF parse、图片读取/解码/缩放、材质、mesh CPU、hierarchy 和 command recording 耗时。
- prepared CPU bytes、纹理/mesh/vertex/index/material/object 数量及上传字节。
- 每帧 upload pump 的最大耗时/字节、batch submit/completion、fence poll/wait 和 peak in-flight。
- KTX2 cache lookup/hit/miss/invalid、读取字节与耗时、transcode 耗时、BC7/RGBA32 fallback 数量和 prebuilt mip 数量。
- 场景资源创建前后的 VMA allocation count、allocation bytes 与 block bytes。

VMA 快照在 SceneGpuBuilder 和 staging 销毁后采集，不把临时 staging 计入场景常驻差值。VMA 数值不等同于 Windows 任务管理器显示的专用显存。

## Cook 与 packaged runtime

`VulkanLabAssetTool cook` 将开发项目和共享 cache 转换成只读运行目录。输入是 ProjectContext、一个精确 profile、选中的 scene ID、已构建 runtime 目录和输出目录。默认选择非 optional scenes；显式 scene ID 可以形成更小的产品包。缺失 artifact 默认直接失败，`--build-missing` 才会在 cook 前调用现有有界并行 importer。

`CookPackageBuilder` 建立以下引用闭包：

1. `VulkanLab.exe`、可选 `VulkanLabCtl.exe` 和 `kShaderVariants` 实际引用的唯一 SPIR-V。
2. 只含选中场景和单一 profile 的 cooked Catalog。
3. glTF/GLB 主文件；外部 `.gltf` 只复制 buffer URI，不复制源图片。
4. 每个 glTF scene/profile 的 manifest 及其内容寻址 KTX2 blob 去重集合。
5. 从 cooked Catalog、manifests 和 blobs 重建的 package-local ArtifactIndex。

Cooked manifest 将 source stamp 改写为包内 scene stamp。源图片因此不属于运行闭包；blob 内容由 package manifest SHA-256 保护，运行时 KTX2 loader 再验证结构和转码。Main Sponza 包不会包含 PNG/JPEG、FBX、USD、MAX 或未登记 shader。

所有文件先写入输出目录旁的唯一 staging。生成 sorted schema v1 `package_manifest.json` 后，工具验证每个文件的规范化相对路径、大小和 SHA-256，再通过目录 rename 发布。替换已有包时先保留 sibling backup；构建或验证失败时移除 staging，原包保持可验证。`package verify` 和运行时启动调用同一 verifier。

packaged `main()` 在 Vulkan 初始化前强制以下契约：

- Package project/profile 必须与 cooked Catalog 一致。
- cache root 固定为包内 `runtime_assets`，asset mode 固定为 CookedOnly。
- 不创建 AssetImportManager，不写 package ArtifactIndex，也不允许 source fallback、外部 cache/tool/project override 或纹理 profile 切换。
- 关闭 validation layer，避免 Release 包依赖 Vulkan SDK 开发层。

开发 CMake 的 `POST_BUILD` 仍复制完整 `models/`，用于 IDE 运行和源 fallback；它不再是交付策略。正式运行目录由 `cook` 决定。

## Platform artifact 与 residency 决策

Stage F 当前推迟。Release cooked Main Sponza 1024 的 72 张 KTX2 全部转为 BC7，总加载约 `1.36 s`，转码约 `0.79 s`，纹理 GPU estimate `96 MiB`，场景 VMA allocation delta `279.74 MiB`。现有场景替换会在新 GPU build 前释放旧 Scene，没有多场景 residency 累积，当前数据不足以支持 platform-final BC payload、streaming 或 LRU 的复杂度。

重新评估条件为：代表场景 Release p95 超过 2 秒且转码占 CPU prepare 超过 40%；目标设备 allocation failure 或单 Scene 超过 device-local budget 的 50%；需要同时驻留多个大场景或 resident set 超过 budget 的 70%；或发布体积成为明确产品约束。memory gate 需要先接入 `VK_EXT_memory_budget`，不能把 VMA block bytes 当作可用预算。

## 当前限制

- CPU prepare 只有一个 worker；不同场景不会并行解码。
- AssetImportManager 当前一次只监督一个资产工具；单个工具内部可以并行编码。没有跨工具并发、持久任务数据库或断点任务队列。
- ArtifactIndex 当前使用 JSON 和命名 mutex；没有 directory watcher 或跨机器共享数据库。索引可从 manifest 重建。
- 单个大 Texture/Mesh 仍是原子 pump，可能造成短帧尖峰。
- 使用 graphics queue，没有专用 transfer queue 或 queue ownership transfer。
- 开发模式中未生成或未命中 KTX2 profile 的图片仍可能运行时 decode/resize，并以 RGBA8 上传；CookedOnly 禁止该路径。
- KTX2 v1 统一使用 BC7，不使用 BC5 normal 或 BC4 AO；无 BC7 设备使用 RGBA32，因此不会获得压缩显存收益。
- 没有运行时自动编码、mip streaming、LRU residency、virtual texturing 或加载时保留旧大场景的显存预算策略。
- Viking Room 仍使用同步 OBJ/PNG factory；开发 CMake 仍复制整个 `models/` 目录，正式包使用 Cook 闭包。
