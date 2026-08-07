# 资源加载

> Status: Current
> Last verified: 2026-08-07
> Verified against: Shared Model/Environment repositories and Local Reflection Probes v1

## 项目、Catalog 与导入

`ProjectContextResolver` 先检查可执行文件旁的 `package_manifest.json`。存在时完整校验 package 并返回只读 package root、Catalog、cache root 和 profile，禁止 `--project` 覆盖。否则按开发规则优先使用 `--project <path>`，再读取可执行文件旁由 CMake 生成的 `vulkanlab_project.json`，最后才从当前目录祖先查找 Catalog。解析结果明确区分 `projectRoot`、`runtimeRoot`、`cacheRoot` 和 `captureRoot`；后续 subsystem 只消费已经解析的路径，不再按当前工作目录拼接资源。Debug、Release 和资产工具因此指向同一个源码 `assets/catalog.json`。Catalog schema v3 将 `models[]`、原生 `scenes[]` 和 `environments[]` 分开；schema v1/v2 的旧 `scenes[]` 继续按模型预览读取，且只读启动不会改写旧文件。具体模型和环境不再硬编码在 `main.cpp`。

开发模式下 Catalog model、Native SceneDocument、glTF/GLB、外部 buffer/image 以及 Viking Room 的 OBJ/PNG 从 `projectRoot` 读取，SPIR-V 和 sibling 工具从 executable 所在的 `runtimeRoot` 读取。Cooked package 中 `projectRoot` 与 `runtimeRoot` 都是 package root，`cacheRoot` 固定为包内 `runtime_assets`。Native `.vkscene.json` 由 `SceneLoadManager` 解析，再通过 `AssetRepository` 和 Environment loader 事务性构造 `RuntimeWorld`。

`VulkanLab -> Scene -> Scenes` 的 `Import Model...` 通过 Win32 `IFileOpenDialog` 选择 `.gltf/.glb`。新源文件先由 `ModelImportService::preflight` 检查本地 URI 与依赖闭包，再由独立 `VulkanLabAssetTool validate scene` 进程运行固定版本 Khronos glTF Validator。只有 `Valid/Warnings` 才能继续；`Unavailable` 需要用户显式勾选未校验导入；`Invalid/Stale/Failed/NotChecked` 都会阻止复制、Catalog 写入和后续 BC7 构建。

验证报告位于 `<cacheRoot>/validation/reports/2.0.0-dev.3.10/`，`validation/index.json` 继续保存兼容字段 `sceneId`、项目相对 source 和内容寻址 report key；在模型路径中该值就是 `modelId`，以避免旧报告和缓存失效。报告记录 source SHA-256、依赖 size/mtime、完整 issues、资产统计和 renderer extension 兼容性，不写入 Catalog、源模型或 Cooked package。已有 Catalog 模型不在启动时扫描，可从 Scenes 页执行 `Validate/Revalidate`；source 或依赖 stamp 变化后查询结果为 `Stale`。

验证通过后，`ModelImportService` 在 worker 中执行以下事务：

1. 解析 glTF JSON（GLB 只读取 JSON chunk），收集本地 buffer/image URI。
2. 接受 data URI，拒绝远程 scheme、绝对路径、缺失文件、路径逃逸和 symlink 逃逸。
3. 重新读取缓存 validation receipt，并用当前 source/dependency stamp 防止 UI 与 worker 之间的 TOCTOU。
4. Copy 模式把主文件与依赖闭包写入 `models/imported/.staging-*`，保持 URI 相对结构并从 staging 二次 preflight。
5. 原子重命名为 `models/imported/<model-id>/`，将验证报告绑定到最终 model ID。
6. 由 `SceneCatalogStore` 用临时文件验证 schema v3 Catalog，检查磁盘版本未变化，再以 atomic replace 发布。

任何步骤失败或取消都会删除本次 staging；目录已发布但 Catalog 未发布时会回滚新目录。项目内源文件也可选择 Reference Existing，只在 Catalog 中保存项目相对路径。`VulkanLabAssetTool catalog add` 调用相同验证门和导入服务，供自动化使用。Validator warning 与 renderer capability warning 分开保存；后者不阻止开发导入或 Cook。

HDR 环境导入只接受本地 2:1 equirectangular `.hdr`。UI 或 `VulkanLabAssetTool catalog add-environment` 将源文件复制到 `assets/environments/<environment-id>/`，再原子更新 Catalog。已存在的 ID 或目标路径不会被覆盖；v1 不接受 EXR、远程 URI 或非 2:1 HDR。

## 加载入口

`SceneWorkflowController` 从 Catalog 的 `models[]` 构建 SceneRegistry；原生 `scenes[]` 当前被有意忽略，直到 RuntimeWorld 阶段。SceneRegistry 的 `SceneEntry` 可以提供两种入口：

- `SceneFactory`：同步创建完整 GPU Scene，当前用于 Viking Room 和初始化兼容路径。
- `ModelPrepareFactory`：只产生 `PreparedModelData`，当前用于全部 glTF 模型；`ScenePrepareFactory` 暂时保留为兼容别名。

`PreparedModelData` 保存纹理 payload、sampler/format 描述、材质参数、vertex/index、`localToAsset` primitive、asset-space 灯光和预览相机，不包含 Vulkan handle、VMA allocation 或 descriptor set。纹理 payload 可以是运行时解码的 RGBA8 base level，也可以是 KTX2 读取后带完整 mip chain 的数据。主线程随后由 `ModelGpuBuilder` 把它转换为不可变、可共享的 `ModelAsset`。

Catalog v3 与 `.vkscene.json` 的数据契约见[场景数据与 Catalog](scene_documents.md)。

## 共享 ModelAsset 加载

`AssetRepository` 按 `(modelId, profileId)` 管理 ModelAsset generation，并持有一个 FIFO CPU worker 和最多一个活动 `ModelGpuBuilder`。相同 key 的加载请求共享 CPU prepare 和 GPU build；Ready 请求直接复用同一资源。`Reload` 创建新 generation，旧 generation 在已有 Scene lease 释放前保持有效。`SceneLoadManager` 不拥有模型 GPU builder；它保留模型预览操作兼容接口，并使用独立单 worker 解析 Native SceneDocument。

Native Scene 加载分为 `parsingDocument -> resolvingModels -> loadingModels -> loadingEnvironment -> publishingWorld`。文档解析后，主线程按实体顺序去重 model ID，并使用各 `CatalogModel.importProfile` 请求 Repository。所有 ModelAsset 和目标 environment Ready 后才创建 `RuntimeWorld`；任一依赖失败、任务取消或发布前出现新的未保存编辑都会终止事务并保留当前 World。

Repository worker 调用 `GltfPreparer` 完成：

- 解析 `.gltf`/`.glb`、node hierarchy 和 world transform。
- 读取 position、normal、UV0、UV1、tangent、COLOR_0 和 index；缺少 tangent 时生成稳定 fallback。
- 读取 `KHR_lights_punctual` 定义，并按模型 node 引用生成 asset-space Directional、Point 和 Spot prototype。
- 转换 metallic-roughness、alpha、double-sided、normal scale、AO、emissive strength、transmission 和 volume 基础参数。
- 读取 embedded image 或 `.gltf` 相对路径下的本地图片，使用 stb 解码为 RGBA8。
- 按 `SceneLoadContext::maxTextureSize` 在 CPU 侧执行等比 bilinear 缩放。
- 生成 primitive、bounds、材质与对象的稳定索引关系。

worker 在文件、图片、材质、primitive 和 hierarchy 等自然边界检查 cancellation token。异常写入任务的 Failed 状态，不跨线程调用 Vulkan，也不向 Application 抛异常。

glTF 灯光与 mesh 使用同一套 node hierarchy 和 Y-up 到 Z-up 根变换。Point/Spot 的位置来自 node asset-space translation；Directional/Spot 的发射方向来自 node 局部 `-Z`。同一个 glTF light definition 被多个 node 引用时会生成多个 prototype。`Scene` 实例化模型时再通过 `rootToWorld` 转换位置和方向；range、intensity 与 cone 不随实例 scale 改变。颜色保持线性，Directional intensity 保持 lux，Point/Spot intensity 保持 candela。

`ModelGpuBuilder` 在主线程按帧预算创建 Texture、Mesh、Material 和 descriptor，完成后发布 `shared_ptr<const ModelAsset>`。Repository 只使用 graphics queue 的增量上传 fence，不在 glTF 切换中调用 `vkDeviceWaitIdle()` 或清空 Pipeline Cache。预览仍暴露旧 `Scene` facade，但内部只放置一个 identity `ModelInstance`；RenderCommand 的矩阵为 `rootToWorld * localToAsset`。被替换的 Scene 和无消费者 ModelAsset 都按 FrameSync submission serial 延迟释放。

引擎基础几何使用同一条 Repository 链路，但 CPU prepare 来自 `PrimitiveMeshGenerator`，不读取源文件或派生缓存。Plane、Cube、Sphere、Cylinder、Cone 和 Capsule 分别使用保留 model ID 与固定 `engine-primitive-v1` profile；同类型实例只生成和上传一份 `ModelAsset`。Cook 将它们视为零文件依赖，不创建 Validator、BC7 或 Artifact Index Model record。

图片格式仍按语义选择：BaseColor/Emissive 使用 sRGB，Normal/MetallicRoughness/Occlusion 使用 linear。同一 glTF image 在不同语义下可以对应不同派生纹理，不能只按 image index 复用；sampler 映射 repeat、clamp、mirrored repeat、min/mag filter 和 mipmap mode。

## KTX2 派生资产

原始 glTF 与运行时 loader 之间有一层只读派生缓存。离线 `VulkanLabAssetTool texture-cache build` 扫描材质实际引用的图片，生成带完整 mip chain 的 KTX2。当前 Windows desktop profiles 默认生成原生 BC7；源 glTF、GLB 和图片保持不变。

资产工具把一次构建分为 scan、schedule/worker 和 publish：scan 解析 glTF、读取源 hash、计算输出尺寸/cache key，并验证可复用 blob；未命中任务进入确定性调度器。默认 worker 数为 `min(4, max(1, logicalCpuCount / 2))`，同时受默认 `2048 MiB` 估算工作集预算约束。单任务超过预算时可以独占执行，多个任务不能绕过总预算。每个 worker 先调用 KTX-Software 完成源图片解码、尺寸限制、Lanczos4 mipmap、wrap 边缘和 normal normalize，得到临时 RGBA8 KTX2；随后使用 DirectXTex 将每级 mip 压缩为 BC7，并通过 libktx 直接写入原生 BC7 KTX2。

Windows 下所有 `ktx.exe` 都由独立 Job Object 管理，并使用 `KILL_ON_JOB_CLOSE`。每个子进程只继承自己的输出管道；Ctrl+C、首个任务失败或资产工具退出会停止分发、终止完整子进程树并清理临时输入/输出。已经验证并原子发布的 blob 可以保留供重试复用，但只有全部任务成功后才通过 atomic replace 发布 scene manifest，因此取消和失败不会覆盖已有有效 manifest。

`development` preset 使用 DirectXTex BC7 quick 模式，`production` 使用完整 BC7 搜索；两者产生不同 cache key。Native BC7 v1 不使用 Zstd supercompression，优先降低运行时 CPU 成本。纹理级并行由现有 worker 调度器控制，DirectXTex 内部并行关闭，避免线程过度订阅。preset、编码器版本和完整 settings 都写入 manifest。使用 `--progress ndjson` 时 stdout 只输出带 `protocolVersion`、task ID、artifact 状态、计数、耗时、峰值 worker 和估算保留字节的 UTF-8 NDJSON；子进程输出和普通错误写入 stderr。

默认缓存根目录为 `%LOCALAPPDATA%/VulkanLab/DerivedAssets/<projectId>`，Debug、Release 和资产工具共享：

```text
DerivedAssets/<projectId>/
  manifests/<scene-id>/<profile-id>.json
  manifests/environments/<environment-id>/<profile-id>.json
  blobs/<content-cache-key>.ktx2
  artifact_index.json
```

manifest schema v3 记录 project/scene/profile 稳定身份、scene、texture limit、编码器名称/版本/质量设置，以及每个 image 的 payload kind、Vulkan format、mip 数量、GPU payload/blob 字节、supercompression、语义、mipmap wrap、输出尺寸、cache key 和源 stamp。schema v1/v2 UASTC manifest 仍可读取；当当前 profile 要求 BC7 时，它们被标记为 `Stale`，不能作为 Native BC7 Ready artifact。`TextureSemantic` 固定为 `SrgbColor`、`LinearData` 和 `Normal`；BaseColor/Emissive 生成 `VK_FORMAT_BC7_SRGB_BLOCK`，MetallicRoughness/Occlusion/Normal 生成 `VK_FORMAT_BC7_UNORM_BLOCK`。同一图片以不同语义或 wrap 参与材质时必须生成独立条目。

运行时按 model ID 和实际 texture limit 对应的 profile ID 查找精确 manifest，并用 size + write time 快速检查依赖。序列化兼容字段仍命名为 `sceneId`，其值继续传 model ID，避免现有缓存失效。Native BC7 命中后，worker 只用 libktx 读取和校验 mip payload，不调用 `ktxTexture2_TranscodeBasis()`；`ModelGpuBuilder` 直接上传 BC7 mip chain。旧 schema v1/v2 UASTC 缓存仍保留 BC7/RGBA32 转码兼容路径。开发设备不支持 BC7 时，Native BC7 条目记录 `native_bc7_unsupported` 并回退源图片；Cooked package 在加载模型前返回 `bc7_required`，不会引入 DirectXTex 或静默回退。

`PreparedImage` 的 prebuilt mip payload 包含总字节数组、最终 `VkFormat` 和每级 mip 的 offset、size、width、height。`ModelGpuBuilder` 将所有 mip 写入 staging，并通过多个 `VkBufferImageCopy` region 上传到只需要 `TRANSFER_DST | SAMPLED` 的 image。原始 RGBA8 fallback 继续上传 base level，并由 GPU blit 生成 mip。

开发模式下，glTF prepare 遇到单个派生条目缺失、KTX2 读取/校验失败或 Native BC7 不受支持时可记录 miss 并走 RGBA fallback；普通 scene load 之前仍会先执行 scene/profile 级 artifact admission。CookedOnly 设置 `SceneLoadContext::requireDerivedTextures`，并要求 scene texture manifest 全部为 Native BC7；manifest 身份、scene stamp、entry/source stamp、format、payload size、mip offset 任一不匹配都会终止加载，不会调用 stb 解码。

## 环境派生资产与异步加载

`VulkanLabAssetTool environment-cache build` 在离线阶段读取 Catalog environment/profile，并使用 `stbi_loadf` 解码线性 RGB HDR。输入必须为 2:1，NaN/Inf 会清零。CPU Baker 使用固定 Hammersley 序列和右手 Z-up 坐标约定，cubemap face 顺序为 `+X/-X/+Y/-Y/+Z/-Z`，确定性生成：

- Radiance：`RGBA16F` cubemap，普通 mip chain 使用跨 face 的 Lanczos4 filter。
- Irradiance：`RGBA16F` cubemap，cosine-weighted convolution，结果已经除以 π。
- Prefiltered Specular：`RGBA16F` cubemap，GGX importance sampling，每级 mip 对应 roughness。
- BRDF LUT：`RG16F` 2D texture，split-sum integration。

默认 `ibl_desktop_v1` 的尺寸为 512/32/256/256，样本数为 diffuse 1024、specular 512、BRDF 1024。四个原生浮点 KTX2 使用 Zstd 9，不转为 BC7。cache key 包含源 SHA-256、profile、算法版本、格式、尺寸、mip 和采样参数。每个输出先写临时文件并重新验证；全部成功后才原子发布 `DerivedEnvironmentManifest`。有效 manifest/blob 命中时跳过 HDR decode、卷积和 KTX2 编码；`--force` 才重新计算。

运行时 `EnvironmentAssetRepository` 按 `(environmentId, profileId)` 管理共享 generation。单个 FIFO worker 读取并验证四个 KTX2，形成只含 CPU payload/subresource 描述的 `PreparedEnvironment`；主线程随后由唯一活动 `EnvironmentGpuBuilder` 使用现有 `IncrementalUploadQueue` 上传所有 face/mip。相同 key 的全局环境、多个 Reflection Probe 和并发请求会合并到同一 prepare/upload；Reload 创建新 generation，未使用的旧 generation 按 submission serial 延迟释放。设备必须支持 `RGBA16F` cubemap sampled + linear filtering 和 `RG16F` 2D sampled + linear filtering；不支持时环境功能不可用，但 Scene 渲染和 constant ambient 保持工作。

环境发布是事务性的：只有四张 GPU texture 和统一 Lighting descriptor generation 全部完成后，Renderer 才切换到新环境。失败或取消保留旧环境；旧 generation 按 `FrameSync` submission serial 延迟销毁，不调用 `vkDeviceWaitIdle()`。所有 binding 始终有合法 fallback texture，因此关闭 IBL、选择 `None` 或加载期间都不会产生 partially-bound descriptor。

## Artifact Index 与依赖失效

`ArtifactIndex` schema v2 是 `%LOCALAPPDATA%` 共享 cache 中的可丢弃 JSON 加速层，不是 artifact 的事实来源。每条记录用 `assetKind + assetId + profileId` 区分 Scene 和 Environment，保存 manifest identity、source dependency 的 size/mtime/SHA-256、blob 闭包、schema/encoder settings、最后成功 import task、访问时间和最近失败诊断。读取 schema v1 时旧记录按 Scene 解释。索引缺失、JSON 损坏、schema 或 project ID 不匹配时，会从 Catalog 和已发布 manifest 自动重建并原子替换。

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

任务 ID 使用不重叠的高位命名空间：Scene 使用低位，Environment 使用 bit 61，Capture 使用 bit 62，Asset Import 使用 bit 63。`AssetLoadCoordinator` 为每次 scene 请求分配 generation：import 完成后可以保留有效缓存，但只有全局最新 generation 的 consumer 能继续提交 CPU prepare。import 失败或取消保留当前 Scene，不自动回退；显式 `Load Source Fallback` 使用不会命中 manifest 的专用 profile ID。

Stage A 的文件导入事务完成 Catalog 注册后会立即提交派生资产 import。勾选 `Load scene after import` 时，同一个 operation 连续覆盖 Catalog registration 后的 KTX2 import、CPU prepare、GPU upload 和 Scene publish。`Scene -> Scenes` 只承担选择/命令和活跃场景加载详情，`Scene -> Assets` 显示 artifact 状态、真实编码进度、worker、日志、错误和历史。

## 增量 GPU Build

CPU prepare 完成后，Repository 将记录放入唯一的 GPU build FIFO。`ModelGpuBuilder` 每帧以默认 `32 MiB` 和 `2 ms` 软预算推进：fallback、Texture、Mesh、等待 GPU、Material、ModelAsset finalize。单个不可拆分 Texture/Mesh 可以超过软预算，但不会因大于预算而饥饿。整个过程继续渲染当前 Scene，不执行 scene teardown。

`IncrementalUploadQueue` 使用 graphics queue 和两个延迟创建的 slot。每个 slot 拥有独立的 128 MiB staging、command pool/buffer 和 fence：

- copy、mipmap blit、barrier 记录到当前批次。
- 当前 slot 放不下时提交，并继续使用另一个空闲 slot。
- 普通帧只用 `vkGetFenceStatus()` 轮询，不调用无限期 `vkWaitForFences()` 或 `vkQueueWaitIdle()`。
- fence 完成前不覆盖 staging，也不销毁关联的半成品 Texture/Mesh。
- 全部上传完成后才创建材质 descriptor 并发布不可变 ModelAsset。

取消或 GPU build 失败会停止记录新资源，提交已经记录的命令，并逐帧轮询在途 fence。相关 fence 完成后才销毁半成品；正常取消路径不使用 device idle。程序退出和显式 teardown 可以 drain。

DescriptorAllocator 创建支持单独释放 set 的 pool。MaterialInstance 析构会归还 descriptor set，因此失败、取消和反复重载不会持续耗尽 pool 容量。

`RenderResourceRegistry` 不管理这里的 ModelAsset Texture/Mesh 或 Environment Texture，也不参与上传或 residency。它只拥有 Renderer 内部的 HDR、depth、shadow、Bloom、Viewport Color、程序化 Atmosphere LUT 和 sampler；模型与环境资源分别由 AssetRepository/ModelGpuBuilder、EnvironmentGpuBuilder、Texture/Mesh 与上传队列按上述生命周期管理。Atmosphere 是 SceneDocument 中的纯参数数据，不产生独立派生资产或加载任务。

## 发布与失败语义

- CPU prepare 期间继续渲染当前 Scene。
- GPU build 期间仍保留并渲染当前 Scene；统一窗口顶部和 `Scene -> Scenes` 显示加载进度。
- 只有最新 preview operation 且对应 ModelAsset 的全部 upload fence 完成后才构造单实例 preview Scene 并原子发布。
- 被替换的 Scene 进入 submission-serial retirement queue；其最后一个 ModelAsset lease 释放后，Repository 再按同一 serial 边界销毁共享 GPU 资源。
- Environment prepare/upload 始终保留当前 Scene 和旧 Environment；只有完整新 generation 可以原子替换。
- CPU prepare、GPU build 失败或取消都保留当前 Scene。
- 同步 Viking Room 切换会先取消后台任务，旧 generation 不能在稍后覆盖同步场景。
- Application 关闭前先停止控制服务和 source import future，再取消并 join AssetImportManager/完整工具进程树和 AssetRepository worker，等待设备空闲后按所有权顺序销毁 Scene、ModelAsset 与 Vulkan 对象。

## LoadStats

日志、`VulkanLab -> Diagnostics -> Load Stats` 和 Runtime Control JSON 会记录：

- taskId、operation generation、model generation、repository hit/coalesced、最终状态、worker queue、CPU prepare、GPU build 和总 wall time。
- glTF parse、图片读取/解码/缩放、材质、mesh CPU、hierarchy 和 command recording 耗时。
- prepared CPU bytes、纹理/mesh/vertex/index/material/object 数量及上传字节。
- 每帧 upload pump 的最大耗时/字节、batch submit/completion、fence poll/wait 和 peak in-flight。
- KTX2 cache lookup/hit/miss/invalid、Native BC7/UASTC 命中数、Native payload 读取字节与耗时、Basis transcode 次数/耗时、BC7/RGBA32 fallback 数量和 prebuilt mip 数量。
- 场景资源创建前后的 VMA allocation count、allocation bytes 与 block bytes。

VMA 快照在 ModelGpuBuilder 完成并发布后采集；Repository 面板另外显示 Ready/Loading/Failed/Retiring record、consumer 和资源计数。VMA 数值不等同于 Windows 任务管理器显示的专用显存。

## Cook 与 packaged runtime

`VulkanLabAssetTool cook` 将开发项目和共享 cache 转换成只读运行目录。发布根固定为一个或多个 Native `SceneDocument`；模型预览、builtin/OBJ 和显式 model/environment/profile 选择不再是 Cook 入口。省略 `--scene-id` 时选择 Catalog 中 `optional=false` 的 Native Scene，`--startup-scene` 省略时使用 Catalog 顺序中的第一个已选场景。

`CookClosureResolver` 加载并验证 SceneDocument，按 Catalog 顺序建立只读闭包：

1. 有序 SceneDocument roots 和 `startupSceneId`。
2. Entity 引用的唯一 Catalog Models；每个 Model 使用自身 `importProfile`。
3. SceneDocument 全局环境和 Reflection Probe component 引用的唯一 Environments，以及各自 environment profile。
4. `VulkanLab.exe`、Shader Manifest 及其 programs 实际引用的唯一 SPIR-V。
5. glTF/GLB 主文件和外部 buffer；不复制源 PNG/JPEG。
6. 每个 Model/profile 的 Native BC7 manifest 与内容寻址 KTX2 blob 去重集合。
7. 每个 Environment/profile 的 manifest 与四类浮点 KTX2 blob；不复制源 HDR。

`CookPackageBuilder` 将旧 SceneDocument 规范化为当前 schema v4 写入 staging，不修改项目源文件。Atmosphere 和 Reflection Probe 参数随 SceneDocument 打包；每个 probe 引用的 environment 都进入闭包。所需程序由 Shader Manifest/SPIR-V 闭包携带。Cooked Catalog 只保留闭包内 Scene、Model、Environment 和解析必需 profiles，且不创建 Model Preview entry。Model 的 Validator 报告必须为 Valid/Warnings，实际使用 profile 必须为 Native BC7。Cooked texture manifest 的 source stamp 改写为包内 glTF/GLB stamp；environment source stamp 清空，因为源 HDR 不进入包。

Artifact Index schema v4 将记录分类为 `Model`、`Environment` 和 `SceneDocument`；旧 `Scene` 读取为 Model。SceneDocument record 保存文档 stamp 和精确 Model/profile、Environment/profile references。包内索引由 cooked closure 重建，不从开发索引直接复制。

所有文件先写入输出目录旁的唯一 staging。schema v3 `package_manifest.json` 记录 Scene roots、startup Scene、默认 profile、目标 runtime build revision/configuration/features、BC7 要求及完整文件 hash。`package verify` 在 size/SHA-256 后继续验证最小 Catalog、SceneDocument 引用、Artifact Index references、KTX2 metadata 与 Shader closure。发布前再次比较源 Catalog/SceneDocument 内容哈希；任一变化或验证失败都会删除 staging 并保留旧包。

packaged `main()` 在 Vulkan 初始化前强制以下契约：

- Package project/default profile、Scene roots 和 startup Scene 必须与 cooked Catalog 一致。
- cache root 固定为包内 `runtime_assets`，asset mode 固定为 CookedOnly。
- schema v3 package 只注册 Native Scene，并异步加载 `startupSceneId`；v1/v2 单模型包继续走旧 Model Preview 兼容路径。
- 不创建 AssetImportManager，不写 package ArtifactIndex，也不允许 source fallback 或外部 cache/tool/project override。
- `windows-msvc-runtime` 必须是 Release，并在编译期移除 Editor、Runtime Control、Capture、Asset Authoring、Validation、Debug Utils、GPU Profiler 和 Tracy。
- Windows package 的全部 Model texture artifact 必须是 Native BC7；目标设备不支持 BC7 sampled、linear filtering 和 transfer destination 时返回 `bc7_required`。

开发 CMake 的 `POST_BUILD` 只 stage project locator，不复制完整 `models/` 或 `textures/`。开发运行直接读取 `projectRoot`，正式运行目录由 `cook` 的闭包决定。

## Platform artifact 与 residency 决策

Native BC7 是当前 Windows desktop platform artifact。采用它的直接原因是 Main Sponza 2048 Debug 基线约 `26.11 s`，其中 KTX2 读取约 `3.52 s`、UASTC 到 BC7 转码约 `21.64 s`，GPU build/upload 只有约 `0.31 s`。压缩现在只在首次离线 import 支付；重复加载只读取和上传 BC7。原有 UASTC loader 暂时保留给旧缓存和自定义 portable profile。

2026-07-31 在当前机器上的 Main Sponza 2048 结果为：Release AssetTool 首次用 4 workers 构建 72 张纹理约 `776.84 s`，复用扫描约 `3.15 s`；Debug runtime 两次加载分别约 `1.06 s` 和 `1.02 s`，Native KTX2 read 约 `146 ms`，`basisTranscodeCount=0`、transcode/decode/resize 都为 0。72 张纹理的 BC7 mip payload 与 blob 总量约 `384 MiB`，与原运行时转码后的 GPU payload 一致。

这一步不同时引入 streaming、常驻缓存或 LRU。为保证切换事务性，新 ModelAsset 准备期间会与当前 Scene 的旧资源短暂同时驻留；旧 Scene 只在新 preview 发布后按 submission serial 释放。重新评估 memory admission/streaming/LRU 的条件仍是：目标设备 allocation failure 或单 ModelAsset 超过 device-local budget 的 50%；切换峰值或 resident set 超过 budget 的 70%；或发布体积成为明确产品约束。memory gate 需要先接入 `VK_EXT_memory_budget`，不能把 VMA block bytes 当作可用预算。

## 当前限制

- AssetRepository 只有一个 CPU worker 和一个活动 ModelGpuBuilder；不同模型不会并行 prepare/upload。
- AssetImportManager 当前一次只监督一个资产工具；单个工具内部可以并行编码。没有跨工具并发、持久任务数据库或断点任务队列。
- ArtifactIndex 当前使用 JSON 和命名 mutex；没有 directory watcher 或跨机器共享数据库。索引可从 manifest 重建。
- 单个大 Texture/Mesh 仍是原子 pump，可能造成短帧尖峰。
- 使用 graphics queue，没有专用 transfer queue 或 queue ownership transfer。
- 开发模式中未生成或未命中 KTX2 profile 的图片仍可能运行时 decode/resize，并以 RGBA8 上传；CookedOnly 禁止该路径。
- Windows desktop profiles 统一使用 BC7，不使用 BC5 normal 或 BC4 AO，也不对 Native BC7 blob 使用 Zstd；开发设备无 BC7 时回退源 RGBA8，Cooked package 要求 BC7。
- 环境只支持本地 RGBE `.hdr` 与离线 KTX2 bake；全局环境和最多 8 个局部 Reflection Probe 共享 Repository。没有 EXR、runtime convolution、自动 probe 更新、diffuse SH、BC6H 或 environment streaming。
- 没有渲染进程内编码、mip streaming、LRU residency、virtual texturing 或新旧大模型重叠时的显存 admission 策略；OnDemand 重建由独立 AssetTool 进程完成。
- Viking Room 仍使用同步 OBJ/PNG factory；开发模式从 `projectRoot` 读取，正式包使用 Cook 闭包。
