# 场景数据与 Catalog

> Status: Current
> Last verified: 2026-08-02
> Verified against: Scene Authoring Stage 7 implementation

VulkanLab 已将“导入模型”“可保存场景”和“运行时世界”拆成三个领域对象。模型预览与 Native Scene 都通过 `AssetRepository` 共享 `ModelAsset`；`.vkscene.json` 由 `RuntimeWorld` 实例化，并可在编辑器中修改、撤销和原子保存。

## 数据分层

```text
CatalogModel
  -> glTF/GLB 或 builtin
  -> 单模型预览 SceneEntry

CatalogSceneDocument
  -> assets/scenes/<id>.vkscene.json
  -> Entity/Transform/Component DTO
  -> RuntimeWorld
  -> RenderQueue / RenderView
```

`vkl_scene_data` 是独立静态库，提供持久 ID 与 SceneDocument DTO、解析、验证和原子存储。它不依赖 Renderer、Vulkan、ImGui、`vkl_engine` 或 `vkl_asset_core`；`vkl_asset_core` 和 `vkl_engine` 反向依赖它。

## Catalog schema v3

Catalog v3 的顶层资产集合为：

```json
{
  "schemaVersion": 3,
  "models": [],
  "scenes": [],
  "environments": []
}
```

- `models[]` 保存 builtin 或 glTF/GLB 模型、导入 profile、optional 状态和可选 `previewCamera`。
- `scenes[]` 保存原生场景 ID、显示名和项目内 `.vkscene.json` 路径。
- `environments[]` 保持现有 HDR/IBL 语义。
- Model ID 与 SceneDocument ID 在同一个命名空间内必须唯一。

schema v1/v2 中的 `scenes[]` 仍按旧语义读取为 `CatalogModel`，旧 `camera` 映射为 `previewCamera`。只读启动不会修改旧文件；保存预览相机、导入或删除资产等显式 Catalog 修改会通过 `SceneCatalogStore` 将完整文件确定性升级到 v3。

`SceneCatalogStore` 是唯一类型化写入口。它在写入前读取 file stamp，并用修改后的模型/环境集合重新校验所有已登记 SceneDocument 引用；随后在同目录创建临时文件、重新解析验证，并仅在源 Catalog 未被外部修改时原子替换。模型导入、Catalog editor 和环境操作不再各自拼接原始 JSON。

## 持久 ID

`PersistentEntityId` 是 128 位 UUID。生成使用系统随机源并设置 UUID v4 version/variant；文本表示统一为小写 `8-4-4-4-12` 格式。解析接受合法十六进制文本，重新序列化时规范化为小写。

`ModelAssetId` 与 `SceneDocumentId` 是稳定 asset ID 的强类型包装，继续使用项目现有的小写字母、数字、连字符和下划线规则。Entity UUID 用于场景内部引用，asset ID 用于 Catalog 和派生资产引用，二者不能互换。

## SceneDocument schema v2

场景文件位于 `assets/scenes/<scene-id>.vkscene.json`。顶层保存场景 ID、显示名、active camera、ambient、可选 environment 和保持原顺序的 entity 数组。Entity 使用扁平数组与 parent UUID：

```json
{
  "id": "<uuid>",
  "name": "Entity",
  "parent": null,
  "enabled": true,
  "transform": {
    "translation": [0, 0, 0],
    "rotation": [0, 0, 0, 1],
    "scale": [1, 1, 1]
  },
  "components": {}
}
```

当前 DTO 定义并运行三类组件：

- `modelInstance`：引用一个 Catalog model ID。
- `light`：Directional、Point 或 Spot 及其颜色、强度、range、cone 和 `castsShadow`。
- `camera`：透视相机的垂直 FOV、near 和 far。

解析为严格模式：未知顶层字段、未知 Entity 字段或未知 component 都会失败，避免读写后静默丢失未来数据。UUID 必须唯一；parent 必须存在且层级无环；active camera 必须引用 Camera entity；Transform、Light 和 Camera 数值必须有限且在有效范围内；scale 分量不能接近零。Quaternion 在加载时验证并规范化。

schema v1 仍可读取：旧 Directional 默认 `castsShadow=true`，Point/Spot 默认 `false`；加载后内存文档规范化为 v2，下一次保存确定性写出 v2。当前只有 Directional 可以设置 `castsShadow=true`，Point/Spot 设置该字段会被验证器拒绝。

不传 Catalog references 时允许模型和环境暂时未解析，便于独立编辑文件。将文档加入 Catalog 时，`SceneCatalogStore` 会使用当前 models/environments 重新加载并校验所有引用。

## 创建与保存

`SceneDocumentService::createDefault()` 创建 Camera、Directional Sun、默认 ambient 和空 environment，不自动加入 ModelInstance。

默认 Camera 位于 `(0,-5,2)` 并朝向原点。Directional Light 的实体局部 `-Z` 是发射方向；提交给 Renderer 时转换为从表面指向光源的方向。Camera 使用实体 world translation、局部 `-Z` forward 和局部 `+Y` up，方向只组合层级 rotation，不受 scale 影响。

`saveAtomic()` 只接受项目根目录内且以 `.vkscene.json` 结尾的路径。保存前会重新验证；调用者必须传入加载时的 file stamp 才能覆盖已有文件。磁盘内容被外部修改时返回 `scene_changed_on_disk`，临时文件会清理，旧文件保持不变。`SceneCatalogStore::addSceneDocument()` 只能在场景文件已经成功保存后调用。

## 工作流与 UI 边界

`SceneWorkflowController` 持有 Catalog、模型预览 registry、选择状态和导入/派生资产任务状态，不依赖 ImGui 或 Vulkan。Application 注入加载、相机和窗口相关 action，并继续拥有实际 Vulkan Scene、GPU builder 与 Renderer。

`ScenesPanel`、`AssetsPanel`、`OutlinerPanel` 和 `InspectorPanel` 位于 `src/editor/panels/`。它们只接收 snapshot 与回调 action，只保存搜索文本、modal 草稿和选中项等临时 UI 状态。

`SceneEditorSession` 持有当前 `RuntimeWorld`、文档路径/file stamp、UUID selection、Editor/Active Camera 模式和最多 256 项的命令栈。`RuntimeWorld` 是编辑期间唯一内存真源；保存时调用 `toDocument()`。命令只保存 before/after `SceneDocument`，不保存 `EntityHandle`、裸指针或 GPU 资源。连续拖动在激活时捕获一次 before，并在释放时提交一个命令。

`SceneViewportController` 是 Editor-only 的空间交互入口。它根据当前实际 Viewport image rect 和 Editor/Active Camera 生成射线，使用 `ModelAsset::localBounds` 完成实体级 CPU picking，并通过 ImGuizmo 修改选中实体的 world matrix。修改结果统一转换回 local TRS，再进入 `SceneEditorSession` 的连续事务，因此不会在 SceneDocument 中引入矩阵或 editor-only 字段。

`RuntimeWorld::setParent()` 支持 Keep Local 与 Keep World。Inspector parent picker 使用 Keep Local；Outliner drag/drop 使用 Keep World。Keep World 在修改层级前完成 inverse-parent 与 TRS 分解校验，拒绝 perspective、近零 scale、非有限矩阵和无法表示的 shear，失败时不修改 world。

Native Scene 加载事务先在 worker 解析文档，再按 Catalog profile 向 `AssetRepository` 请求唯一模型集合，并等待模型与 environment 全部 Ready。新 World 完整构造后才原子发布；失败或取消时旧 World 与旧 environment 保持可用。旧 World 和共享资源按 submission serial 延迟释放，不调用 `vkDeviceWaitIdle()`。

## Cook 与发布运行时

Stage 7 以 Native SceneDocument 作为 Cook root。`CookClosureResolver` 从 Entity 的 `modelInstance` 与顶层 environment 收集唯一依赖；每个模型使用自己的 Catalog `importProfile`，重复实例只打包一个 Model artifact。builtin/OBJ 不能进入闭包，Validator Error、缺失/过期 artifact 或非 Native BC7 profile 都会阻止发布。

Cook 会将旧 v1 文档规范化为 schema v2 写入 staging，但不修改项目源文件。最小 cooked Catalog 只包含已选 SceneDocuments 及其引用的 Models、Environments 和 profiles；包内 Artifact Index 使用 `Model / Environment / SceneDocument` 三类 record，并在 SceneDocument record 中保存精确 asset references。

schema v3 package manifest 保存有序 `sceneIds` 和 `startupSceneId`。Cooked runtime 只注册这些 Native Scene，启动后通过现有异步事务构建 `RuntimeWorld`；相同 model ID 的多个 Entity 继续由 `AssetRepository` 共享同一 generation 和 GPU 资源。旧 schema v1/v2 单模型包保留 Model Preview 兼容路径。

## 当前限制

- 一次只打开一个 Native Scene 编辑会话；模型预览仍作为独立兼容入口。
- Viewport picking 只使用 ModelAsset bounds，暂不支持 primitive picking、重叠对象循环选择或 GPU Object-ID Pass。
- Gizmo 暂不支持 snapping；编辑器仍只支持单选和一个 Scene Viewport。
- builtin/OBJ 模型不能放入 Native Scene；Viking Room 继续使用 legacy preview。
- Directional、Point 和 Spot 共享 256 盏有效灯光上限。超限 Light Entity 仍会保存，并由 RenderView 的精确上传结果在 Outliner/Inspector 标记为未上传；当前 Forward shader 会直接遍历所有已上传灯光。
- Native Scene 使用各 Catalog model 的 import profile；全局 Texture Limit 仅影响模型预览。
- Validator index、KTX2 manifest、ArtifactIndex 和 Runtime Control 中既有 `sceneId` 字段保持不变；在模型资产路径中它是 `modelId` 的兼容序列化名称，不触发缓存迁移。

Stage 0-7 的完整演进记录见[可编辑场景实施计划归档](../archive/plans/scene_authoring/scene_authoring_plan.md)。
