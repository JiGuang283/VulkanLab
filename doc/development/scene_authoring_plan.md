# VulkanLab 可编辑场景与多模型世界实施计划

> Status: Active
> Last verified: 2026-08-02
> Verified against: Scene Authoring Stage 2 working tree

## Summary

VulkanLab 当前可以导入、准备、上传并渲染一个完整 glTF/GLB，但 Catalog、派生纹理缓存、异步加载和运行时 `Scene` 都把这个 glTF 视为最终场景。glTF 内部可以包含多个 node、primitive 和 `KHR_lights_punctual` 灯光，但加载后会被展平为不可编辑的 `SceneObject` 与静态 `SceneLight`；切换 Catalog 条目会整体替换当前 Scene。

本计划把项目演进为可以组合和编辑多个模型实例、相机及独立灯光的场景编辑器，同时保留已经完成的 Validator、Native BC7/KTX2、异步 CPU prepare、增量 GPU upload、Viewport、IBL、阴影、Bloom、诊断和 Cook 基础。

目标数据流为：

```text
外部 glTF/GLB
  -> Validator / Model Import
  -> CatalogModel
  -> PreparedModelData / ModelGpuBuilder
  -> shared ModelAsset

.vkscene.json
  -> SceneDocument
  -> RuntimeWorld
  -> RenderQueue / RenderView
  -> existing Renderer
```

本计划只覆盖场景数据、模型资产共享、编辑工作流、多独立灯光和 Cook/package 适配。暂不纳入 Windows CI、测试基础设施扩建、glTF geometry 优化、厂商 profiler 接入等后续工具链工作。

## Progress

- Stage 0 已完成：`SceneWorkflowController` 持有 Catalog/模型预览 registry 和任务状态；Scenes/Assets 已迁入独立 panel，并通过 snapshot/action 与 Application 交互。
- Stage 1 已完成：新增 `vkl_scene_data`、持久 UUID、Catalog schema v3、`SceneCatalogStore`、规范化 Model Import API 和严格的 SceneDocument schema/原子存储。
- Stage 2 已完成：glTF 准备与 GPU 构建已拆为 `PreparedModelData`、`ModelGpuBuilder`、共享 `ModelAsset` 和按 generation 管理的 `AssetRepository`；模型预览通过临时 `ModelInstance` 继续兼容旧 `Scene`。
- 原生 `.vkscene.json` 仍按计划不进入主列表或运行时；下一未完成阶段是 Stage 3 RuntimeWorld 与 Native Scene Loading。

当前事实说明见[场景数据与 Catalog](../architecture/scene_documents.md)。

## Baseline At Plan Creation

当前实现以[资源加载架构](../architecture/resource_loading.md)和[编辑器 UI 工作区](../guides/editor_ui.md)为准：

- Catalog schema v2 的 `CatalogScene` 直接引用一个 builtin、glTF 或 GLB 源文件。
- `GltfPreparer` 在 worker 中产生 `PreparedSceneData`，其中包含纹理、材质、mesh、展平后的对象矩阵、静态世界空间灯光和建议相机。
- `SceneGpuBuilder` 在主线程增量创建 Texture、Mesh、Material 和 `SceneObject`，完成后一次性发布运行时 `Scene`。
- `Scene` 同时拥有 GPU 资源、扁平 draw object、灯光、bounds 和可选 update callback。
- `SceneObject` 只有 Mesh、Material 和 world matrix，没有稳定 ID、父子关系、局部 Transform 或组件。
- glTF node hierarchy 只在 prepare 期间用于计算最终 world matrix；发布后无法编辑或重新挂接。
- `Scene::bounds()` 只在添加对象时累计。修改对象矩阵不会自动更新 bounds，因此不能直接用于可编辑 Transform。
- RenderView 当前最多上传一盏 Directional 和八盏 Point/Spot；第一盏有效 Directional 可以使用现有方向光阴影。
- Docking、独立 Viewport、Scenes、Assets、Render、Materials 和 Diagnostics 已存在，但没有 Outliner、实体 Inspector、对象拾取、Gizmo 或 Undo/Redo。
- `Application.cpp` 仍承担大量场景工作流与 Panel 逻辑；[工程结构计划](engineering_refactor_plan.md)中未完成的 `SceneWorkflowController` 和 Editor Panel 边界与本计划直接相关。

这些能力说明不需要重写 Renderer 或资产导入器，但必须修正“导入模型等于最终场景”的领域模型。

## Goals

- 区分不可直接修改的 `ModelAsset`、可序列化的 `SceneDocument` 和可渲染的 `RuntimeWorld`。
- 一个 `.vkscene.json` 可以引用多个不同模型，也可以多次实例化同一个模型。
- 相同 ModelAsset 的 Mesh、Material、Texture 和 descriptor 只创建一份 GPU 资源。
- Scene Entity 拥有稳定持久化 ID、父子关系、局部 Transform 和可选组件。
- 编辑器支持 New/Open/Save/Save As、Outliner、Inspector、模型放置、独立灯光和基础层级编辑。
- 关闭并重新打开场景后，实体身份、层级、Transform、模型引用、灯光、相机和环境设置保持一致。
- 添加或删除一个模型实例不销毁和重建整个 RuntimeWorld。
- glTF 内嵌灯光在 ModelAsset 中保持局部定义，并随 ModelInstance 正确实例化。
- 新场景可以创建和编辑 Directional、Point、Spot 灯光；后续阶段将固定 UBO 灯光数组迁移到 SSBO。
- 旧 Catalog、旧派生纹理 manifest、现有 Runtime Control scene ID 和视觉场景入口在迁移期间继续可用。
- Cook 从原生 SceneDocument 计算模型、环境、shader 和派生 blob 的完整闭包。

## Non-Goals

- 不引入完整第三方 ECS；第一版使用项目内 Entity handle 和显式 component store。
- 不实现脚本、游戏逻辑组件、物理、导航、动画编辑器或粒子系统。
- 不实现完整 Prefab override、嵌套 Prefab 或 glTF 内部节点逐项 override。
- 不把 Main Sponza 的每个 primitive 自动展开为 Outliner Entity。
- 不实现多场景同时编辑、World Partition、场景 streaming 或 One File Per Actor。
- 不实现材质编辑、贴图替换或模型几何修改；Material 面板继续以诊断为主。
- 不修改或回写用户的 glTF/GLB、PNG/JPEG 或 HDR 源文件。
- 不在本计划中实现 Point/Spot shadow、shadow atlas、CSM 或所有灯光投影。
- 不把 Forward+ 作为本计划完成条件；SSBO 后是否需要 tiled/clustered culling 由实际灯光规模决定。
- 不新增 CI、Golden、自动测试框架或其它后续工具链阶段。

## Terminology And Ownership

### ModelAsset

`ModelAsset` 是一个导入模型的共享运行时资源：

```cpp
struct ModelAsset {
    ModelAssetId id;
    std::vector<std::shared_ptr<Texture>> textures;
    std::vector<std::shared_ptr<MaterialInstance>> materials;
    std::vector<ModelPrimitive> primitives;
    std::vector<ModelLightPrototype> lights;
    Bounds localBounds;
};

struct ModelPrimitive {
    std::shared_ptr<Mesh> mesh;
    std::shared_ptr<MaterialInstance> material;
    glm::mat4 localToAsset{1.0f};
};
```

第一版不要求把完整 glTF node tree 暴露给编辑器。现有 `PreparedObject::transform` 改为语义明确的 `localToAsset`；RuntimeWorld 收集 draw command 时计算：

```cpp
world = modelInstanceWorld * primitive.localToAsset;
```

glTF 内嵌灯光同样保存相对 ModelAsset 根的 local transform。一个 ModelAsset 被实例化多次时，每个实例产生对应的世界空间灯光实例。

### SceneDocument

`SceneDocument` 是纯 CPU、可版本化、可保存的数据模型，不包含 `Vk*` handle、VMA allocation、descriptor、raw pointer 或 UI widget 状态。

```cpp
struct SceneDocument {
    uint32_t schemaVersion = 1;
    SceneId id;
    std::string displayName;
    std::optional<PersistentEntityId> activeCamera;
    SceneEnvironmentSettings environment;
    SceneAmbientSettings ambient;
    std::vector<SceneEntityDocument> entities;
};
```

Entity 使用两种身份：

- `PersistentEntityId`：128-bit UUID 的规范字符串，写入文件并用于 Undo/Redo、父子关系和跨组件引用。
- `EntityHandle { index, generation }`：RuntimeWorld 内的紧凑句柄；generation 防止删除实体后旧选择误引用复用 slot。

Scene 文件使用扁平 Entity 数组和 `parent` ID，不使用递归 JSON。这样便于稳定排序、原子更新、引用校验和未来 patch/merge。

### RuntimeWorld

`RuntimeWorld` 拥有实体、component store、Transform hierarchy、动态 bounds 和对 ModelAsset 的共享引用，但不拥有同一模型的重复 GPU 副本。

第一批组件固定为：

```cpp
struct TransformComponent {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

struct ModelInstanceComponent {
    ModelAssetId modelId;
    bool visible = true;
    bool instantiateImportedLights = true;
};

struct LightComponent {
    LightType type = LightType::Point;
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    float innerConeRadians = 0.0f;
    float outerConeRadians = 0.785398f;
    bool castsShadow = false;
};

struct CameraComponent {
    float verticalFovRadians = 1.047198f;
    float nearPlane = 0.05f;
    float farPlane = 1000.0f;
};
```

Transform hierarchy 使用 dirty propagation。局部 TRS 变化后，只重算受影响的 world matrix、ModelInstance world bounds 和全局 scene bounds。Renderer 继续只消费最终矩阵和 bounds。

### AssetRepository

`AssetRepository` 是 ModelAsset 生命周期和去重边界：

```cpp
AssetHandle<ModelAsset> requestModel(ModelAssetId id,
                                    ImportProfileId profile);
ModelAssetStatus queryModel(ModelAssetId id) const;
void releaseUnused(SubmissionSerial completedSerial);
```

Repository 以 `modelId + profileId` 为 key，管理：

- `Unloaded / PreparingCpu / ReadyForUpload / Uploading / Ready / Failed` 状态。
- 合并相同模型的重复请求。
- worker 产生 `PreparedModelData`，主线程使用现有 IncrementalUploadQueue 创建 GPU 资源。
- 最后一个 RuntimeWorld/Entity 引用释放后，按 FrameSync submission serial 延迟销毁资源。
- 编辑当前 World 时不调用 `vkDeviceWaitIdle()`，也不清空无关 Pipeline Cache。

## File Formats

### Catalog Schema v3

Catalog 将模型、可编辑场景和环境分开：

```json
{
  "schemaVersion": 3,
  "projectId": "vulkan-lab",
  "defaultImportProfile": "desktop_1024",
  "models": [
    {
      "id": "main-sponza",
      "displayName": "Main Sponza",
      "type": "gltf",
      "source": "models/main_sponza/NewSponza_Main_glTF_003.gltf",
      "importProfile": "desktop_1024",
      "optional": true
    }
  ],
  "scenes": [
    {
      "id": "sponza-lighting",
      "displayName": "Sponza Lighting",
      "source": "assets/scenes/sponza_lighting.vkscene.json",
      "optional": false
    }
  ],
  "environments": []
}
```

规则：

- `models` 使用现有 glTF Validator、import profile、Native BC7 和 ArtifactIndex 管线。
- `scenes` 只引用项目内 `.vkscene.json`，不直接携带 texture profile。
- SceneDocument 中的 `modelId` 必须解析到同一 Catalog 的 Model。
- SceneDocument 中的 environment ID 必须解析到 CatalogEnvironment 或 `null`。
- 路径继续相对 ProjectContext project root，拒绝逃逸。

### Scene Schema v1

建议目录为 `assets/scenes/<scene-id>.vkscene.json`：

```json
{
  "schemaVersion": 1,
  "id": "sponza-lighting",
  "displayName": "Sponza Lighting",
  "activeCamera": "b4a3f624-6c16-4bad-a031-f21813c7b4a1",
  "ambient": {
    "color": [1.0, 1.0, 1.0],
    "intensity": 0.08
  },
  "environment": {
    "environmentId": "kloppenheim-05",
    "intensity": 1.0,
    "rotationRadians": 0.0,
    "iblEnabled": true,
    "skyboxEnabled": true
  },
  "entities": [
    {
      "id": "85e213b8-c2a9-4e16-a7fe-583fcf804838",
      "name": "Sponza",
      "parent": null,
      "enabled": true,
      "transform": {
        "translation": [0.0, 0.0, 0.0],
        "rotation": [0.0, 0.0, 0.0, 1.0],
        "scale": [1.0, 1.0, 1.0]
      },
      "components": {
        "modelInstance": {
          "modelId": "main-sponza",
          "visible": true,
          "instantiateImportedLights": true
        }
      }
    },
    {
      "id": "c78e5a3b-3034-46d0-aa1c-372275763b83",
      "name": "Fill Light",
      "parent": null,
      "enabled": true,
      "transform": {
        "translation": [2.0, -3.0, 4.0],
        "rotation": [0.0, 0.0, 0.0, 1.0],
        "scale": [1.0, 1.0, 1.0]
      },
      "components": {
        "light": {
          "type": "point",
          "color": [1.0, 0.82, 0.65],
          "intensity": 20.0,
          "range": 12.0,
          "castsShadow": false
        }
      }
    }
  ]
}
```

持久化规则：

- Translation/scale 使用 world units，rotation 固定为 `[x, y, z, w]` quaternion。
- 颜色存储线性 RGB；灯光 intensity 继续使用当前 Renderer/glTF 语义。
- 未知 component 保留原始 JSON 或给出明确 schema 错误；第一版不静默丢弃再覆盖文件。
- UUID 必须唯一；parent 必须存在或为 `null`；加载时检测 parent cycle。
- 保存时按稳定 Entity 顺序和稳定 component key 输出，减少无意义 diff。
- 使用临时文件、flush 和 atomic replace；失败时保留上一个有效场景。
- Editor viewport camera、当前选择、Panel 布局、Shader variant、Texture Limit、Validation profile 和诊断设置不进入 SceneDocument。

## Compatibility Strategy

Catalog、cache 和控制协议需要渐进迁移，不能要求用户一次性重新导入全部模型：

1. Catalog loader 继续读取 schema v1/v2。
2. 旧 `scenes[]` 条目在内存中解释为 `CatalogModel`，并为 UI/Runtime Control 合成一个只包含该模型的 read-only preview scene。
3. `Convert to Editable Scene` 创建一个新的 `.vkscene.json` 和 Catalog v3 scene entry，不修改原 glTF，也不立即删除旧入口。
4. 旧 texture manifest 中的 `sceneId` 按相同字符串解释为 `modelId`；命中已有 Native BC7 blob 时不重新编码。
5. ArtifactIndex 下一 schema 增加真正的 `Model` kind；旧 `Scene` kind 记录在迁移读取时映射为 Model。
6. Validator receipt/index 从 scene ID 迁移为 model ID，旧命令和字段保留兼容 alias，并记录 deprecated 日志。
7. Runtime Control `scene.load <legacy-id>` 在兼容期仍打开合成 preview scene；原生 SceneDocument 使用稳定 scene ID。
8. 只有显式保存/转换操作才写 Catalog v3；普通启动不得自动改写用户 Catalog。

## Stage 0: Responsibility Boundaries

### Scope

- 从 `Application.cpp` 提取 `SceneWorkflowController`，负责 New/Open/Close/Save、加载 generation、当前 Scene 状态和错误传播。
- 新增 `SceneDocumentService`，先提供 data-only 接口和空实现，不在本阶段改变当前加载行为。
- 将 Scenes、Assets 和后续 Outliner/Inspector Panel 绘制拆到 `src/editor/panels/`；Panel 只能发出 action，不能直接创建/销毁 Vulkan 对象。
- 抽取 `ModelAssetId`、`SceneId`、`PersistentEntityId`、Catalog DTO 等 data-only header，避免 assets/scene/editor 循环 include。
- 建立 `EditorAction`/controller 入口，让 ImGui 和 Runtime Control 不复制 scene command 逻辑。

### Invariants

- 当前 Catalog 条目、glTF 加载、场景切换和画面完全不变。
- Application 仍是 composition root 和主线程 Vulkan owner。
- 不在本阶段更改 Catalog、cache 或 package schema。

### Exit Criteria

- `Application.cpp` 不再实现 Scenes/Assets Panel 的详细控件和 scene command switch。
- 旧 Viking Room、Main Sponza 和其它 Catalog 条目可按原方式加载。
- Debug 和 `windows-msvc-dev-fast` 能构建并启动。

## Stage 1: Catalog v3 And SceneDocument

### Scope

- 增加 `CatalogModel` 和 `CatalogSceneDocument`，Catalog schema 升级为 v3。
- 实现 v1/v2 兼容读取和 v3 确定性写入，不自动重写旧 Catalog。
- 将现有 glTF 导入 UI/服务语义改为 `Import Model`；旧 API 名称暂时作为 adapter。
- 实现 Scene schema v1 parser、validator、deterministic serializer 和 atomic save。
- 增加 New Scene 模板：一个明确的 Sun Entity、一个 Camera Entity、默认 ambient 和 `None` environment。
- 增加旧 Catalog model 到单实体 SceneDocument 的内存 adapter。
- 更新 ArtifactIndex/DerivedTextureManifest/Validator identity 的兼容读取规则，但本阶段不移动现有 blob。

### Exit Criteria

- 可以在无 Vulkan 环境下解析、验证并重新序列化 `.vkscene.json`。
- v2 Catalog 不经写回即可产生与当前相同的模型列表和 preview scene。
- 新建并保存空场景后，Catalog 中出现独立 Scene entry。
- 修改源模型不会改变 SceneDocument；重命名 display name 不改变稳定 ID。

## Stage 2: Shared ModelAsset And AssetRepository

### Scope

- 将 `PreparedSceneData` 的 glTF 内容迁移为 `PreparedModelData`；保留 typedef/adapter 供旧同步入口过渡。
- 将 `SceneGpuBuilder` 的资源构建部分迁移为 `ModelGpuBuilder`，输出共享 `ModelAsset`。
- 新增 `AssetRepository`，按 model/profile 去重 CPU prepare、GPU build、状态和失败结果。
- 将 glTF primitive transform 定义为 `localToAsset`，将内嵌 light 定义保存在 ModelAsset local space。
- ModelAsset 拥有 Texture、Mesh、Material 和 descriptor；World/Entity 只持有 AssetHandle。
- 支持同一 ModelAsset 的多个实例；实例销毁不立即销毁共享资源。
- GPU resource 释放接入 FrameSync submission serial，避免编辑操作调用 device idle。
- 旧“一 glTF 一 Scene”入口通过创建一个 ModelAsset 和一个临时 ModelInstance 继续工作。

### Loading Semantics

- CPU prepare 继续只在 worker 中访问文件和纯 CPU 数据。
- Vulkan/VMA/descriptor 和 upload 继续由主线程负责。
- 同一 model/profile 的并发请求合并为一个 task。
- 添加模型期间当前 World 继续渲染；未 Ready 的实例在 Outliner 显示 Loading，第一版可不绘制占位 geometry。
- 单个模型失败只标记对应 AssetHandle/Entity，不清空整个 RuntimeWorld。

### Exit Criteria

- 一个临时 World 中放置两个相同模型实例时，GPU Texture/Mesh/Material 只创建一份。
- 两个实例使用不同 root Transform 并生成不同 RenderCommand world matrix。
- 删除一个实例后另一个实例继续正常渲染。
- 旧 Native BC7 cache 继续命中，且不因 Scene/Model 术语迁移重新编码。

## Stage 3: RuntimeWorld And Native Scene Loading

### Scope

- 新增 RuntimeWorld、Entity registry、generation handle 和显式 component stores。
- 从 SceneDocument 建立 Entity hierarchy，检测重复 UUID、missing parent 和 cycle。
- 实现 local/world Transform dirty propagation。
- 根据 ModelAsset local bounds 和 Entity world matrix维护实例 bounds 与完整 World bounds。
- RenderQueue 从 RuntimeWorld 的 ModelInstance 收集 draw commands，不再从扁平 `SceneObject` 直接读取。
- RenderView 从显式 LightComponent 和 ModelAsset imported-light instances 收集 SceneLight。
- Camera Entity 可以成为 active camera；没有 active camera 时使用 editor camera，不把 editor camera写回 SceneDocument。
- Environment/Ambient 从 SceneDocument 应用到现有 Environment loader 和 RenderSettings 接口。
- 新增 native scene open task：解析文档、收集唯一 model IDs、请求 AssetRepository、等待必要资源并发布 RuntimeWorld。
- 旧 `Scene` 保留为过渡 facade，直到 Renderer/Application 全部消费 RuntimeWorld。

### Scene Switch Policy

- CPU 文档解析和 ModelAsset 查询期间继续渲染当前 World。
- 如果目标 Scene 所需 ModelAsset 已驻留，直接创建新 RuntimeWorld，不重复上传。
- 需要大量新 GPU 资源且预算不足时，可以在开始 GPU build 前释放旧 World 的独占引用；共享 ModelAsset 不重复释放。
- 新 generation 才能成为 current World；旧任务完成不得覆盖更新场景。
- 文档失败时保留当前 World；单个 optional model 失败时由 Scene 打开策略决定阻止发布或保留 error entity，第一版默认阻止完整 Scene 发布并报告 model ID。

### Exit Criteria

- 一个原生 SceneDocument 能同时显示至少两个不同 ModelAsset 和同一模型的两个实例。
- 父 Entity 移动会更新全部 child world matrices、bounds、透明排序和方向光阴影拟合。
- imported glTF light 随 ModelInstance root Transform 正确移动和旋转。
- Scene 切换、取消、resize 和退出不产生 stale AssetHandle。

## Stage 4: Scene Authoring UI v1

### Scope

- DockSpace 增加 `Outliner` 和 `Inspector`；Assets 窗口演进为可搜索的 Model/Environment Browser。
- 增加 File/Scene 命令：New、Open、Save、Save As、Close、Convert Legacy Preview。
- Outliner 支持选择、重命名、启用/禁用、创建 Empty/Model/Directional/Point/Spot/Camera、删除、复制和 parent reassign。
- Inspector 支持 Transform、ModelInstance、Light、Camera 和 Scene settings。
- Model picker 只保存 model ID；不会把文件绝对路径写入 SceneDocument。
- 新增 `EditorCommandStack`；所有可编辑操作通过 `apply()/undo()` 修改 RuntimeWorld 和 SceneDocument mirror。
- Scene 使用 edit generation/clean generation 维护 Dirty 状态；切换或退出时对未保存修改给出 Save/Discard/Cancel modal。
- Materials 面板改为显示选中 ModelInstance 的 ModelAsset materials；仍保持只读。
- imported model 内部 primitive/node 第一版不出现在顶层 Outliner，仅显示一个 ModelInstance root。

### Independent Lights v1

- 新建场景使用显式 Sun Entity，fallback Sun 只服务旧 preview scene或没有有效灯光的兼容情况。
- Light direction 由 Entity world rotation 的 local `-Z` 计算，与 glTF spot/directional 约定一致。
- Point/Spot position 来自 Entity world translation。
- UI 显示当前 GPU 上限、已上传和被忽略数量。
- 本阶段仍使用现有 `1 directional + 8 punctual` GPU ABI；超过上限的实体保留在 SceneDocument，但明确显示未参与渲染。
- 只有选定的首个有效 Directional 使用现有 shadow map；Point/Spot 的 `castsShadow` 在 v1 中显示 Unsupported，不产生错误实体阴影。

### Exit Criteria

- 用户可从空场景放置不同模型、复制实例、创建三类灯光并编辑数值 Transform。
- Undo/Redo 覆盖创建、删除、重命名、Transform、parent、模型引用和灯光参数。
- Save 后重启程序再打开，所有实体和设置一致。
- 删除 Entity 不会让 Inspector、选择或 Undo 栈持有悬空指针。

## Stage 5: Viewport Selection And Manipulation

### Scope

- 从 Viewport 图像区域生成 camera ray，第一版使用 ModelInstance world AABB 完成实体级选择。
- 空白点击清除选择；Ctrl 增加/移除多选可以延后，v1 只要求单选。
- 引入固定版本、仅 Editor 构建使用的 ImGuizmo，提供 Translate/Rotate/Scale 和 Local/World 模式。
- Gizmo 操作开始时创建一个 transaction，拖动期间预览，释放时生成一个可撤销 EditorCommand，避免每个鼠标事件占用 Undo entry。
- 从 Assets Browser 拖放 Model 到 Viewport；使用 camera ray 与地面或当前焦点平面求交创建 Entity。
- 支持 Outliner drag/drop reparent，并默认保持世界 Transform。
- Viewport toolbar 使用图标控制选择和 Gizmo mode；输入只在实际 Viewport image rect 内生效。
- 选中实体使用 Gizmo 和轻量 bounds overlay 标识；第一版不增加完整 outline postprocess。
- 当 AABB picking 无法满足 primitive/重叠对象选择时，再增加 `R32_UINT` object-ID pass；该 pass 不是 v1 必需条件。

### Exit Criteria

- 从 Asset Browser 拖入两个模型，可以在 Viewport 选择并移动、旋转、缩放。
- Viewport camera 操作、Dock resize、Gizmo 和 ImGui 控件不会争抢鼠标。
- Gizmo 编辑可 Undo/Redo，保存后结果一致。
- 隐藏/恢复 Viewport 或 scene reload 后选择状态不会引用已销毁 Entity。

## Stage 6: Scalable Scene Lights

### Scope

- 将 variable-length scene light list 从 `GlobalFrameUbo` 固定数组迁移到 per-frame storage buffer。
- Global UBO 只保存 camera、ambient、environment、shadow 和灯光计数；`GpuLight` ABI 保持 16-byte aligned。
- Frame-global descriptor 增加 storage buffer binding，PBR Shader 按 Directional/Point/Spot count 遍历。
- Renderer 根据当前 Scene 灯光数量增长 buffer capacity，设置明确的开发上限，例如 256；不每帧重新分配。
- Legacy 和 Debug variants 明确声明是否消费 scene-light buffer；Shader contract 同步更新。
- Light Inspector 增加 enabled、type、color、intensity、range、cone 和 shadow support 状态。
- imported lights 与 editor-created lights 使用同一上传路径和统计。
- 方向光阴影选择规则变为稳定字段：优先 `castsShadow=true` 的第一盏有效 Directional，再按 Entity UUID 稳定排序；没有时禁用 shadow。

### Performance Boundary

- 第一版 Forward Shader 直接遍历 active punctual lights，目标是可靠支持几十盏独立灯光。
- 当实际场景需要上百灯光且 GPU profile 证明 fragment light loop 为瓶颈时，再单独规划 Forward+ tiled/clustered culling。
- 不为了“支持 256 个存储项”宣称 256 盏全屏灯光都具有稳定性能。

### Exit Criteria

- 场景可保存并渲染超过八盏 Point/Spot，而不再静默截断到固定 UBO 数组。
- 调整任意独立灯光 Transform 或参数在下一帧生效。
- 无灯光、只有 imported lights、只有 editor lights 和混合场景行为一致。
- 现有单方向光阴影和 fallback 兼容场景保持工作。

## Stage 7: Cook, Package And Runtime Integration

### Scope

- Cook 的顶层选择从“glTF scene ID”改为 native SceneDocument ID。
- Cook 解析 SceneDocument closure：ModelAsset IDs、Environment ID、import profile、model source/buffer、Native BC7 manifests/blobs、environment KTX2、shader 和 scene document。
- 相同 ModelAsset 被多个 Entity 或多个 cooked Scene 引用时只打包一份资源和派生 blob。
- ArtifactIndex schema 增加 `Model / Environment / SceneDocument` 三种 kind；旧 `Scene` kind 迁移为 Model。
- Cooked Catalog 只保留被选中的 scenes、它们引用的 models/environments 和所需 profiles。
- Cooked runtime 第一版可以直接读取经 package hash 验证的 `.vkscene.json`；自定义二进制 Scene 格式不作为本计划前置条件。
- Editor UI、ImGuizmo、Scene 保存和模型导入不进入 `windows-msvc-runtime` 或 Cooked package。
- Runtime Control 的 `scene.list/current/load/reload` 转为 native Scene ID；兼容 preview scene 在开发模式继续可用。
- `render.status` 增加 current scene document ID、dirty=false、entity/model/light count 和 model load status；Cooked runtime不暴露 editor mutation 命令。

### Exit Criteria

- 选择一个包含多个 ModelAsset 和 Environment 的 native Scene 后可以成功 Cook。
- 包内不包含未引用模型、源 PNG/JPEG/HDR、编辑器状态或开发工具。
- Cooked VulkanLab 能打开 SceneDocument、共享重复 ModelAsset，并渲染独立灯光。
- package verify 能检测 scene/model dependency 缺失或被修改。

## Editor And Runtime State Separation

以下状态属于 SceneDocument：

- Entity ID、名称、enabled、parent 和 Transform。
- ModelInstance、Light、Camera component。
- Active Camera、ambient、environment 选择/强度/旋转及 IBL/Skybox 开关。

以下状态属于用户/编辑器会话：

- Viewport editor camera、当前 selection、Outliner filter。
- Dock layout、Panel 显隐和 Gizmo mode。
- Shader variant、Texture Limit、Validation profile。
- Capture、RenderDoc、Tracy 和 Diagnostics 设置。

Exposure、Tone Mapper、Bloom 和 shadow bias 第一版继续作为 RenderSettings 会话状态。需要按场景保存时，应以后增加明确的 Camera/PostProcess component，而不是把 Application 全局状态直接序列化进 SceneDocument。

## Cross-Stage Invariants

- Worker thread 不访问 Vulkan、GLFW、ImGui、RuntimeWorld mutable state 或 Editor selection。
- Vulkan/VMA/descriptor 创建销毁继续在主线程完成。
- SceneDocument、PreparedModelData 和 Catalog DTO 不包含运行时 GPU handle。
- ModelAsset GPU 资源由 AssetRepository 唯一拥有；Entity 不拥有 Texture/Mesh/Material 生命周期。
- Renderer 不读取 JSON、Catalog 或 editor command；只消费 RuntimeWorld 生成的 RenderQueue/RenderView。
- 删除 Entity、撤销命令、切换 Scene 和完成旧异步任务都必须通过 UUID/generation 校验。
- glTF/GLB 和派生 cache 都是模型资产；`.vkscene.json` 才是可编辑场景。
- 任何 schema 写入使用 temporary file + atomic replace。
- 兼容读取不得在普通启动时隐式改写用户 Catalog、scene 或 cache。

## Verification Plan

遵循项目级开发速度策略，每阶段默认只进行受影响配置的编译和实际启动，不运行 CTest、Golden、视觉回归或 Validation smoke；只有用户另行要求时才执行测试套件。

### Per Stage

1. 构建 `windows-msvc-dev-fast` 并启动 VulkanLab。
2. 涉及 Cook/runtime 裁剪时额外构建并启动 `windows-msvc-runtime`。
3. 确认旧 Viking Room、Main Sponza 和至少一个 glTF 灯光场景仍可打开。
4. 检查日志没有重复 ModelAsset 上传、stale task publish 或资源生命周期错误。
5. 执行 `git diff --check`。

### Final Manual Scenario

1. 创建一个空 SceneDocument。
2. 放置 Main Sponza、Sheen Chair 和两个 CarConcept 实例。
3. 给两个 CarConcept 设置不同 Transform，确认共享同一 ModelAsset GPU 资源。
4. 创建一盏 Directional、两盏 Point 和两盏 Spot，分别调整位置、方向、颜色、强度和范围。
5. 建立一个父子层级并移动父实体，确认模型、灯光、bounds、透明排序和 shadow fit 更新。
6. 保存、关闭、重新打开，确认 UUID、层级、组件和环境完全一致。
7. 使用 Undo/Redo 覆盖创建、删除、Transform 和灯光修改。
8. 从旧 Catalog model 创建 preview scene并转换为 editable scene，确认原模型和 BC7 cache 未改变。
9. Cook 该场景并用 runtime build 打开，确认依赖闭包没有重复或遗漏。

## Risks And Mitigations

| Risk | Mitigation |
|---|---|
| `Scene` 术语在 Catalog/cache/runtime 中含义冲突 | 先引入 Model/Scene 新类型和兼容 adapter，再分阶段改名；不做一次性全仓库替换。 |
| 添加一个模型仍触发整场景销毁 | AssetRepository 将任务和资源生命周期下沉到 ModelAsset；World 只增删引用。 |
| 相同模型实例重复占用显存 | Repository 以 model/profile 稳定 key 去重，并在 Stats 显示 unique assets 与 instances。 |
| Transform 编辑后 bounds/shadow/透明排序过期 | Transform dirty propagation 同时失效 instance/world bounds；RenderQueue 每帧读取最终 world matrix。 |
| glTF 内嵌灯光被错误当作世界空间常量 | PreparedModelData 保存 local-space light prototype，实例收集时乘 ModelInstance world transform。 |
| 旧 KTX cache 因字段改名全部失效 | 兼容读取把旧 sceneId 映射为 modelId，cache key 的源内容/语义/profile 部分保持不变。 |
| Undo/Redo 引用已删除对象 | 命令只保存 UUID 和值快照；执行时通过 RuntimeWorld generation 重新解析。 |
| Application 继续膨胀 | Stage 0 先建立 controller、document service 和独立 panels，后续 UI 只调用 action。 |
| Scene 文件被半写或未知字段丢失 | 原子保存；未知 schema/component 不允许在未确认迁移时覆盖原文件。 |
| 灯光数量提高后 Forward 性能下降 | SSBO 先解决容量和正确性；达到真实性能门槛后再独立实施 Forward+。 |
| Cook 仍按旧 glTF scene 计算闭包 | 最终 Cook 入口必须从 SceneDocument 引用图遍历，旧 preview 只作为开发兼容入口。 |

## Delivery And Commit Strategy

建议保持每个提交可构建、可启动，并按以下边界交付：

1. `refactor: isolate scene workflow and editor panels`
2. `feat: split catalog models from scene documents`
3. `feat: add versioned scene document serialization`
4. `refactor: build shared model assets through repository`
5. `feat: load multi-model runtime worlds`
6. `feat: add scene outliner inspector and command history`
7. `feat: add viewport entity manipulation`
8. `feat: upload scalable scene lights through storage buffers`
9. `feat: cook native scene document dependencies`
10. `docs: document scene authoring and runtime world architecture`

每个阶段完成后更新本计划 Progress；完整完成后将计划移动到 `doc/archive/plans/`，并在 `doc/architecture/` 与 `doc/guides/` 中写入最终实现，而不是继续把计划当作当前事实来源。

## Completion Criteria

本计划完成必须同时满足：

- Catalog 明确区分 Model、SceneDocument 和 Environment。
- `.vkscene.json` 是可创建、编辑、保存、重载和 Cook 的正式场景格式。
- 一个场景可以包含多个模型和同一模型的多个实例。
- 重复实例共享 ModelAsset GPU 资源，编辑实例不会触发整场景重建。
- Entity 具有稳定 UUID、父子层级、Transform 和组件，bounds 能随编辑更新。
- Outliner、Inspector、Viewport manipulation 和 Undo/Redo 可以完成基础场景搭建。
- Directional、Point、Spot 都可以作为独立 Scene Entity 编辑；灯光容量不再固定为八盏 punctual。
- glTF imported lights 与 editor-created lights 使用一致的 RuntimeWorld/RenderView 数据流。
- 旧 Catalog、旧派生缓存和 legacy preview scene 在迁移期间可用。
- Cooked package 能按 SceneDocument 依赖闭包运行，并排除编辑器和未引用资源。

## Assumptions

- 第一版只支持一个当前 SceneDocument 和一个 Scene Viewport。
- 模型源资产不可变；内部节点编辑和 Prefab override 延后。
- Entity component 类型在 v1 中是固定集合，不支持插件动态注册。
- RuntimeWorld 使用项目内轻量 component stores，不引入通用 ECS 框架。
- 新场景默认包含显式 Sun 和 Camera；fallback Sun 只保留兼容意义。
- 多独立灯光首先保证编辑、保存和正确渲染；Point/Spot shadow 和 Forward+ 不属于完成门槛。
- SceneDocument 第一版使用可读 JSON；只有加载或包体数据证明需要时才增加二进制 cooked scene。
- 本计划暂不安排后续工具链工作；现有诊断能力只作为开发观察手段复用。
