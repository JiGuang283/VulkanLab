# 场景数据与 Catalog

> Status: Current
> Last verified: 2026-08-02
> Verified against: Scene Authoring Stage 2 working tree

VulkanLab 已将“导入的模型”和“可编辑场景文档”拆成两个领域对象。模型预览现在由共享 `ModelAsset` 驱动；原生场景文档仍只具备数据与存储基础，尚未转换为运行时 World，也不会出现在 Scenes 列表或 `scene.list` 中。

## 数据分层

```text
CatalogModel
  -> glTF/GLB 或 builtin
  -> 单模型预览 SceneEntry

CatalogSceneDocument
  -> assets/scenes/<id>.vkscene.json
  -> Entity/Transform/Component DTO
  -> Stage 3 才接入 RuntimeWorld
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

## SceneDocument schema v1

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

当前 DTO 定义三类组件，但尚不运行它们：

- `modelInstance`：引用一个 Catalog model ID。
- `light`：Directional、Point 或 Spot 及其颜色、强度、range 和 cone。
- `camera`：透视相机的垂直 FOV、near 和 far。

解析为严格模式：未知顶层字段、未知 Entity 字段或未知 component 都会失败，避免读写后静默丢失未来数据。UUID 必须唯一；parent 必须存在且层级无环；active camera 必须引用 Camera entity；Transform、Light 和 Camera 数值必须有限且在有效范围内；scale 分量不能接近零。Quaternion 在加载时验证并规范化。

不传 Catalog references 时允许模型和环境暂时未解析，便于独立编辑文件。将文档加入 Catalog 时，`SceneCatalogStore` 会使用当前 models/environments 重新加载并校验所有引用。

## 创建与保存

`SceneDocumentService::createDefault()` 创建 Camera、Directional Sun、默认 ambient 和空 environment，不自动加入 ModelInstance。

`saveAtomic()` 只接受项目根目录内且以 `.vkscene.json` 结尾的路径。保存前会重新验证；调用者必须传入加载时的 file stamp 才能覆盖已有文件。磁盘内容被外部修改时返回 `scene_changed_on_disk`，临时文件会清理，旧文件保持不变。`SceneCatalogStore::addSceneDocument()` 只能在场景文件已经成功保存后调用。

## 工作流与 UI 边界

`SceneWorkflowController` 持有 Catalog、模型预览 registry、选择状态和导入/派生资产任务状态，不依赖 ImGui 或 Vulkan。Application 注入加载、相机和窗口相关 action，并继续拥有实际 Vulkan Scene、GPU builder 与 Renderer。

`ScenesPanel` 和 `AssetsPanel` 位于 `src/editor/panels/`。它们只接收 `SceneWorkflowSnapshot`/panel snapshot 与回调 action，只保存搜索文本、modal 草稿和选中项等临时 UI 状态。Runtime Control 与 UI 最终调用同一 Application/workflow 入口；兼容协议仍使用 `scene.*` 表示模型预览。

## 当前限制

- 主 Scenes 窗口标题语义是 `Model Previews`，一次仍只运行一个模型预览。
- Catalog v3 的原生 `scenes[]` 会被解析和验证，但不会生成 `SceneEntry`。
- 当前没有 New/Open/Save Scene UI、Outliner、Inspector、Undo/Redo、Dirty 状态或 RuntimeWorld。
- Validator index、KTX2 manifest、ArtifactIndex 和 Runtime Control 中既有 `sceneId` 字段保持不变；在模型资产路径中它是 `modelId` 的兼容序列化名称，不触发缓存迁移。

后续 RuntimeWorld、编辑和多模型实例阶段见[可编辑场景实施计划](../development/scene_authoring_plan.md)。
