# 场景与资源资产管线完整计划

> Status: Active
> Last verified: 2026-07-19
> Verified against: `fa30693`

## Summary

本计划把 VulkanLab 当前分散的场景注册、KTX2 离线工具、派生纹理缓存和运行时场景加载连接成一条轻量资产管线。目标体验接近 UE 的 Derived Data Cache 和 Unity 的 Asset Database：原始 glTF、GLB、PNG/JPEG 保持不变，导入器根据源依赖、导入设置、编码器版本和目标 profile 自动生成可丢弃的派生资产；后续场景加载直接复用缓存，不在渲染主线程中执行高成本纹理编码。

计划覆盖以下完整路线：

1. Scene Catalog、稳定资产身份和运行界面内的模型文件导入。
2. Debug/Release 共享的项目级派生缓存。
3. 有界并行、可取消、可观测的纹理导入器。
4. VulkanLab 内的自动按需导入调度。
5. 本地 Artifact Index 和依赖失效管理。
6. 面向 Release 的 Cook/Package。
7. 平台最终纹理和资源驻留管理的后续衔接。

当前已经实现的响应式 CPU prepare、增量 GPU upload 和 KTX2 读取链路继续保留，详见[资源加载](../architecture/resource_loading.md)和[大型场景响应式加载路线图](async_scene_loading_plan.md)。本计划不重新实现这些模块，而是在它们之前增加资产管理层。

## Current Baseline And Gaps

### 已有基础

- `SceneLoadManager` 在 worker thread 中解析 glTF 并生成 `PreparedSceneData`。
- `SceneGpuBuilder` 和 `IncrementalUploadQueue` 按帧上传 GPU 资源。
- `VulkanLabAssetTool texture-cache build` 可以生成 UASTC + Zstd KTX2 和完整 mip chain。
- `DerivedTextureManifest` 根据 scene、texture limit、纹理语义和源文件元数据描述缓存。
- `DerivedTextureCache` 在运行时命中 KTX2 后转码为 BC7；缺失或损坏时回退 stb RGBA8。
- Main Sponza 1024 profile 已验证可从 72 个 KTX2 blob 加载，运行时不再 decode/resize 源图片。

### 当前缺口

- 场景列表和相机覆盖仍硬编码在 `main.cpp`，添加场景必须修改并重新编译程序。
- 运行界面没有原生文件选择入口，不能选择 `.gltf`/`.glb`、检查依赖并注册为场景。
- 资产工具必须手工执行，运行时只消费缓存，不会创建或更新缓存。
- 工具顺序处理纹理，每张纹理同步启动一个 `ktx create` 子进程，首次构建耗时较长。
- 缓存默认跟随工作目录，Debug、Release、测试和不同 build tree 容易形成多份 manifest/blob。
- manifest 以 scene path 为主要入口，缺少独立于复制目录和绝对路径的稳定 scene ID。
- 没有统一的 import task、进度协议、取消语义、失败历史和缓存状态 UI。
- 没有面向发布版本的 Cook；CMake 仍复制完整 `models/`，包括运行时不需要的源图片和其他文件。
- 运行时仍解析源 glTF；当前 KTX2 是通用中间资产，不是完整的目标平台 cooked scene。

## Goals

- 添加或调整场景主要通过数据文件完成，不再修改 `main.cpp`。
- 开发模式可以从 Scenes 面板选择 `.gltf`/`.glb`，完成依赖预检、项目内复制、Catalog 注册、派生资产导入和可选自动加载。
- 选择场景时自动判断对应纹理 profile 是否 Ready、Missing、Stale、Invalid 或 Importing。
- 缓存缺失时由独立进程导入，渲染主线程持续处理窗口、ImGui、Runtime Control 和当前场景。
- 多张纹理可以并行编码，但必须同时受 worker 数量和内存预算约束。
- Debug、Release 和命令行工具默认复用同一个用户级派生缓存。
- 源文件、设置、工具版本或 profile 变化时只重建受影响的 artifact。
- 中断、失败和进程退出不能发布半成品 manifest，也不能破坏已有有效缓存。
- 开发运行支持源资产 fallback；Cooked/Player 模式只消费已验证的运行时资产。
- Stats、日志、UI 和 Runtime Control 对同一 import task 使用一致的状态与统计口径。
- 最终可以构建只含可执行文件、shader、scene metadata 和运行时 artifacts 的发布目录。

## Non-Goals

- 不实现完整编辑器、Content Browser、资产缩略图、拖放导入或材质编辑器；第一版只提供原生文件选择器驱动的 `Import Scene` 工作流。
- 不在 `SceneLoadManager` 或 Vulkan 渲染主线程中执行 UASTC/BC7 编码。
- 第一版不使用数据库服务、云存储或必须部署的中心服务器。
- 第一版不原地修改用户选择的 glTF/GLB、PNG/JPEG；Copy 模式只把经过验证的依赖复制到新的项目目录。
- 不在自动导入阶段同时实现 texture streaming、virtual texturing、mesh LOD 或 bindless。
- 不立即替换 KTX2 为自定义资源格式；Cook 阶段优先复用已经验证的 KTX2 payload。
- 不承诺 Main Sponza Full profile 可在所有 GPU 上驻留；纹理限制和后续显存预算仍然有效。

## Target Architecture

```text
External glTF/GLB -> SceneImportService
                          | copy + register
                          v
Project Source Assets + SceneCatalog + ImportProfile
  models/*.glb
  models/**/*.gltf + .bin + images
          |
          v
AssetImportManager ------------------------------+
  process supervision / progress / cancel        |
          |                                      |
          v                                      |
VulkanLabAssetTool                               |
  dependency scan -> parallel ktx create         |
          |                                      |
          v                                      |
ArtifactStore + ArtifactIndex                    |
  manifests + content-addressed blobs            |
          |                                      |
          +------------ cache status ------------+
          |
          v
SceneLoadManager -> PreparedSceneData
          |
          v
SceneGpuBuilder -> Runtime Scene
```

### Thread And Process Ownership

| 工作 | 渲染主线程 | Scene worker | Asset import worker/process |
|---|---:|---:|---:|
| Scene Catalog 查询 | 是 | 否 | 可读 |
| 原生文件对话框和 Import modal | 是 | 否 | 否 |
| glTF 依赖 preflight、复制和验证 | 否 | 否 | 是 |
| writable Catalog transaction | 否 | 否 | 是 |
| import 状态推进和 UI | 是 | 否 | 否 |
| glTF CPU prepare | 否 | 是 | 仅依赖扫描时解析 |
| KTX2 编码 | 否 | 否 | 是 |
| Vulkan/VMA/descriptor | 是 | 否 | 否 |
| GPU upload | 是 | 否 | 否 |
| manifest/blob 发布 | 否 | 否 | 是 |

Asset tool 必须是独立进程。即使 `ktx create` 崩溃、被取消或内存不足，也不能破坏 VulkanLab 进程和当前 Scene。

## Source Catalog

新增受版本控制的 `assets/catalog.json`，作为开发项目中的场景入口。建议 schema v1：

```json
{
  "schemaVersion": 1,
  "projectId": "vulkan-lab",
  "defaultImportProfile": "desktop_1024",
  "scenes": [
    {
      "id": "main_sponza",
      "displayName": "Main Sponza",
      "source": "models/main_sponza/NewSponza_Main_glTF_003.gltf",
      "camera": {
        "position": [0.0, -35.0, 10.0],
        "yaw": 93.0,
        "pitch": -2.0
      },
      "importProfile": "desktop_1024",
      "optional": true
    }
  ],
  "importProfiles": {
    "desktop_1024": {
      "textureLimit": 1024,
      "textureEncoder": "uastc",
      "qualityPreset": "development"
    }
  }
}
```

规则：

- `projectId` 和 scene `id` 是稳定身份，重命名 display name 或复制 build directory 不改变 artifact identity。
- `source` 相对 catalog 所在项目根目录解析；拒绝逃逸出项目根目录的路径。
- `displayName` 供 UI 和 Runtime Control 使用，必须在 catalog 内进行 ASCII case-insensitive 唯一性检查。
- `optional=true` 的源文件缺失时显示为 Unavailable，不阻止程序启动。
- 相机覆盖属于 scene metadata，不写回 glTF。
- Viking Room 第一阶段允许继续使用内建 factory，并以 `type: "builtin"` 注册；后续可以单独迁移 OBJ 资源。
- catalog schema、重复 ID、未知 profile、非法路径和无效相机数据在 Vulkan 初始化前给出结构化错误。

`SceneRegistry` 改为从 catalog 构造 `SceneEntry`。`main.cpp` 只注册内建 importer/factory 类型，不再列出每个 glTF 场景。

### Project Context And Writable Catalog

开发运行不能修改 build output 中由 CMake 复制的 catalog，否则 Debug 和 Release 会产生互不一致的场景列表。新增统一的 `ProjectContext`：

```cpp
struct ProjectContext {
    std::filesystem::path projectRoot;
    std::filesystem::path catalogPath;
    std::filesystem::path cacheRoot;
    bool catalogWritable = false;
};
```

- `--project <path>` 显式指定开发项目根目录。
- CMake 可以在输出目录生成不进入版本控制的 developer locator，记录当前源码项目根目录，使 IDE 直接启动时不要求额外参数。
- VulkanLab、资产工具和测试统一调用 `ProjectContextResolver`，不能分别猜测 working directory。
- 开发模式的 writable catalog 始终是源码项目中的 `assets/catalog.json`。
- Cooked 包使用包内只读 catalog，`catalogWritable=false`，不保留源码绝对路径。
- 无法确认 writable project root 时，程序仍可读取场景，但禁用 `Import Scene` 和 Catalog 修改操作并显示原因。

## Interactive Scene Import

Scenes 面板增加明确的 `Import Scene...` 命令。点击后使用 Windows 原生 `IFileOpenDialog`，第一版过滤：

```text
glTF Binary (*.glb)
glTF JSON (*.gltf)
```

文件对话框只是选择入口，所有可测试逻辑放在独立的 `SceneImportService`，不写在 ImGui callback 中。导入分为以下步骤：

1. 用户选择 `.gltf` 或 `.glb`。
2. worker 执行轻量 preflight，解析 glTF、列出本地 `.bin`/图片依赖、识别 data URI，并拒绝 remote URI。
3. 显示 Import Scene modal，允许确认 display name、稳定 scene ID、import profile 和 `Load scene after import`。
4. 将源文件和依赖复制到项目 staging 目录。
5. 从 staging 位置再次解析，确认所有相对引用闭合。
6. 原子重命名为 `models/imported/<scene-id>/`。
7. 使用临时文件 + atomic replace 更新 writable catalog。
8. Stage C 完成后自动提交派生资产 import task；在此之前允许注册后显式 source fallback 加载。

### Source Placement Rules

- `.glb` 通常是单文件，复制到 `models/imported/<scene-id>/<source-name>.glb`。
- `.gltf` 必须复制 JSON、外部 buffer 和本地图片，保持能够解析引用的相对目录结构，不能只复制主文件。
- data URI 和 bufferView image 已嵌入主资产，不生成额外源文件。
- `http://`、`https://` 和其他远程 URI 第一版拒绝，并列出具体依赖。
- 包含 `..` 的 URI 在规范化后必须仍位于导入源的允许根目录；路径逃逸、设备路径和符号链接逃逸必须拒绝。
- 源文件已经位于当前项目根目录时，可以选择 `Reference Existing`，此选项只写相对项目路径，不复制文件。
- 项目外文件第一版只能选择 `Copy Into Project`。不把机器相关绝对路径写入受版本控制的 catalog。
- 目标 scene ID 或目录冲突时要求用户修改 ID，不静默覆盖已有场景或源文件。

### Transaction And Failure Semantics

- preflight、复制、验证和 catalog publication 组成显式 transaction。
- catalog 发布前失败时删除本次 staging；不得修改已有 catalog 和同名目标目录。
- 目标目录完成但 catalog 发布失败时回滚新目录；若无法回滚则记录 orphan 路径并给出恢复命令。
- catalog 更新采用进程内写锁、临时文件、flush 和 atomic replace；检测磁盘版本变化，不能覆盖用户刚刚完成的手工编辑。
- Catalog 注册成功而 KTX2 import 失败时保留场景条目，状态显示 ImportFailed，用户可以 Retry、Load Source Fallback 或 Remove From Catalog。
- `Remove From Catalog` 默认只移除注册；删除复制进项目的源目录和未引用 blobs 必须作为独立、带确认的操作。

### Initial Camera

- preflight 没有可靠 bounds 时先不写 camera override。
- 第一次 CPU prepare 得到 scene bounds 后生成 framing camera，并作为运行时建议使用。
- Scenes 面板提供 `Save Current Camera`，只更新 catalog metadata，不写回 glTF/GLB。
- 保存 camera 与 import task 解耦，不触发纹理重新编码。

## Artifact Identity And Storage

### Shared Cache Root

默认缓存移到用户级目录：

```text
%LOCALAPPDATA%/VulkanLab/DerivedAssets/<projectId>/
```

允许通过 `--cache-root` 和 Config 显式覆盖。VulkanLab、VulkanLabAssetTool、Debug、Release 和测试必须调用同一个 `DerivedAssetPaths` resolver，禁止各自拼接路径。

缓存仍是可删除、可重新生成的数据，不提交 Git。现有工作目录下的 `derived_assets/` 作为 legacy cache，只提供一次显式迁移命令，不做静默跨目录复制。

### Cache Layout

```text
<cache-root>/
  index/artifacts-v1.json
  manifests/<scene-id>/<profile-id>.json
  blobs/<content-key>.ktx2
  temp/<import-task-id>/
  logs/<import-task-id>.log
```

manifest key 至少包含：

- scene ID 和 source dependency graph。
- 源图片内容 SHA-256。
- `TextureSemantic`。
- 输出宽高、mipmap filter 和 wrap。
- importer schema、KTX-Software 版本和 encoder settings。
- import profile ID。
- 将来生成平台最终格式时包含 target platform、GPU format 和 encoder version。

blob 继续按内容寻址，使多个 scene、sampler 和 profile 在 key 完全一致时自动共享结果。manifest 只引用 blob，不拥有 blob。

## Import Profiles

至少提供两类 preset：

| Preset | 目标 | 建议设置 |
|---|---|---|
| `development` | 快速日常迭代 | UASTC quality 1 或 2，减弱/关闭 RDO，Zstd 3，完整 mip |
| `production` | 最终画质和分发体积 | 当前 UASTC quality 2、RDO、Zstd 9、Lanczos4 |

profile 的完整参数必须进入 cache key，不能只记录显示名称。修改 preset 定义会生成新 artifact，不能误用旧结果。

1024、2048 和 Full 是不同 profile。自动导入不能用 1024 artifact 冒充 2048，但 UI 可以明确提供“临时使用较低 profile”的人工选择。

## Import Task Protocol

扩展资产工具命令：

```powershell
VulkanLabAssetTool catalog validate --catalog assets/catalog.json
VulkanLabAssetTool import scene --catalog assets/catalog.json --scene main_sponza
VulkanLabAssetTool import all --catalog assets/catalog.json
VulkanLabAssetTool cache status --catalog assets/catalog.json
VulkanLabAssetTool cache migrate --from derived_assets
```

保留现有 `texture-cache build` 一段兼容期，并在文档中标记 deprecated；内部最终调用同一个 importer implementation。

当传入 `--progress-json` 时，stdout 只输出 UTF-8 NDJSON 事件，普通日志写 stderr 和 task log。事件至少包括：

```json
{"event":"started","taskId":"...","scene":"main_sponza","total":72}
{"event":"artifact_started","index":4,"image":12,"estimatedBytes":268435456}
{"event":"progress","completed":18,"reused":12,"encoded":6,"failed":0}
{"event":"artifact_completed","index":4,"durationMs":8421}
{"event":"completed","manifest":"...","encoded":60,"reused":12}
```

错误事件包含稳定 `code`、message、scene、image index 和子进程 exit code。协议版本必须出现在 `started` 事件中。

## Bounded Parallel Import

`TextureCacheBuilder` 从串行循环改为 task graph：

1. 主线程解析 glTF、收集纹理任务、计算源 hash 和 cache key。
2. 已存在且通过验证的 blob 立即标为 Reused。
3. 未命中任务进入 ready queue。
4. scheduler 同时受 `maxWorkers` 和 `memoryBudgetMiB` 限制。
5. worker 为一张纹理创建独立临时输入/输出并启动 `ktx create`。
6. 完成后验证 KTX2，原子替换 blob，再释放预算。
7. 所有任务成功后一次性原子发布 manifest。

默认 worker 数量建议为：

```text
min(4, max(1, logicalCpuCount / 2))
```

但内存预算拥有更高优先级。每个任务在调度前根据解码尺寸估算工作集，并保留一个保守下限；总 reservation 超过默认 `2048 MiB` 时暂不启动新任务。单个超预算任务允许独占执行，避免永久饥饿。

工具增加：

- `--workers N`
- `--memory-budget-mib N`
- `--force`
- `--keep-going`，只用于批量导入；默认 fail-fast。

Windows 下父工具进程使用 Job Object 管理所有 `ktx.exe` 子进程。取消或异常退出时终止完整子进程树，清理 task temp directory，保留已经原子发布的内容寻址 blob，但不发布不完整 scene manifest。

## Automatic Import Coordination

新增 `AssetImportManager`，只负责进程监督和状态，不解析 glTF、不访问 Vulkan：

```cpp
struct AssetImportRequest {
    std::string sceneId;
    std::string profileId;
    ImportReason reason;
};

enum class AssetImportState {
    Queued,
    Scanning,
    Importing,
    Publishing,
    Completed,
    Failed,
    Cancelling,
    Cancelled
};
```

行为：

- Application 请求加载 scene 前查询 artifact status。
- Ready：立即提交给现有 `SceneLoadManager`。
- Missing/Stale/Invalid：开发模式下提交 import task，并继续渲染当前 Scene。
- import 成功：重新验证 manifest，然后自动开始 scene load。
- import 失败或取消：保留当前 Scene，UI 显示错误，不自动进入慢速 fallback，除非用户明确选择 `Load Source Fallback`。
- 相同 scene/profile 的请求合并；新的不同 profile 请求不覆盖仍有消费者的任务。
- scene generation 同时覆盖 import 和 load。旧 import 完成后可以留下有效缓存，但不得自动发布旧 Scene。
- Application 退出时先停止接收任务、取消子进程树、join supervisor，再销毁日志和窗口。

开发运行模式：

```text
OnDemand     缺失时自动导入，推荐默认
ReadOnly     只读缓存，缺失时允许显式 source fallback
CookedOnly   缺失 artifact 视为发布包错误
```

模式通过 Config 和启动参数选择，不根据 Debug/Release 宏隐式改变语义。这样自动化、测试和用户运行结果可预测。

## UI And Runtime Control

Scenes 面板采用紧凑列表而不是完整 Content Browser：

- 顶部命令包含 `Import Scene...` 和 `Refresh`，并提供名称搜索。
- 列表显示 display name、Ready/Missing/Stale/Invalid/Importing 状态和 profile。
- 单击选择，双击或 `Load` 命令加载；`Reimport` 只重建派生资产。
- 选中项显示 source、scene ID、profile，并提供 `Load`、`Reimport`、`Save Current Camera` 和 `Remove From Catalog`。
- `Import Scene...` 打开原生文件对话框，再显示只包含 display name、scene ID、profile、source placement 和自动加载选项的 modal。

新增 ImGui `Assets` 面板显示详细导入诊断：

- catalog 路径、project ID、cache root 和 import mode。
- scene 的 source、profile 和 Ready/Missing/Stale/Invalid/Importing 状态。
- 当前 import task 的阶段、纹理进度、encoded/reused/failed、耗时和 worker 数量。
- `Import`、`Reimport`、`Cancel`、`Load Source Fallback` 和 `Open Log` 操作。
- 最近任务历史和失败原因。

导入 modal 确认后立即关闭，长时间进度只显示在 Assets 面板和全局 loading 状态，不用阻塞 modal。OnDemand 模式显示 Import/Reimport；ReadOnly 模式禁用写操作并显示原因；CookedOnly 模式完全隐藏开发导入命令。

Renderer 和 Scenes 面板只显示用于选择/加载场景的简洁状态；详细诊断放在 Assets 面板，避免重复状态机。

Runtime Control 增加：

- `asset.catalog`
- `asset.status`
- `asset.import`
- `asset.cancel`
- `asset.cache_info`

命令行自动化增加 `VulkanLabAssetTool catalog add --source ...`，复用 `SceneImportService` 的 preflight、copy 和 catalog transaction。v1 不通过 Named Pipe 暴露任意本地路径导入，避免 Runtime Control 协议同时承担文件系统写入权限；Runtime Control 只导入已注册 scene 的派生资产。

`scene.load` 在 OnDemand 模式下可以返回一个覆盖 import + load 的 operation ID。`load.status` 保持兼容，并在结果中增加 `phase=importing|preparing|uploading`。客户端默认等待整个 operation，`--no-wait` 立即返回 ID。

## Artifact Index And Dependency Tracking

阶段一到三可以直接读取 manifest 判断状态。自动导入稳定后增加可重建的本地 `ArtifactIndex`，用于快速列出项目状态，避免每次 UI 刷新扫描所有 manifest。

index 记录：

- scene/profile 状态和最后成功 import task。
- source dependency path、size、mtime 和已知 SHA-256。
- manifest path、blob 数、总磁盘字节和最后访问时间。
- importer/encoder/schema 版本。
- 最近失败 code 和日志路径。

index 使用临时文件 + atomic replace 更新。它不是事实来源，损坏或缺失时可以从 catalog 和 manifests 重建。第一版继续使用 nlohmann JSON，不为当前规模引入 SQLite；当资产数量、并发写入或查询需求证明 JSON 成为瓶颈时再迁移。

依赖检查分两级：

1. 快速检查 size + mtime，用于启动和场景列表。
2. import 前或快速检查可疑时计算 SHA-256，决定是否真正失效。

文件监听作为后续增强：只把 scene 标记为 PossiblyStale，不在文件变化回调中直接启动编码。真正导入仍由 scheduler 串行化决策，避免保存多个源文件时反复重建。

## Cook And Package

增加明确的发布命令：

```powershell
VulkanLabAssetTool cook `
  --catalog assets/catalog.json `
  --platform windows-x64 `
  --profile desktop_2048_production `
  --output dist/VulkanLab
```

Cook 执行：

1. 验证 catalog 和全部必需源依赖。
2. 确保所有目标 profile artifact Ready；缺失时导入或按参数失败。
3. 生成只读 cooked catalog，去除开发绝对路径和 import-only metadata。
4. 复制可执行文件、shader、必要 glTF/GLB/bin、KTX2 manifests 和被引用 blobs。
5. 默认不复制已由 artifact 覆盖的 PNG/JPEG，也不复制 FBX、USD、MAX、预览图和无引用文件。
6. 写入 package manifest，包含文件 hash、schema、平台和构建版本。
7. 在临时 staging 目录完成后原子发布或替换输出目录。

CookedOnly 运行时禁止调用资产工具和 source fallback。artifact 缺失、损坏或设备不支持包内格式时给出明确启动/加载错误，而不是静默占用大量 RGBA8 显存。

第一版包内仍可保留 KTX2，并在 worker 中快速转码 BC7。后续可增加 Windows 平台最终 BC7 artifact，直接保存完整 BC7 mip payload，进一步消除运行时转码；该优化必须通过加载时间和包体积数据决定。

## Relationship To Residency And Streaming

资产管线完成后，资源才具备稳定 ID、尺寸、mip metadata 和引用关系，适合进入[大型场景响应式加载路线图](async_scene_loading_plan.md)的条件性 Stage 4：

- `VK_EXT_memory_budget` admission。
- mip residency 和按需上传。
- LRU eviction。
- 跨场景 texture/mesh 复用。
- 可选 virtual texturing。

这些能力消费 Asset Database 提供的 metadata，但不应阻塞本计划的 Scene Catalog、自动导入和 Cook。若 1024/2048 BC7 已满足目标显存和加载时间，可以继续推迟 streaming。

## Implementation Stages

### Stage A: Scene Catalog And Shared Cache Foundation

#### Scope

- 增加 catalog schema、parser、validation 和 `SceneCatalog`。
- 将 glTF scene 列表和相机覆盖从 `main.cpp` 迁移到 catalog。
- 增加 `ProjectContextResolver`、`--project` 和 CMake developer locator，确保 UI 修改源码 Catalog 而非 build output 副本。
- 增加 Win32 `IFileOpenDialog` adapter、`SceneImportService`、依赖 preflight 和 Import Scene modal。
- 实现 `.glb` 单文件复制、`.gltf` 本地依赖闭包复制、项目内 reference 和 catalog transaction。
- 增加稳定 project ID、scene ID 和 profile ID。
- 实现统一 `DerivedAssetPaths`，默认使用 `%LOCALAPPDATA%` 项目级缓存。
- 更新 `DerivedTextureManifest` 路径和 schema，使其使用 scene ID/profile ID。
- 提供 legacy cache migration 命令。
- 更新构建复制逻辑和文档。

#### Acceptance

- 添加可选 glTF scene 不需要重新编译 VulkanLab。
- 从 Scenes 面板选择项目外 `.glb` 后，可以复制、注册并通过 source fallback 加载。
- 导入包含外部 `.bin` 和多级纹理目录的 `.gltf` 后，项目内副本可以独立解析；缺失、远程或逃逸依赖在写 catalog 前失败。
- Debug/Release developer locator 指向同一 writable catalog，不修改各自输出目录中的副本。
- Debug、Release 和资产工具报告完全相同的 cache root。
- Main Sponza 现有 1024 blobs 可以迁移并复用，无重新编码。
- catalog 非法时在创建 Vulkan/Window 前失败并报告具体字段。
- 场景 UI 和 Runtime Control 继续按 display name 工作，同时结果包含稳定 scene ID。

#### Implementation Status (2026-07-19)

Stage A 已实现于 `412ca02` 和 `df02615`：

- `assets/catalog.json`、`ProjectContextResolver` 和 CMake developer locator 已替换 `main.cpp` 的逐场景硬编码。
- Debug、Release 和 `VulkanLabAssetTool` 默认使用 `%LOCALAPPDATA%/VulkanLab/DerivedAssets/<projectId>`。
- manifest schema v2 使用 scene/profile 稳定 ID；`texture-cache migrate` 已把 Main Sponza 1024 的 72 个旧 blob 无重编码迁移并由运行时命中。
- `SceneImportService` 已实现 `.glb/.gltf` preflight、依赖闭包、URI 安全检查、staging、二次验证、目录/Catalog 原子发布和取消回滚。
- Scenes 面板已接入 `IFileOpenDialog`、导入 modal、worker 进度/取消和可选 source fallback 自动加载；CLI `catalog add` 复用同一服务。
- Debug/Release 构建、CPU tests、CLI import CTest、Runtime Control Catalog 查询以及 Main Sponza 72/72 cache-hit 加载已通过。

仍需人工体验检查 Windows 文件对话框、modal 文本布局和从真实外部模型导入后的最终画面；这些检查不改变 Stage A 的公开接口或事务实现。

### Stage B: Parallel Incremental Importer

#### Scope

- 把 `TextureCacheBuilder` 拆为 scanner、scheduler、worker result 和 publisher。
- 增加 worker count、内存预算、Job Object 取消和 NDJSON progress。
- 保持 blob/manifest 原子发布和现有 cache key 语义。
- 增加 development/production preset。
- 增加单元测试用可替换 process runner，避免测试依赖真实长时间编码。

#### Acceptance

- Main Sponza 1024 clean import 在目标机器上明显快于当前串行基线。
- 默认并行度下系统不出现内存持续无界增长。
- 第二次执行仍为 `encoded=0, reused=72`。
- 中途取消后没有发布 scene manifest，已有有效 manifest 不受影响。
- 一个纹理失败时错误包含 image/semantic/command exit code。
- 相同输入在串行和并行模式下生成相同 cache key 和等价 manifest。

#### Implementation Status (2026-07-19)

Stage B 已实现于 `67bb5c1`：

- `TextureCacheBuilder` 已拆为 scan/work/publish 流程，`TextureCachePipeline` 提供确定性调度和 worker result，`IProcessRunner` 允许测试替换真实编码进程。
- scheduler 同时执行 worker 上限和估算内存 reservation；默认最多 4 worker、2048 MiB，超预算单任务可独占执行。
- Windows `Win32JobProcessRunner` 使用 Job Object 管理完整 `ktx.exe` 子进程树，精确限制继承句柄；Ctrl+C、失败和退出都会终止在途编码并清理临时文件。
- CLI 已支持 `--workers`、`--memory-budget-mib`、`--preset development|production`、`--progress ndjson`/`--progress-json`。stdout 的 NDJSON 与 stderr 子进程诊断分离。
- manifest 记录 preset/encoder settings，并以临时文件加 `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)` 发布；development 保留原 cache key，production 使用独立参数/key。
- CPU tests 覆盖 worker/预算上限、超大任务、乱序完成、fail-fast、fake runner 和真实 Job Object 取消；CLI CTest 覆盖真实 KTX、串并行等价、cache hit、preset 隔离、结构化失败及无 manifest/temp 泄漏。

Release/Main Sponza 1024 clean 实测：串行 1 worker 为 `261.19 s`，并行 4 worker 为 `117.15 s`，加速约 `2.23x`；两份 72-entry manifest 的 SHA-256 相同。第二次并行执行为 `encoded=0, reused=72`。真实 Ctrl+C 测试在 8 个 blob 完成后中断，确认 scene manifest 不存在、临时文件为 0、资产工具/ktx 子进程为 0；重试复用 8 个 blob、编码剩余 64 个，并生成与 clean 基准相同的 manifest。

自动化能够证明调度 reservation 有界，运行时进程采样未观察到持续增长；Windows 任务管理器中的系统级峰值仍属于人工性能观察项，不作为 Stage B 接口完成的阻塞条件。

### Stage C: Automatic On-Demand Import

#### Scope

- 新增 `AssetImportManager`、进程管道读取、operation ID 和任务历史。
- Application 在 scene load 前执行 artifact admission。
- import 完成后自动连接现有 SceneLoadManager。
- 把 Stage A 的 Import Scene transaction 接入 AssetImportManager，增加 Assets UI、取消、source fallback 和 import mode。
- `Load scene after import` 覆盖 catalog registration、artifact import 和 scene load，并使用统一 operation/generation。
- 扩展 Runtime Control 和 VulkanLabCtl。

#### Acceptance

- 删除 Main Sponza 1024 manifest 后选择场景，窗口仍响应并显示真实编码进度。
- 通过 `Import Scene...` 选择新 `.glb` 后，Catalog 注册、KTX2 编码和自动加载可以连续完成，modal 不会阻塞主循环。
- 编码完成后自动开始 CPU prepare/GPU upload 并发布 Scene。
- 取消、快速切换 scene、退出和 importer crash 不会崩溃或发布旧 generation。
- cache hit 场景不启动资产工具，加载性能不低于当前实现。
- ReadOnly/CookedOnly 模式绝不启动编码进程。

#### Implementation Status (2026-07-19)

Stage C 已实现于 `b4536f4`：

- `ArtifactStatus` 在 scene load 前验证稳定 manifest identity、profile/texture limit、source stamp、blob 路径和 KTX2 header；状态统一为 Ready/Missing/Stale/Invalid/Importing。
- `AssetImportManager` 使用独立 supervisor thread 和高位 task ID，启动同目录 `VulkanLabAssetTool import scene`，校验 protocol v1 NDJSON、保存日志/历史，并用 Job Object 终止完整工具/ktx 子进程树。编码并发仍由 Stage B worker 与内存预算控制。
- `AssetLoadCoordinator` 把 import consumer 纳入全局 operation generation；旧 import 可以留下 cache，但不能触发旧 Scene。import 成功后自动接入现有 SceneLoadManager/SceneGpuBuilder，失败或取消保留当前 Scene。
- OnDemand、ReadOnly、CookedOnly 已作为显式 Config/启动参数接入；只有 OnDemand 可以编码，ReadOnly 仅允许显式 source fallback，CookedOnly 禁止 fallback。
- Stage A 的 Catalog 注册成功后会自动提交 KTX2 import，并可继续自动加载。Scenes 面板增加搜索、选择/双击、Load、Reimport、source fallback、camera 保存和 Catalog 移除；Assets 面板显示 artifact 状态、真实编码进度、worker、日志、失败和历史。
- Runtime Control/VulkanLabCtl 已增加 `asset.catalog/status/import/cancel/cache_info`；`scene.load` 可返回覆盖 import + prepare + upload 的复合 operation ID，`load.status/cancel` 在各阶段保持同一外部 ID。

自动验证覆盖 Debug/Release 构建与 3/3 CTest、协议/崩溃任务失败、generation、Catalog 原子编辑、OnDemand/cache hit、真实取消/重试和模式隔离。真实取消返回 Cancelled，残留 `VulkanLabAssetTool/ktx` 进程为 0；ReadOnly/CookedOnly 的 import 均被拒绝，缺失 artifact 的 scene load 返回确定性错误。

Main Sponza 1024 使用隔离 cache、保留 72 个内容寻址 blob 但不提供 manifest：自动 operation 重建结果为 `encoded=0/reused=72`，随后加载 75 textures、405 meshes，runtime 72/72 cache hit、0 decode、0 resize；manifest 重建后的 Debug scene-load 阶段约 `7.22 s`，VMA allocation delta `279.74 MiB`。Release 共享 cache hit 的 scene-load 阶段约 `1.82 s`。这验证了缺失 manifest admission 与复合 operation，不重复 Stage B 的 117 秒 clean encoding 基准。

Windows 文件选择器、Import modal/Assets 面板布局、导入期间拖动/resize 和最终画面仍需人工 UX/视觉检查；自动测试未把这些未执行项目描述为通过。

### Stage D: Artifact Index And Change Detection

#### Scope

- 增加可重建 ArtifactIndex、快速状态查询和 cache usage 统计。
- 增加 source dependency 两级验证。
- 可选接入 Windows directory watcher，仅做 stale 标记和 debounce。
- 增加 cache prune/dry-run 命令，按未引用 blob 和最后访问时间清理。

#### Acceptance

- 数百到数千 artifact 的 Assets 面板不需要逐帧扫描文件系统。
- 修改一张源图片只使引用它的 scene/profile 失效。
- index 删除或损坏后可以从 manifest 重建。
- prune 默认 dry-run，不能删除 catalog 当前引用或正在导入的 blob。

#### Implementation Status (2026-07-19)

Stage D 已实现于 `8fa838e`：

- 新增 schema v1 `ArtifactIndex`，记录 scene/profile identity、manifest、依赖 size/mtime/SHA-256、blob 闭包、encoder/schema、成功/访问时间和失败日志。索引使用临时文件、atomic replace 和按 cache root 命名的短时 mutex；缺失、损坏或 schema/project 不匹配时从 Catalog + manifests 重建。
- Fast 查询只在 stamp 可疑时使用共享 BCrypt SHA-256，mtime 假阳性不会触发重建；真实内容变化只使引用该文件的记录 Stale。Scene admission 额外验证 blob size 和 KTX2 header。旧进程遥测与 importer 新记录按当前 manifest stamp 合并，避免 lost update。
- 资产工具在 manifest 成功后刷新索引；import、migration、index rebuild 和 prune execute 使用同一个跨进程 cache mutation mutex。Assets UI 和 Runtime Control 显示索引、cache usage、未引用 blob、成功 task/访问时间及失败诊断，不再逐帧递归扫描 cache。
- `VulkanLabAssetTool cache index rebuild` 提供显式恢复；`cache prune` 默认 dry-run，`--execute` 在 mutation lock 内重新扫描全部 manifest。保留期默认 7 天，已引用/正在导入/近期孤立 blob 不删除，损坏 manifest 时 fail closed。

CPU tests 覆盖损坏索引恢复、SHA-256 已知向量、mtime 假阳性、单依赖定向失效、admission 损坏 blob、同 key 并发写合并、prune dry-run/execute/保留期和损坏 manifest fail-closed。Debug/Release 构建与 3/3 CTest 均通过；真实共享 cache index rebuild 得到 3 条 Ready 记录、81 个受引用 blob，prune dry-run 候选为 0。

Runtime Control 下 Main Sponza 1024 再验证为 72/72 KTX2 hit、0 decode/resize、75 textures、405 meshes，Debug 总加载约 `6.75 s`，VMA allocation delta 约 `279.74 MiB`。`asset.cache-info` 与索引一致，成功 load 后 `lastAccessUnixMs` 更新；程序通过 `quit` 正常退出。

Windows directory watcher 是本阶段可选项。当前 Assets 状态不是逐帧刷新，Fast 查询会在明确刷新和 admission 时确定性校验，Catalog 规模也没有显示 watcher 瓶颈，因此暂缓实现；后续只有在需要即时外部文件通知且轮询实测不足时再加入 PossiblyStale/debounce。最终画面、Assets 面板布局和外部编辑文件后的即时 UX 仍需人工检查，未作为自动验证通过项。

### Stage E: Cook And Packaged Runtime

#### Scope

- 增加 `cook` 命令、cooked catalog 和 package manifest。
- 发布目录只包含运行所需资产。
- 增加 CookedOnly 模式和严格 artifact validation。
- CMake 不再把整个 `models/` 作为最终发布策略。
- 自动化运行 packaged build 并加载主要场景。

#### Acceptance

- 发布目录不含 Main Sponza 未使用源图片和非运行文件。
- 在没有源码仓库和用户 cache 的机器上仍能加载 cooked scenes。
- 删除一个必需 blob 会得到确定性错误，不发生 PNG fallback。
- package manifest hash 验证通过，重复 cook 的文件集合稳定。

Stage E 已实现于 `fa30693`：

- 新增 schema v1 `RuntimePackageManifest`、package discovery 和启动前 size/SHA-256 验证；packaged ProjectContext 使用只读 Catalog/cache，强制 CookedOnly、固定 profile、关闭 validation layer，并拒绝外部 project/cache/tool override。
- `VulkanLabAssetTool cook` 按 scene/profile 生成 transaction staging，只复制 runtime executable、15 个实际 Shader variant SPIR-V、glTF/GLB 与必要 buffer、cooked manifest/blob 去重闭包和重建后的 ArtifactIndex。`package verify` 复用运行时 verifier。
- CookedOnly 不创建 importer、不写 package ArtifactIndex，并将缺 manifest/entry、stamp 不匹配、KTX2 读取/转码/mip 错误升级为 load failure。纹理档位被 package profile 锁定，不会回退源图片。
- CTest 覆盖 missing artifact、真实 KTX2 cook、输出路径重叠、最小文件集合、重复 cook 文件集合、失败替换保留旧包、移除 source/cache 后验证和 blob 篡改。Debug/Release 均为 4/4 tests 通过。
- Main Sponza 1024 Release 包包含 1 scene、72 unique blobs、15 SPIR-V 和 94 个受保护文件，总计 `225,169,159` bytes；没有 PNG/JPEG 或未使用 source 文件。它从独立临时目录和空 `LOCALAPPDATA` 完成加载，72/72 KTX2 hit、0 miss/decode/resize，Release 总加载约 `1.39 s`，VMA allocation delta 约 `279.74 MiB`。删除必要 blob 后进程在 Vulkan 初始化前以 `runtime package file is missing` 确定性退出。

最终画面和无开发工具的全新 Windows 机器安装体验仍需人工检查；包闭包、启动、严格加载、Runtime Control 和错误路径已自动验证。

### Stage F: Platform Artifacts And Residency, Conditional

只有 Stage E 后的统计证明 KTX2 转码、显存或加载时间仍不满足目标时实施：

- 缓存最终 BC7/BC5/BC4 mip payload。
- normal map 评估 BC5，AO 评估 BC4，更新 shader/format 契约。
- 引入 memory budget admission、mip streaming 和 LRU。
- ArtifactIndex 提供每级 mip offset/bytes，streamer 按稳定 resource ID 请求数据。

## Testing Strategy

### Unit Tests

- catalog schema、重复 ID、路径规范化、可选源和 profile 引用。
- `SceneImportService` 的 GLB copy、glTF dependency closure、data URI、remote URI、路径逃逸、ID 冲突和 transaction rollback。
- catalog 文件被外部修改时拒绝 lost update，不覆盖磁盘新版本。
- cache root resolver 在 VulkanLab、工具、Debug 和 Release 中一致。
- artifact key 对源 hash、语义、尺寸、wrap、profile 和 encoder version 敏感。
- scheduler 遵守 worker 数量和内存预算，超大单任务不会饥饿。
- progress NDJSON schema、乱序 worker completion 和错误传播。
- manifest/index atomic publication、损坏恢复和取消。
- operation generation 保证旧 import/load 不发布 Scene。
- cooked catalog 和 package manifest 的引用闭包。

### Integration Tests

- 8x8 测试纹理真实调用 KTX-Software，验证 KTX2 mip/block metadata。
- Sheen Chair clean import、cache hit、源修改失效和 source fallback。
- 使用临时项目通过 `catalog add` 导入 GLB 和带 `.bin`/纹理子目录的 glTF，再从复制后的项目目录加载。
- 模拟 catalog publication 失败，确认 staging/目标目录回滚且原 catalog 字节不变。
- Main Sponza 1024 clean/import-hit/cancel/retry。
- 同时发起多个 scene 请求，只有最后 operation 发布。
- importer 异常退出、非法 JSON、超长日志和 VulkanLab 退出。
- Cook 到临时目录后从该目录启动 VulkanLab，禁止访问源码和用户 cache。

### Manual GPU And UX Validation

- Main Sponza 1024/2048 的首次自动导入耗时、CPU、内存和磁盘峰值。
- 从 Windows 文件选择器导入项目外 GLB/glTF，确认 modal、进度、取消、自动加载和 Scenes 列表更新。
- import 期间窗口移动、resize、shader 切换、Runtime Control ping/status/cancel。
- PBR-lite NormalMapped 下 baseColor、normal、MR、AO、alpha、emissive 和 transmission 画面。
- packaged build 在独立目录运行，任务管理器专用显存与 VMA 统计合理。
- 删除/损坏缓存时 UI 错误和恢复流程清晰。

最终画面、Windows 窗口响应和驱动显存曲线需要人工检查；代码、构建、单元/集成测试和 Runtime Control 自动化可以由 Codex 完成。

## Metrics And Acceptance Targets

- Main Sponza 1024 cache hit 保持约 72/72，runtime decode/resize 为 0。
- clean import 相比当前串行工具至少有明确加速；先记录 1/2/4 workers 基线，再确定默认值，不预设不可靠的硬倍数。
- import scheduler 的实际 in-flight workers 和 reserved memory 不超过配置。
- cache hit 场景不产生 importer process，加载时间不高于当前已验证基线的显著范围。
- 自动 import 期间主循环持续渲染，Windows 不报告无响应。
- 取消后残留 `ktx.exe` 数量为 0，未发布 temp manifest 数量为 0。
- Debug 和 Release 对同一 scene/profile 使用同一 blob key，磁盘不重复保存相同内容。
- Cooked 包不依赖用户 DDC；发布资产缺失时确定性失败。

## Migration And Commit Sequence

建议按以下独立提交推进，避免 catalog、并发、运行时调度和打包混在一起：

1. `feat: add project context and data-driven scene catalog`
2. `feat: import gltf scenes from the application`
3. `refactor: centralize derived asset cache paths`
4. `feat: migrate texture manifests to stable asset ids`
5. `feat: parallelize KTX2 asset imports`
6. `feat: expose structured asset import progress`
7. `feat: add automatic on-demand asset imports`
8. `feat: add asset diagnostics and runtime control`
9. `feat: track local artifact dependencies`
10. `feat: cook runtime asset packages`
11. `docs: document asset import and cooking workflows`

每个阶段完成后更新 Current 架构文档；本计划全部完成、废弃或被替代后移入 `doc/archive/plans/`。

## Risks And Mitigations

- **并行 UASTC 导致内存峰值过高**：worker count 与内存 reservation 双重限制，默认保守，并记录每任务峰值。
- **工具取消留下子进程**：Windows Job Object 绑定完整 process tree。
- **mtime 导致错误失效**：快速检查后用 SHA-256 确认；稳定 ID 不依赖绝对 build path。
- **manifest 并发写入损坏**：单 scene/profile publication lock、临时文件、flush 和 atomic replace。
- **自动导入造成意外长任务**：Assets UI 明确进度/取消；ReadOnly 和 CookedOnly 模式绝不自动编码。
- **复制 glTF 时遗漏或逃逸依赖**：先解析完整本地 URI 闭包，在 staging 中二次解析，通过后才发布目录和 catalog。
- **UI 修改了 build output Catalog**：所有写操作依赖 writable `ProjectContext`；无法确认源码项目时禁用导入。
- **Catalog 手工编辑与 UI 写入冲突**：写前比较文件版本/hash，发现变化后重新加载并要求用户重试。
- **缓存无限增长**：ArtifactIndex 记录访问时间，提供安全 dry-run prune；不自动删除当前引用资源。
- **开发 fallback 掩盖发布缺失**：CookedOnly 禁止 fallback，Cook 在发布前验证完整引用闭包。
- **引入完整 Asset Database 过度设计**：第一版使用 catalog + manifests + 可重建 JSON index，不引入数据库服务。

## Assumptions

- Windows、Vulkan、KTX-Software v4.4.2 和当前 Named Pipe Runtime Control 仍是第一目标环境。
- `ktx create` 继续作为编码后端；并行阶段通过多个受控子进程扩展，不先重写为 libktx encoder API。
- KTX2 继续作为跨平台中间纹理资产，桌面 Vulkan 优先运行时转码 BC7。
- 默认自动导入模式为 OnDemand，但必须可通过 Config/启动参数切到 ReadOnly 或 CookedOnly。
- 用户级共享缓存不进入版本控制；发布包通过 Cook 携带自己的只读 artifacts。
- 第一轮并行度和内存预算需要在 Main Sponza 1024 clean import 上测量后最终确定。
- 本计划优先解决场景和资源的身份、导入、缓存与发布；Residency/Streaming 仍依据实际显存数据条件实施。
