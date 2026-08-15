# VulkanLab 渲染器收口与 Application 重构计划

> Status: Active
> Last verified: 2026-08-15
> Verified against: `ff80960`

## Progress

| Stage | Status | Evidence |
|---|---|---|
| 0. Baseline And Scope Lock | Complete | [Stage 0 baseline](renderer_consolidation_baseline.md) |
| 1. Retire Viking Room And Legacy OBJ | Complete | Renderer Smoke Scene replaces Viking; async ModelAsset/Native Scene loading is the only scene path |
| 2. Remove RenderGraph Migration Debt | Complete | Graph setup/recordNode is the only pass path; legacy barriers, startup self-tests, and migration aliases removed |
| 3. Feature-Aware Resource Residency | Complete | Graph-driven residency, reduced MRT variants, adaptive occlusion and capacity-tiered punctual shadows; default 800x600 steady-state resident images are about 162.9 MiB |
| 4. Extract SceneRuntimeCoordinator | Complete | Scene/environment load state machines, repositories, publication and serial retirement moved out of Application |
| 5. Complete SceneWorkflowController | Complete | Catalog, artifact, validation, import and asset task state moved behind typed Workflow snapshots/actions |
| 6. Render Settings And Feature Ownership | Complete | Typed requested/active settings, centralized validation, deterministic frame feature resolution and feature-owned Shader program families |
| 7. Extract EditorController | Complete | Editor workspace, authoring session, viewport, capture and panels moved behind an editor-only controller |
| 8. Isolate Runtime Control | Complete | Pipe lifecycle, dispatch, protocol serialization and Host implementation moved behind an optional adapter |
| 9. Final Application Composition | Complete | Application reduced to grouped composition, staged initialization, canonical frame scheduling and explicit shutdown |
| 10. Documentation And Closeout | Next | Update current README and architecture documentation against the consolidated implementation |

## Summary

VulkanLab 当前已经从早期单模型 Vulkan 示例演进为包含 RenderGraph、Dynamic Rendering、Bindless Material、Native Scene、场景编辑、离线资产管线、自动化控制以及多种实时光照算法的开发中渲染器。功能数量本身不是问题；当前需要处理的是功能边界、运行时资源成本和应用层职责没有同步收口。

本计划统一处理以下问题：

1. 将项目正式定位为“开发中的 Vulkan 1.3 实时渲染器”，Scene Editor、AssetTool 和 Runtime Control 都是服务于渲染开发的辅助设施。
2. 退役最早期的 Viking Room、OBJ parser、同步 `SceneFactory` 和 `SceneObject` 兼容路径。
3. 删除 RenderGraph 迁移后仍保留的旧 Pass 执行路径、手动 barrier 和运行时 self-test。
4. 让 RenderGraph 的 Pass 裁剪真正驱动物理资源驻留，关闭的算法不再分配专用图像或执行无意义的 MRT/复制。
5. 将约一万行的 `Application.cpp` 收缩为 composition root 和主循环，提取场景运行、项目工作流、渲染设置、编辑器和 Runtime Control 边界。
6. 保留 SSAO、GTAO、CACAO、SSR、SSGI、DDGI、TAA、Atmosphere 等现有算法作为正式维护的核心渲染能力，并按功能族组织，而不是通过删除算法解决复杂度。
7. 更新 README 和 Current 文档，使代码结构、产品定位和实际运行行为一致。

目标架构：

```text
Application
  ├─ Platform/Vulkan lifecycle
  ├─ SceneRuntimeCoordinator
  ├─ SceneWorkflowController
  ├─ RenderSettingsController
  ├─ EditorController              [optional]
  ├─ RuntimeControlAdapter         [optional]
  └─ Renderer
       ├─ RenderGraph
       ├─ RenderResourcePool
       └─ Render Feature Modules
            ├─ Core Forward / HDR
            ├─ Shadows / Visibility
            ├─ Ambient Occlusion
            ├─ Reflections
            ├─ Global Illumination
            ├─ Atmosphere / Environment
            └─ Temporal / Post Process
```

## Current Baseline

### Code And Ownership

当前源码和职责基线：

- `src/app/Application.cpp` 约 9,900 行，包含初始化、主循环、场景和环境加载、资产状态、Runtime Control JSON、全部主要 ImGui 页面、Scene authoring、Capture 和诊断拼装。
- `Application` 直接实现 `RuntimeControlHost`，声明约 30 个协议方法。
- `SceneWorkflowController` 当前主要保存 Catalog、SceneEntry 和 UI/asset operation 状态，并未真正统一场景、导入和取消工作流。
- `Renderer`、`RenderGraph` 和各 Pass 已经存在清晰雏形，但 Application 仍手动解析大量 Shader program、组装 feature 状态并处理大量 diagnostics。
- 15 个 Pass 同时保留 RenderGraph `recordNode()` 和旧 `execute()` 实现，旧路径约有 1,200 行重复或失效逻辑。
- `runResourcePoolSelfTest()` 和 `runModelAssetSharingSmoke()` 仍在普通运行时执行。
- `Application::registerScene()` 已无调用者。

### Runtime Resource Baseline

在 800x600、默认场景和大多数高级算法关闭的本机采样中：

- RenderGraph active Pass：约 24。
- RenderGraph culled Pass：约 91。
- active image bytes：约 325 MiB。
- resident image bytes：约 497 MiB。
- screen-space estimated bytes：约 79 MiB。

主要浪费来源：

- `RenderResourceRegistry::realize()` 提前创建所有已注册图像，Graph 只裁剪 Pass，没有裁剪物理资源。
- MainForward 在 SSR/SSGI 关闭时仍写入 baseline indirect specular/diffuse MRT。
- HDR Composite 在无需合成时仍保留完整分辨率目标和 Copy node。
- Hi-Z Occlusion 默认开启，小场景也支付 SurfacePrepass、pyramid 和 compute 成本。
- Point/Spot Shadow 按最大容量注册固定资源，而不是按当前设置或实际使用量驻留。

### Viking Room Compatibility Island

Viking Room 是当前唯一使用以下路径的内容：

- Catalog `type = builtin` 和 `builtinFactory = viking_room`。
- 同步 `SceneFactory`。
- OBJ parser 和 `Mesh::fromOBJ()`。
- `SceneObject`、legacy material/mesh/texture ownership。
- `RenderItemOwnerKind::LegacyObject`。
- 同步加载期间的 `vkDeviceWaitIdle()`、Pipeline Cache clear 和直接 Scene 发布。

Cooked Native Scene 已经拒绝 builtin/OBJ，因此删除该路径不会削弱正式 SceneDocument 发布能力。

## Product Positioning

README 的目标表述固定为：

> VulkanLab is a Vulkan 1.3 real-time renderer under active development for Windows. It provides a scene-authoring workspace and offline asset pipeline for implementing, profiling, and comparing modern rendering techniques.

项目边界：

- 核心产品是实时渲染器。
- Scene Editor 用于构造、保存和检查可重复的渲染测试场景，不定位为通用游戏编辑器或 DCC 工具。
- AssetTool 用于验证、导入、派生和 Cook 渲染资产，不定位为通用内容制作软件。
- Runtime Control 是 Developer Automation Interface，不是普通用户运行时功能。
- Cooked Runtime 用于验证可发布的只读场景闭包，不承诺完整游戏框架能力。
- API、SceneDocument schema、Shader ABI 和渲染结果在开发阶段允许演进。
- 项目暂不承诺生产级稳定性、跨平台、脚本系统、游戏逻辑、网络、物理或完整 ECS。

README 应按以下结构组织：

1. Status
2. Rendering Features
3. Architecture Highlights
4. Scene Authoring And Asset Pipeline
5. Developer Tooling
6. Build And Run
7. Current Limitations
8. Documentation

## Goals

- 删除 Viking Room 和它维持的全部 OBJ/同步 Scene 兼容路径。
- 所有 Model Preview 和 Native Scene 使用 `PreparedModelData -> AssetRepository -> ModelAsset -> ModelInstance -> IRenderWorld`。
- Application 只保留依赖组合、平台生命周期、主循环和少量跨服务帧调度。
- UI 和 Runtime Control 通过相同的类型化 service/action 操作场景、资产、渲染设置和 Capture。
- Runtime Control 保留完整自动化价值，但默认关闭，并在 Runtime/Cooked 配置中不编译。
- RenderGraph 是唯一的帧内 Pass 顺序、资源使用和同步真源。
- 关闭的渲染功能不产生专用 active Pass、专用资源分配或 descriptor 更新。
- 所有现有算法继续编译并可通过 Editor/Runtime Control 选择。
- 当前 glTF、KTX2/BC7、Catalog、SceneDocument、Environment 和 Cook cache key 不因本计划失效。
- 每个阶段都能独立构建和启动，不依赖后续提交恢复程序可运行性。

## Non-Goals

- 不删除现有 AO、Reflection、GI、Atmosphere、Shadow、TAA 或 Culling 算法。
- 不引入 RHI、插件 ABI、脚本系统或运行时动态加载模块。
- 不在本阶段实现 async compute、多 Queue、并行 command recording、GPU-driven draw compaction 或资源别名。
- 不重写 RenderGraph 拓扑编译器。
- 不改变 Bindless/Legacy Material GPU ABI。
- 不改变 Native SceneDocument 和 Cook package schema，除非删除 builtin 兼容字段必须产生明确迁移错误。
- 不为了拆文件创建大量只有一个实现的抽象接口。
- 不将 Renderer、Scene、Asset 和 Editor 一次性拆成大量静态库；先解除依赖环，再调整 CMake target。
- 不修改 `doc/archive/` 中记录 Viking Room 的历史文档。
- 不将本地大型模型、环境、KTX cache、`imgui.ini` 或构建产物纳入提交。

## Design Principles

1. **功能多不等于运行成本高**：算法可以全部保留，但 inactive 功能必须接近零 Pass、零专用显存和零 descriptor churn。
2. **Application 是 composition root**：可以拥有服务并规定调用顺序，但不实现服务内部状态机、UI 或协议序列化。
3. **单一行为入口**：Editor 和 Runtime Control 必须调用同一套 action，不允许复制业务判断。
4. **不可变帧快照**：UI 修改完成后只构建一次 `RenderWorldFrameSnapshot`，RenderView、Shadow、Visibility 和 Diagnostics 使用同一时刻的数据。
5. **Graph 是帧内真源**：Pass 不自行推导执行顺序，不在 Graph 外插入帧内 barrier。
6. **资源所有权明确**：场景 GPU 资产归 AssetRepository；帧内 attachment 归 RenderResourcePool；Editor 只保存 ID 和 DTO。
7. **先删除兼容孤岛，再提取边界**：先移除 Viking/OBJ 和旧 Pass path，避免把废弃逻辑搬进新模块。
8. **按行为拆分，不按行数拆分**：大文件只有在包含多个状态机或依赖域时拆分。
9. **开发工具显式隔离**：Editor、Runtime Control、Capture、Validation 和 Tracy 可以独立裁剪，不改变渲染算法 ABI。

## Target Runtime Ownership

### Application

Application 最终只负责：

- 按依赖顺序创建和销毁 Window、VulkanContext、Device、FrameSync、Renderer 及各 Controller。
- 轮询窗口和输入。
- 调用 Controller 的 `pump()`/`update()`。
- 固化每帧调用时序。
- 处理 swapchain 和 viewport resize 的跨系统协调。
- 将最终 `RenderFrameInput` 提交给 Renderer。
- 在退出前停止后台任务并按反向所有权销毁系统。

目标接口示意：

```cpp
class Application {
  public:
    void run();

  private:
    void initialize();
    void mainLoop();
    void runFrame(float deltaSeconds);
    void recreateSwapchainIfNeeded();
    void shutdown();

    AppPlatform platform_;
    RenderRuntime renderRuntime_;
    SceneRuntimeCoordinator sceneRuntime_;
    SceneWorkflowController sceneWorkflow_;
    RenderSettingsController renderSettings_;
    OptionalEditorController editor_;
    OptionalRuntimeControl runtimeControl_;
};
```

不要求机械实现上述聚合类型；重点是 Application 不再拥有每个子系统的内部任务字段。

### SceneRuntimeCoordinator

拥有所有依赖 Vulkan 主线程和 GPU 生命周期的 World 状态：

- `AssetRepository`。
- `EnvironmentAssetRepository`。
- `SceneLoadManager`。
- 当前 `IRenderWorld`。
- Native Scene model/environment resolution。
- Scene/environment 异步发布。
- 当前和 retired World generation。
- submission-serial 延迟销毁。
- 当前 Scene、Environment、Load task和 `SceneLoadStats` snapshot。

主要公开操作：

```cpp
struct SceneLoadRequest {
    std::string sceneId;
    bool reloadModelAssets = false;
    bool sourceFallback = false;
};

class SceneRuntimeCoordinator {
  public:
    uint64_t requestLoad(const SceneLoadRequest&);
    bool cancel(uint64_t taskId);
    uint64_t reloadCurrent();
    void pump(const SceneRuntimeFrameContext&);
    RenderWorldFrameSnapshot buildFrameSnapshot() const;
    SceneRuntimeSnapshot snapshot() const;
};
```

它不绘制 UI、不解析 Runtime Control JSON、不保存 modal 状态。

### SceneWorkflowController

负责项目级、CPU-only 或工具进程级工作流：

- Catalog 和 Scene Registry 刷新。
- Model Import、Validator、Artifact 和派生资产状态。
- Import/reimport/cancel/load-after-import 协调。
- 当前模型和 SceneDocument 选择。
- Catalog 写入后的 registry generation。
- 为 UI 和 Runtime Control 生成统一只读 snapshot。

它通过注入的类型化 actions 请求 SceneRuntime 执行 GPU load，而不是持有 `Device`、Renderer 或 World 指针。

```cpp
struct SceneWorkflowActions {
    std::function<uint64_t(const SceneLoadRequest&)> requestLoad;
    std::function<bool(uint64_t)> cancelLoad;
    std::function<uint64_t()> reloadCurrent;
};
```

当前 `ModelImportUiState` 中纯 UI 字段应迁移到 Editor；future、任务映射、Artifact 和 Validator 状态保留在 Workflow。

### RenderSettingsController

集中管理：

- 当前 Shader variant ID。
- `RenderSettings` requested values。
- 参数范围和跨设置冲突校验。
- Device/Shader capability 与 requested/active/fallback 状态。
- Texture Limit 对 Model Preview 的会话策略。
- Camera 会话参数和 fallback Sun设置。
- `FrameRenderFeatures` 推导。

公开类型化接口：

```cpp
class RenderSettingsController {
  public:
    ApplySettingsResult apply(const RenderSettingsPatch&);
    bool selectShader(std::string_view id);
    ResolvedRenderSettings resolve(const RendererCapabilities&) const;
    RenderSettingsSnapshot snapshot() const;
};
```

UI、CLI 和 Runtime Control 均使用 `apply()`，不各自 clamp 或验证参数。

### EditorController

仅在 `VKL_ENABLE_EDITOR_UI` 开启时存在，拥有：

- `EditorDockWorkspace`。
- `SceneEditorSession`。
- Scenes、Assets、Outliner、Inspector、Render、Materials、Diagnostics 页面。
- Scene authoring dialogs。
- Undo/Redo、Dirty、快捷键和 selection。
- Viewport controller、Gizmo 和输入路由。
- Reflection Probe capture authoring workflow。
- 纯 UI 状态、搜索文本和 modal 输入。

Editor 只持有 snapshot 和 actions，不直接访问 Application 私有字段或 Vulkan 对象。需要 ImGui texture 的 Viewport 资源通过窄接口由 GuiSystem 提供。

### RuntimeControlAdapter

Runtime Control 继续保留，定位为 Developer Automation Interface：

```text
NamedPipeServerWin32
  -> RuntimeCommandQueue
  -> RuntimeCommandDispatcher
  -> RuntimeControlAdapter
  -> typed services/snapshots
```

调整要求：

- `Application` 不再继承 `RuntimeControlHost`。
- `RuntimeControlAdapter` 实现 `RuntimeControlHost`。
- `RuntimeStatusSerializer` 负责 typed snapshot 到 JSON 的转换。
- Pipe thread 仍只处理 I/O 和排队，所有 service action 在主线程执行。
- Runtime Control默认关闭。
- `windows-msvc-runtime` 和 Cooked package不编译 server/adapter。
- 协议命令保持兼容；移除 Viking 后请求该名称正常返回 `scene_not_found`。
- Asset authoring未编译时继续返回 `feature_not_compiled`。

建议保留的自动化类别：

- system/info/quit。
- scene list/current/load/reload/status/cancel。
- shader、camera、environment 和 render settings。
- render status、GPU/Graph/material/lighting diagnostics。
- capture request/status/cancel。
- automation-only window resize和稳定帧等待。
- 可选 asset validation/import/status。

### Renderer And Feature Modules

所有现有算法保留为核心渲染能力，但按功能族管理：

```text
Core Forward/HDR
  Surface Data, MainForward, HDR Composite, ToneMap, Present

Shadows/Visibility
  CSM, Point/Spot Shadow, Frustum, Distance, Small Object, Hi-Z

Ambient Occlusion
  Off, SSAO, GTAO, CACAO

Reflections
  Off, SSR, Reflection Probe

Global Illumination
  Off, SSGI, DDGI

Environment/Atmosphere
  IBL, Skybox, Procedural Atmosphere, Aerial Perspective

Temporal/Post Process
  TAA, Bloom, Screen-Space Debug
```

每个功能族统一负责：

- Capability 查询。
- Settings 解析。
- Graph node注册。
- 逻辑资源声明。
- Shader program解析。
- Typed status和 diagnostics。

不要求运行时插件或虚函数注册表。v1 可以使用静态组合的 feature owner，只要 Application 不再手动解析约 40 个 program ID。

## Canonical Frame Order

每帧调用顺序固定为：

```text
Window::pollEvents
RuntimeControlAdapter::processOne
SceneWorkflowController::pump
SceneRuntimeCoordinator::pump
EditorController::beginFrame/draw
Apply editor mutations
RuntimeWorld::buildRenderSnapshot
RenderSettingsController::resolve
buildRenderView(snapshot, resolved settings)
VisibilitySystem::build(snapshot, render view)
Renderer::renderFrame
Capture completion / Present
Repository and World retirement collection
```

约束：

- UI 修改完成后才生成不可变 snapshot。
- RenderView、ShadowSystem、VisibilitySystem、Materials Inspector 和 Runtime diagnostics 使用同一 snapshot generation。
- Editor 或 Runtime action 不直接调用 `RuntimeWorld::update()` 来修补局部时序。
- Scene/Environment 发布发生在 snapshot 构建前的固定阶段。
- GPU 资源销毁只依据 FrameSync completed submission serial，不在普通切换路径调用 `vkDeviceWaitIdle()`。

## Stage 0: Baseline And Scope Lock

### Changes

- 记录当前默认 800x600 的 active/culled Pass、active/resident image bytes 和主要资源占用。
- 记录 Application/Renderer/Pass 源码规模和当前 include dependency。
- 建立功能保留矩阵，确认现有算法全部保留。
- 列出 Viking/OBJ、旧 Pass execute、runtime self-test 和兼容 alias 的完整引用。
- README 先修改产品定位，但不宣称后续架构已经完成。

### Completion Criteria

- 有可重复的 Runtime Control或日志采样步骤。
- 后续阶段可以比较显存、Pass和代码变化。
- 没有在未测量前删除渲染算法。

## Stage 1: Retire Viking Room And Legacy OBJ

### Source And Asset Removal

- 从 Catalog 删除 `viking-room`。
- 删除 `models/viking_room.obj` 和 `textures/viking_room.png`。
- 删除 `vikingRoomSceneFactory()`。
- 将 `BuiltinScenes.*` 重命名为表达 glTF prepare职责的文件。
- 删除同步 `SceneFactory` typedef和 `SceneEntry::factory`。
- 删除 `SceneEntry::builtin` 及其 Application/UI/Asset 分支。
- Catalog遇到旧 `type=builtin` 时返回明确 `catalog_builtin_model_unsupported`，不静默映射。
- 删除 `Config::texturePath`。
- 删除 `Mesh::fromOBJ()`。
- 删除 `src/tiny_obj_loader.cpp`、tinyobj header wrapper、`vkl_obj_parser` target和链接依赖。
- 删除 `SceneObject`、legacy textures/materials/meshes/lights数组和 `RenderItemOwnerKind::LegacyObject`。
- 保留 `Scene` 作为 Model Preview facade，但只允许 ready `ModelInstance`。
- 删除只为 Viking旋转存在的 `Scene::UpdateFn`。
- 删除 Application同步 `loadScene()` 和其中的 device idle/cache clear路径。

### Replacement Smoke Scene

新增一个小型、可提交、仅依赖程序化 primitive的 Native Scene：

```text
Renderer Smoke Scene
  Camera
  Directional Light
  Point Light
  Plane
  Cube
  Sphere
  MASK test primitive/material fixture if available
```

它替代：

- Validation smoke中的 Viking load。
- Legacy/PBR/Debug Shadow smoke。
- IBL visual fixture中的 OBJ/PNG复制。
- Runtime Control、RenderDoc和Tracy文档示例。
- Viking golden；旧 baseline和metadata删除，确认新候选后再建立新 baseline。

Sheen Chair继续承担 glTF/PBR/material smoke。

### Compatibility

- glTF Model Preview ID保持不变。
- Native Scene ID保持不变。
- Primitive Model ID保持不变。
- Runtime Control协议版本不变。
- KTX/BC7、ArtifactIndex和Cook cache不迁移。
- `doc/archive/` 中 Viking历史记录不修改。

### Completion Criteria

- 非 archive源码和Current文档无 Viking、OBJ或builtin factory引用。
- 构建图中不存在 tinyobj target。
- Application启动只走异步 ModelAsset或Native Scene加载。
- Model Preview不再触发 `vkDeviceWaitIdle()`或Pipeline Cache clear。

### Completion Record

- `Renderer Smoke Scene` 已作为可提交的 Native Scene 加入 Catalog，并替代 Validation、RenderTest 和 IBL fixture 中的 Viking 入口。
- Viking OBJ/PNG、tinyobj target、同步 `SceneFactory`、`SceneObject` 和 Application 同步加载路径已删除。
- Debug 原生 Smoke Scene与无 Editor Runtime 模型预览均已实际启动；场景加载只通过异步 Repository/Native Scene 状态机发布。
- 全功能 Debug 的 VulkanLab、AssetTool、Ctl 和 RenderTest 已构建；CPU test target仍有与本阶段无关的既有接口漂移，未执行测试套件。

## Stage 2: Remove RenderGraph Migration Debt

### Changes

- 将 `IRenderPass` 的 Graph `setup()/recordNode()` 设为唯一执行接口。
- 删除默认 `record()->execute()` adapter。
- 删除所有 Pass旧 `execute()` 实现。
- 删除 Graph管理路径中的 Pass-owned image barrier。
- 对仍需要内部多步操作的 Pass拆成 Graph node，或使用 node内部明确的局部 barrier并记录理由。
- 删除无人调用的 FrameSync一次性上传 API及始终为零的 legacy统计字段。
- 删除 `runResourcePoolSelfTest()` 普通启动调用。
- 删除 `runModelAssetSharingSmoke()` 普通加载调用。
- 将仍有价值的 invariant检查移到现有测试代码或 Debug-only显式诊断命令。
- 删除 `Application::registerScene()`。
- 分批删除 `PreparedSceneData`、`RenderCommand`、`CatalogScene`、`SceneImportService` 等已完成迁移的兼容别名。

### Completion Criteria

- Pass不存在双重执行实现。
- Graph帧路径不存在 Synchronization1 barrier。
- 普通启动不执行内部 self-test。
- 行为和画面保持不变。

### Completion Record

- `IRenderPass::setup()/recordNode()` 已成为唯一执行接口，15 个多节点 Pass 的旧 `execute()` 路径和单节点 adapter 已删除。
- Graph 帧路径不再包含 Synchronization1 barrier；DDGI 仅保留持久资源初始化和 AS 依赖所需的明确 Synchronization2 局部 barrier。
- FrameSync 一次性上传 API、legacy 同步统计、普通启动资源池/ModelAsset self-test 和无调用场景注册入口已删除。
- `RenderItem`、`PreparedModelData` 和 `ModelImportService` 已成为正式命名，旧兼容别名和桥接头文件已移除。
- 全功能 Debug、dev-fast、runtime Release 和启用 CACAO 的 AO compare 配置均已构建；原生 Renderer Smoke Scene 与无 Editor Runtime 已实际运行，未执行测试套件。

## Stage 3: Feature-Aware Resource Residency

### Graph And Resource Pool

- `CompiledRenderGraph` 输出 active logical resources及其生命周期。
- `RenderGraphResourcePool` 只 realize 当前或近期需要的物理资源。
- 新激活功能在当前 frame slot安全点创建资源。
- 失活资源通过 submission serial进入 retirement，不立即销毁在途资源。
- 默认不进行 transient aliasing；先实现正确的按需驻留。
- Diagnostics区分：
  - logical declared bytes。
  - active bytes。
  - resident bytes。
  - retiring bytes。

### Feature-Dependent Attachments

- SSR关闭时不创建或写 baseline indirect specular MRT。
- SSGI关闭时不创建或写 baseline indirect diffuse MRT。
- SSR/SSGI都关闭时跳过无意义的 HDR Composite target和copy。
- 没有后处理合成需求时，MainForward输出直接进入后续ToneMap输入。
- 仅Occlusion需要SurfacePrepass时使用depth-only或最小attachment配置。
- Point/Spot Shadow资源按设置容量或实际选中数量创建，不固定占满最大配置。
- Bloom、AO、SSR、SSGI、DDGI、Atmosphere和TAA关闭时不驻留其专用资源。

### Adaptive Work

- 增加 Occlusion最小候选阈值，小场景跳过 VisibilityDepth、Hi-Z和OcclusionCull。
- 阈值使用统计驱动，保留 UI/Runtime override用于对比。
- 不改变 Frustum和Shadow culling。

### Initial Targets

- 800x600默认配置 resident image初始目标不高于约250 MiB；该值是本机工程目标，不是跨GPU硬门槛。
- inactive功能的 dedicated resource bytes为0或仅保留明确共享fallback。
- Feature切换和resize不调用 `vkDeviceWaitIdle()`。

### Stage 3 Completion Record

- `CompiledRenderGraph` now publishes active image handles and owner Passes. `RenderResourceRegistry` tracks `Unallocated / Resident / Retiring`, creates newly active images at the current frame-slot safe point, and releases inactive images after their submission serial completes.
- Runtime and UI diagnostics report logical, active, resident and retiring image bytes. On the current machine at 800x600, the default Sheen Chair path is about `162.9 MiB` active/resident versus the previous roughly `497 MiB` baseline; the full logical declaration remains about `664.7 MiB` without becoming resident.
- PBR Forward and SurfacePrepass compile reduced fragment-output variants. SSR-only writes the specular lighting MRT, SSGI-only writes the required lighting MRTs, and the baseline path skips both lighting MRTs and HDR composite work.
- SurfacePrepass selects depth-only, depth plus normal, depth plus normal/motion, or full MRT output from actual frame features. Small scenes skip Surface/Hi-Z/Occlusion by the default `64` candidate threshold, with UI and Runtime Control override.
- Point and Spot shadow images use capacity tiers. Verified examples: one Point shadow uses `24 MiB`; two Point plus two Spot shadows use `48 MiB + 8 MiB`, instead of reserving all four slots for both types.
- Disabled Atmosphere and DDGI outputs bind persistent 1x1 fallback images; their real LUT/probe images participate in graph residency. Enabling and disabling Atmosphere returns steady-state residency to the baseline after serial retirement.
- Descriptor refresh is frame-slot safe. Passes are prepared in two phases: topology/buffer preparation before graph compilation, then image descriptor preparation after residency synchronization. This preserves DDGI/RayTracing dynamic topology without updating descriptor sets still used by another frame.
- Dynamic SSAO, TAA, SSR, SSGI, Bloom and Occlusion transitions were exercised through Runtime Control without validation errors. `windows-msvc-dev-fast` and `windows-msvc-runtime` both build and sustain rendering.
- Known follow-up: the current constructor bootstrap still briefly realizes all registered images so legacy Pass constructors can create initial descriptor sets. First-frame graph synchronization removes inactive residency, so this affects startup peak rather than steady state. Fully eliminating that peak requires lazy Pass descriptor construction and can be handled independently after the Application extraction stages.

## Stage 4: Extract SceneRuntimeCoordinator

### Move From Application

- Scene load/reload/cancel。
- SceneLoadManager pump。
- Native Scene model resolution。
- Environment request/publish。
- current/retired World ownership。
- Repository pump/release。
- Scene generation和load statistics。
- Scene Camera defaults的运行时部分。

### Transaction Rules

- 新World全部资源Ready后才原子发布。
- 失败、取消或generation过期时保留当前World。
- 当前World和Environment始终有单一owner。
- Editor session attach/detach通过发布事件完成，不由Coordinator包含ImGui逻辑。

### Completion Criteria

- Application不包含具体scene load phase switch。
- SceneRuntime可在无Editor、无Runtime Control构建中独立工作。
- 所有Model Preview和Native Scene共享相同异步发布模型。

### Stage 4 Completion Record

- 新增 `SceneRuntimeCoordinator`，统一持有 `AssetRepository`、`EnvironmentAssetRepository`、`SceneLoadManager`、当前/退役 World、环境句柄、加载任务、统计和 scene generation。
- Model Preview 与 Native Scene 的解析、资源等待、环境等待、原子发布、失败/取消和 submission-serial 延迟回收均从 `Application` 移入 Coordinator；`Application` 不再包含 scene load phase switch。
- Editor attach/detach、ArtifactIndex touch 和渲染设置同步通过无 ImGui/Runtime Control 依赖的发布回调完成。通知回调失败与 World 发布事务隔离，不会反向把已发布场景标记为失败。
- Coordinator 提供稳定的领域错误码，`Application` 仅在边界处转换为 Runtime Control 错误，保持既有 `scene_unavailable`、`source_fallback_not_applicable`、`model_prepare_unavailable` 和 `environment_artifacts_unavailable` 语义。
- `Application.cpp` 删除约 1,100 行场景/环境状态机和生命周期代码；`Application.h` 不再直接持有 Repository、LoadManager、World retirement queue 或环境加载内部状态。
- `windows-msvc-dev-fast` 与 `windows-msvc-runtime` 均已构建并实际启动。Runtime Control 加载 `Renderer Smoke Scene` 后得到 7 个实体、4 个共享模型实例和 2 盏灯；未执行测试套件。

## Stage 5: Complete SceneWorkflowController

### Move From Application

- Catalog/registry刷新。
- ArtifactIndex读取、保存、status刷新。
- Validator status刷新。
- AssetImportManager task监督和load-after-import映射。
- Scene/model选择和查找。
- Import/reimport/cancel动作。

### State Separation

- Workflow保存future、task、generation和错误。
- Editor保存搜索文本、modal输入和当前Tab。
- Runtime Control只读取Workflow snapshot并调用actions。
- Workflow不包含Renderer、Device、ImGui或JSON类型。

### Completion Criteria

- UI与Runtime Control的scene/asset行为来自同一action实现。
- Application不直接访问ArtifactIndex内部结构。
- `SceneWorkflowController`名称与实际职责一致。

### Stage 5 Completion Record

- `SceneWorkflowController` 现统一持有 Catalog/registry 刷新、ArtifactIndex 与使用统计、Validator 状态、AssetImportManager、load-after-import 映射、model import future、task generation、取消和错误状态。
- `SceneWorkflowSnapshot` 与 `AssetWorkflowSnapshot` 提供纯类型只读状态；Scenes/Assets 面板只保留搜索、modal 草稿和选择等展示状态，不持有后台 future、Catalog、ArtifactIndex 或 manager task。
- UI、Runtime Control 和 Reflection Probe 环境构建均调用同一 Workflow action；Runtime Control 只在 Application 边界执行 typed snapshot 到 JSON/协议错误的转换。
- `Application` 不再直接持有 `AssetImportManager`、`ArtifactIndex`、artifact/validation 状态映射、model import UI operation 或 import-to-load task 映射。保留的 `catalog_` 与 `sceneRegistry_` 是 Controller 数据的兼容引用，用于 RuntimeWorld、authoring 和环境运行时适配，不形成第二份状态。
- Asset Authoring 关闭时，Controller 的编译期路径不引用 authoring-only service symbol；Runtime Release 因此保持可独立链接和运行。
- `windows-msvc-dev-fast` 与 `windows-msvc-runtime` 均已构建成功；开发版 Runtime Control 的 `info`、`scene list`、`asset catalog`、`quit` 以及带 Editor/无 Editor 启动路径已实际验证。按项目策略未运行测试套件。

## Stage 6: Render Settings And Feature Ownership

### RenderSettingsController

- 从Application移出Shader选择和RenderSettings patch校验。
- 集中处理debug view互斥、capability fallback和参数范围。
- 输出requested/active typed snapshot。
- Runtime Control不再单独实现校验。

### Renderer Feature Families

- Shader program解析由对应feature owner完成。
- Application不再填充大型 `RendererShaderPaths`字符串集合。
- 每个feature根据`FrameRenderFeatures`注册实际Graph nodes。
- 公共资源只在明确契约下共享：Surface Data、Depth Pyramid、Color Pyramid、Temporal History、HDR。
- AO后端共享输入、输出约定和可复用denoise/history基础，但允许各算法保留独立实现。
- Reflection和GI算法共享Surface/Depth/Color基础，不互相隐藏依赖。

### Completion Criteria

- 添加新算法不需要修改Application。
- 所有算法仍在dev-fast和runtime构建中可用。
- 不使用散布在Shader ABI中的算法编译宏裁剪运行时行为。

### Stage 6 Completion Record

- 新增 `RenderSettingsController`，统一持有 Shader variant、requested settings、capability support 和上一帧 runtime active state；UI 与 Runtime Control 通过同一 patch action 修改设置。
- Controller 集中处理 debug view 互斥、capability fallback、有限值/枚举检查、参数范围归一化以及 CACAO 重建副作用。Runtime Control adapter 只保留 JSON 类型和名称解析，不再保存领域范围规则。
- `Application` 构建 RenderView 时只消费 Controller 的 resolved active settings；requested settings继续用于 UI 和协议查询，因此切换不兼容 Shader 时设置不会丢失。
- 新增纯函数 `FrameFeatureResolver`，成为 `FrameRenderFeatures` 和 requested-to-active AO/GI 状态的唯一推导入口；Application 和 Renderer 不再各自拼装 feature flags。
- Shader Manifest program解析按 Shadow、Surface/Visibility、Screen Space、Atmosphere/GI 和 Post Process family 分组，各 family解析自己的 contract；旧的 `RendererShaderPaths` 巨型字符串集合已删除。
- RenderGraph Pass注册按五个 feature family收口，保留原有执行顺序和显式共享资源契约。新增算法不再要求修改 Application。
- `windows-msvc-dev-fast` 与 `windows-msvc-runtime` 均已构建并持续启动。Runtime Control验证了曝光范围归一化，以及 Bloom requested state在 PBR/Legacy切换时保持、active state按 Shader能力自动变化；按项目策略未运行测试套件，也未进行完整GPU视觉回归。

## Stage 7: Extract EditorController

### Move From Application

- 所有 `draw*Panel()`。
- Scene authoring dialogs。
- Entity delete/duplicate/shortcuts。
- Editor session保存/冲突处理。
- Viewport UI状态和Gizmo输入协调。
- Reflection Probe authoring capture状态机。
- Capture UI状态。

### UI Contract

Editor每帧接收：

```cpp
struct EditorFrameContext {
    SceneWorkflowSnapshot scenes;
    SceneRuntimeSnapshot runtime;
    RenderSettingsSnapshot rendering;
    RendererDiagnosticsSnapshot diagnostics;
    EditorActions actions;
};
```

Panel只能保存展示状态，不持有GPU资源或后台任务。

### Completion Criteria

- `VKL_ENABLE_EDITOR_UI=OFF` 时Application不包含ImGui相关成员和空panel方法。
- Application不直接调用ImGui。
- Editor操作和Runtime Control操作使用相同service actions。

### Stage 7 Completion Record

- 新增仅在 `VKL_ENABLE_EDITOR_UI=ON` 时编译的 `EditorController`，集中持有 Dock workspace、各编辑器 Panel、`SceneEditorSession`、Viewport/Gizmo、Reflection Probe authoring capture 和 Capture UI 状态。
- `EditorControllerServices` 显式注入平台、场景运行时、工作流、渲染设置和捕获服务；Controller 不持有 `Application&`，Panel 继续通过 typed snapshot/action 操作业务状态。
- `Application` 不再包含 ImGui 调用、Panel 实现、编辑会话、Viewport resize 状态或 authoring modal；只保留编辑器生命周期和 canonical frame-order 委托。
- Editor 与 Runtime Control 继续共用 `SceneWorkflowController`、`SceneRuntimeCoordinator` 和 `RenderSettingsController` 的 action/validation 路径，没有复制场景加载或渲染设置规则。
- `Application.cpp` 从约 7,700 行降至约 3,270 行，`Application.h` 为约 215 行；剩余主要体积来自 Runtime Control JSON/协议实现，交由 Stage 8 处理。
- `windows-msvc-dev-fast` Debug 和 `windows-msvc-runtime` Release 均构建成功并持续启动 6 秒；日志无 error/critical。按项目策略未运行测试套件或完整 GPU 视觉回归。

## Stage 8: Isolate Runtime Control

### Changes

- 新增 `RuntimeControlAdapter` 实现现有Host接口。
- 新增typed snapshot到JSON serializer。
- Application只调用`processOne()`并处理最终quit intent。
- 将约1,600行runtime方法从Application移出。
- 保持Named Pipe线程不访问World、Renderer或Vulkan。
- 为长任务保留现有task ID/status/cancel语义。
- 在README中将其放入Developer Tooling，而不是Highlights首屏核心功能。

### Completion Criteria

- Application不继承`RuntimeControlHost`。
- Runtime Control关闭时不创建queue、pipe、adapter或JSON状态。
- `windows-msvc-runtime`不链接Runtime Control实现。
- CLI现有非Viking命令保持兼容。

### Stage 8 Completion Record

- 新增 `RuntimeControlAdapter`，集中拥有 Named Pipe、`RuntimeCommandQueue`、Dispatcher、pending quit response handshake 和全部协议 JSON 序列化。
- `Application` 不再继承 `RuntimeControlHost`，不再声明或实现任何 `runtime*()`/`ControlJson` 方法；主循环只调用 `processOne()` 并处理最终 quit intent。
- Adapter 通过 `RuntimeControlServices` 读取平台、场景、渲染和诊断状态，通过 `RuntimeControlActions` 调用现有 Scene/Environment/Settings action，不持有 `Application&`。
- 共享 Application action 不再抛出协议层错误；SceneWorkflow、SceneRuntime 和 RenderSettings 的领域错误只在 Adapter 边界映射为稳定协议错误码。
- 只有 `VKL_ENABLE_RUNTIME_CONTROL=ON` 且启动参数显式启用时才创建 Adapter、Queue 和 Pipe；Runtime preset 不编译 Adapter，也不链接 Runtime Control 实现。
- `Application.cpp` 从约 3,270 行降至约 1,245 行，`Application.h` 从约 215 行降至约 158 行。
- dev-fast Debug、完整 Debug 控制工具和 runtime Release 均构建成功；实际验证 `ping`、`system.info`、`scene.current`、`render-settings get`、`unknown_environment` 错误码与 `quit` 响应后退出。Runtime Release 持续启动 6 秒，日志无 error/critical；未运行测试套件。

## Stage 9: Final Application Composition

### Changes

- 将初始化按依赖顺序拆成短函数或`AppComposition`。
- 将shutdown顺序显式化，避免依赖成员声明顺序猜测。
- Application主循环只保留canonical frame order。
- 将跨模块共享状态改为snapshot/event/action，不暴露可变内部容器引用。
- 清理无用include、前向声明和条件编译块。
- 只有在依赖方向稳定后，再评估拆分：
  - `vkl_renderer_runtime`
  - `vkl_scene_runtime`
  - `vkl_editor`
  - `vkl_runtime_control`
- 不要求每个目录都有独立target；以打破真实依赖环为准。

### Completion Criteria

- `Application.cpp`目标约1,500-2,000行。
- `Application.h`目标不超过约300行，并且不包含UI modal/task内部结构。
- Application成员按Platform、Runtime、Optional Tooling聚合，而不是暴露每个子对象。
- `src/app`不成为资产、编辑器和控制协议的实现目录。

### Stage 9 Completion Record

- `Application` 的所有权按 `PlatformServices`、`RuntimeServices`、`OptionalTooling` 和 `FrameState` 四组收口；没有再引入一个转发所有行为的 `AppComposition` 大对象。
- 初始化拆为平台与 Renderer、Scene Runtime、可选 Tooling 三个明确阶段，主循环只保留 scene/workflow pump、editor/input、snapshot/view/visibility 构建、render/submit 的 canonical frame order。
- 新增幂等 `shutdown()`：先停止外部控制和后台场景工作，再等待 GPU submission 完成，最后按 Editor/Capture、Scene Runtime、frame state、Vulkan platform 的逆依赖顺序销毁。
- Catalog 和 Scene Registry 仍由 `SceneWorkflowController` 单点拥有；Scene Runtime、Editor 和 Runtime Control 的跨模块引用改为只读，写入继续通过 Workflow action/event 完成。
- 删除无调用的 Application wrapper、旧 helper 和迁移后遗留 include；`Application.cpp` 约 1,190 行，`Application.h` 约 64 行，低于原目标且不包含 UI、资产任务或协议内部结构。
- `windows-msvc-dev-fast` Debug 与 `windows-msvc-runtime` Release 构建成功；dev-fast 实际执行 `ping -> quit`，响应成功、进程以 0 退出且新日志无 Validation error。Runtime Release 持续启动 6 秒。按项目策略未运行测试套件。

## Stage 10: Documentation And Closeout

### README

- 使用“Vulkan 1.3 real-time renderer under active development”定位。
- 删除learning project和通用scene editor暗示。
- 列出当前实际能力：RenderGraph、Dynamic Rendering、Bindless、CSM/Point/Spot Shadow、AO backends、SSR、SSGI、DDGI、TAA、Atmosphere、Culling、Native Scene和Cook。
- 将Editor、AssetTool、Runtime Control、RenderDoc、Tracy归入辅助开发能力。
- 增加Current Limitations，避免生产级承诺。

### Current Documentation

- `architecture/overview.md`记录Application作为composition root和新的service边界。
- `architecture/rendering.md`记录feature owner、按需资源和Graph唯一执行路径。
- `architecture/render_graph.md`记录active/resident/retiring资源语义。
- `architecture/resource_loading.md`删除Viking/OBJ/同步SceneFactory描述。
- `architecture/scene_documents.md`删除builtin preview兼容说明。
- `guides/runtime_control.md`改称开发自动化接口并替换Viking示例。
- `guides/renderdoc_validation.md`、`visual_regression.md`、`tracy_profiling.md`使用Renderer Smoke Scene。
- `doc/archive/`保持历史原文。

### Plan Lifecycle

- 完成后将本计划移入`doc/archive/plans/engineering/`。
- 旧`engineering_refactor_plan.md`中被本计划替代的Application部分标记为superseded或在全部完成后归档。

## Diagnostics And Error Handling

- 所有Controller提供typed snapshot，不通过日志反向推断状态。
- Service action返回稳定错误码和用户可读message。
- Editor显示错误；RuntimeControlAdapter将同一错误映射为协议error。
- GPU资源容量或创建失败不得破坏当前World。
- Feature资源创建失败只在允许fallback的功能上禁用该功能；核心HDR/Depth失败仍明确终止初始化。
- Diagnostics至少保留：
  - Scene/Environment load state。
  - AssetRepository state。
  - RenderGraph active/culled nodes。
  - active/resident/retiring bytes。
  - Material binding状态。
  - Shadow/Visibility/Screen-space/GI状态。
  - GPU Pass timings。

## Build And Verification

遵循项目级默认策略：只构建和实际启动，不默认运行CTest、Golden、视觉回归或Validation smoke。

每个Stage至少执行：

1. 构建受影响的`windows-msvc-dev-fast`目标。
2. 实际启动`VulkanLab.exe --project .`，确认可以进入主循环。
3. 对Runtime/feature裁剪有影响时构建并启动`windows-msvc-runtime`。
4. 使用Runtime Control或UI完成该Stage直接涉及的场景/设置操作。
5. 执行`git diff --check`。
6. 明确报告未手动验证的GPU视觉行为。

阶段性人工检查：

- Renderer Smoke Scene、Sheen Chair、Algorithm Playground和Sponza GI Stress可加载。
- Model Preview切换不阻塞device idle。
- Native Scene编辑、保存和重载保持正常。
- Shadow、AO、SSR、SSGI、DDGI、TAA、Atmosphere和Bloom均可独立切换。
- 关闭功能时Graph节点和专用资源消失。
- resize、scene reload、shader切换、无GUI启动和Cooked Runtime正常。

只有用户明确要求时运行完整测试套件。

## Commit Strategy

建议每个提交都保持可构建、可启动：

1. `docs: define renderer consolidation roadmap`
2. `refactor: retire Viking Room and legacy OBJ scenes`
3. `assets: add deterministic renderer smoke scene`
4. `refactor: remove legacy render pass execution paths`
5. `feat: realize render graph resources on demand`
6. `refactor: extract scene runtime coordination`
7. `refactor: centralize project scene workflows`
8. `refactor: centralize render settings and feature ownership`
9. `refactor: extract editor application layer`
10. `refactor: isolate runtime automation control`
11. `refactor: reduce application to composition root`
12. `docs: reposition VulkanLab as a renderer under development`

如果某一阶段规模过大，可以按“接口引入 -> 调用者迁移 -> 旧路径删除”拆为多个提交，但不能长期保留两套行为入口。

## Risks And Mitigations

### Removing Viking Breaks Existing Automation

风险：测试、PowerShell smoke、Runtime Control和文档仍引用Viking。

措施：在同一阶段加入Renderer Smoke Scene并迁移所有Current入口；历史archive不修改。

### Controller Extraction Creates A New God Object

风险：将Application方法整体搬入一个新的巨大Coordinator。

措施：按GPU World生命周期、项目资产工作流、渲染设置、Editor和协议adapter拆分；每个模块公开snapshot/action而不是彼此访问内部字段。

### Lazy Residency Causes Lifetime Bugs

风险：feature切换或resize时descriptor仍引用已销毁image view。

措施：资源退役使用submission serial；descriptor更新只发生在对应frame slot fence完成后；Graph diagnostics记录physical generation。

### Feature Modules Duplicate Shared Infrastructure

风险：每种AO/GI算法重新实现Depth、Normal、Pyramid和Temporal history。

措施：明确共享输入资源contract；算法只拥有自身trace/filter/history输出。

### Runtime Control Drifts From Editor

风险：提取后两边继续各自验证参数。

措施：所有mutation通过typed service action，adapter只负责解析和序列化。

### Broad File Movement Obscures Behavior Changes

风险：大量rename和逻辑修改混在一个diff中。

措施：先引入接口和迁移调用，再单独移动文件；产品文档和行为修改分提交。

## Success Metrics

计划完成时应满足：

- 非archive代码和Current文档中不存在Viking Room、OBJ parser、builtin SceneFactory和Legacy SceneObject路径。
- Application不继承Runtime Control接口，不绘制具体ImGui页面，不执行scene load phase状态机。
- RenderGraph是唯一Pass执行和帧内同步路径。
- inactive算法不驻留其专用render targets。
- 默认800x600本机resident image相较约497 MiB基线显著下降，初始目标不高于约250 MiB。
- Application.cpp约1,500-2,000行，Application.h约300行以内。
- 添加新渲染算法不要求修改Application或Runtime Control协议实现。
- Editor、Runtime Control和Renderer使用相同的Scene/Settings actions。
- dev-fast和runtime均可构建、启动并持续渲染。
- README准确描述“开发中的渲染器”，不暗示完整游戏引擎或生产级编辑器。

## Assumptions

- Vulkan 1.3、Dynamic Rendering、Synchronization2、RenderGraph和Bindless Material继续作为当前架构基础。
- 所有现有算法都属于正式维护的渲染能力，但默认设置和设备capability可以使其inactive。
- Runtime Control继续保留，因为自动化、性能分析和远程调试是开发中渲染器的重要能力。
- Scene Editor继续保留，但定位为渲染测试场景authoring workspace。
- glTF是唯一外部模型运行格式；不继续维护OBJ导入。
- Primitive Model继续用于无外部资产的测试和Scene authoring。
- 用户当前Scene、Environment、`external/ktx`和`imgui.ini`不纳入本计划提交。
