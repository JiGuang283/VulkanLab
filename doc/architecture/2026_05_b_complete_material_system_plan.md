# VulkanLab Phase B-complete 具体执行方案

## 0. 文档定位

本文是 `2026_05_render_architecture_execution_plan.md` 中 Phase B-complete 的细化执行文档。

目标不是重新设计长期架构，而是把 B-complete 拆成可以逐步提交、逐步运行验证的小切片。后续实际编码以本文为准，实际改动仍记录到：

```text
doc/architecture/2026_05_render_architecture_change_log.md
```

当前代码已经完成 B-complete 第一刀：

```text
feature/material-template-phase-b-complete
3d5b327 refactor: introduce material templates and pipeline cache
```

已完成内容：

- `MaterialTemplate` 已新增。
- `Material` 已改为共享 `MaterialTemplate`。
- `PipelineCache` 已新增。
- `Application` 不再通过“第一个对象材质”创建 pipeline。

本文后续方案基于这个状态继续推进。

---

## 1. B-complete 完成后的目标状态

B-complete 完成后，渲染器应满足：

- `MaterialTemplate` 表达 shader、descriptor layout、pipeline layout 所需 set、渲染状态。
- `MaterialInstance` 表达某个模型实例实际使用的贴图和材质参数。
- `PipelineCache` 通过明确的 `PipelineKey` 创建和缓存 pipeline。
- `MainForwardPass` 根据每条 `RenderCommand` 的材质模板选择 pipeline。
- `Application` 不再持有或选择 opaque pipeline。
- glTF 多个 material 可以共享模板，但仍保留各自贴图和 PBR 参数。
- 缺失的 glTF 贴图通过统一 fallback texture 补齐。

B-complete 不强行完成：

- 全量 ResourceManager / Handle 化。
- 多线程 command buffer 录制。
- Deferred rendering。
- Shader variant/permutation 系统。
- 完整 PBR 光照。
- SceneGraph。

原因：这些内容会同时牵动 Scene、Pass、资源生命周期和 shader 系统，应该在材质边界稳定后进入后续阶段。

---

## 2. 目标数据模型

### 2.1 MaterialTextureSlot

新增固定 texture slot：

```cpp
enum class MaterialTextureSlot : uint32_t {
    BaseColor = 0,
    Normal = 1,
    MetallicRoughness = 2,
    Occlusion = 3,
    Emissive = 4,
    Count,
};
```

第一版 descriptor binding 固定为：

```text
set 1 binding 0 = baseColor
set 1 binding 1 = normal
set 1 binding 2 = metallicRoughness
set 1 binding 3 = occlusion
set 1 binding 4 = emissive
```

即使 shader 当前只使用 `baseColor`，也可以先把 descriptor layout 和 descriptor 写入结构稳定下来。

### 2.2 MaterialParams

`MaterialParams` 继续保存 CPU 侧参数：

```cpp
struct MaterialParams {
    glm::vec4 baseColorFactor{1.0f};
    glm::vec3 emissiveFactor{0.0f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
};
```

短期仍通过 push constants 写入 GPU。不要在本阶段同时引入 per-object SSBO 或 material parameter buffer。

### 2.3 MaterialTemplate

`MaterialTemplate` 最终应持有：

```cpp
struct MaterialTemplateDesc {
    std::string name;
    std::string vertShaderPath;
    std::string fragShaderPath;
    VertexLayout vertexLayout;
    RenderState renderState;
};
```

其中 `RenderState` 从现有 `PipelineConfig` 中拆出：

```cpp
struct RenderState {
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool depthTest = true;
    bool depthWrite = true;
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS;
    bool blendEnable = false;
    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;
};
```

过渡期可以继续让 `MaterialTemplate` 内部保存 `PipelineConfig`，但对外语义要变成：

```text
MaterialTemplate 提供 pipeline 所需描述
MaterialInstance 不提供 pipeline state
```

### 2.4 MaterialInstance

新增 `MaterialInstance`，从当前 `Material` 中拆出实例职责：

```cpp
class MaterialInstance {
public:
    const MaterialTemplate& materialTemplate() const;
    const MaterialParams& params() const;

    VkDescriptorSet descriptorSet(uint32_t frameIndex) const;
    void bindDescriptors(VkCommandBuffer cmd,
                         VkPipelineLayout layout,
                         uint32_t frameIndex) const;
};
```

内部持有：

```cpp
std::shared_ptr<MaterialTemplate> materialTemplate_;
MaterialParams params_;
std::array<std::shared_ptr<Texture>, kMaterialTextureSlotCount> textures_;
std::vector<VkDescriptorSet> descriptorSets_;
```

完成后，`Material` 这个名字可以：

- 方案 A：直接重命名为 `MaterialInstance`，同步修改调用点。
- 方案 B：保留 `using Material = MaterialInstance;` 作为短期兼容层。

建议采用方案 A，避免新旧概念长期并存。

### 2.5 PipelineKey

当前 `PipelineCache` 使用字符串 key。B-complete 完成前应替换为强类型 key：

```cpp
enum class PassId : uint32_t {
    MainForward = 0,
};

struct PipelineKey {
    const MaterialTemplate* materialTemplate = nullptr;
    PassId pass = PassId::MainForward;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    uint32_t subpass = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};
```

第一版可以继续用 `VkRenderPass` 表示 attachment 兼容性。等后续切到 dynamic rendering 或 RenderGraph，再改成 color/depth format key。

---

## 3. 分阶段实施

### Step B1：第一刀运行确认和合并

当前已完成代码提交，但尚未合并到 `master`。

需要做：

1. 本地运行确认：

```text
Viking Room
Sheen Chair
可选 glTF 场景
GUI
场景切换
窗口 resize
```

2. 运行正常后：

```text
change log 标记 B-complete 第一刀已验证
提交验证记录
fast-forward 合并回 master
```

验收标准：

- 当前画面不变。
- 场景切换不崩溃。
- resize 后 pipeline cache 可以重建 pipeline。

### Step B2：引入 MaterialTextureSlot 和 fallback textures

目标：

把 baseColor 单贴图 descriptor 扩展为固定 PBR texture slots，但不急着改 shader 效果。

建议新增：

```text
src/render/MaterialTextureSlot.h
src/render/FallbackTextures.h
src/render/FallbackTextures.cpp
```

`FallbackTextures` 第一版提供：

```text
white       baseColor / metallicRoughness / occlusion
black       emissive
flatNormal  normal
```

改动范围：

```text
MaterialTemplate
Material
GltfLoader
BuiltinScenes
```

具体工作：

1. `MaterialTemplate::createDescriptorSetLayout()` 创建 5 个 combined image sampler binding。
2. `Material` 或后续 `MaterialInstance` descriptor 写入 5 个 texture slot。
3. `GltfLoader` 解析：

```text
baseColorTexture
normalTexture
metallicRoughnessTexture
occlusionTexture
emissiveTexture
```

4. 缺失 slot 使用 fallback texture。
5. shader 暂时只使用 binding 0。

验收标准：

- 所有 material descriptor set 都写满 5 个 binding。
- 缺失贴图不会导致 descriptor 未写入。
- 当前画面保持不变。

### Step B3：拆出 MaterialInstance

目标：

让类型名和职责一致，避免当前 `Material` 既像实例又像旧材质系统。

建议新增：

```text
src/render/MaterialInstance.h
src/render/MaterialInstance.cpp
```

迁移方向：

```text
Material 当前职责 -> MaterialInstance
SceneObject::material -> std::shared_ptr<MaterialInstance>
RenderCommand::material -> const MaterialInstance*
GltfAsset::materials -> vector<shared_ptr<MaterialInstance>>
```

旧 `Material.h/.cpp` 处理方式：

第一提交可以保留兼容头：

```cpp
#pragma once
#include "MaterialInstance.h"
namespace vkr {
using Material = MaterialInstance;
}
```

第二提交再删除旧引用和旧文件，避免一次性改动过大。

验收标准：

- `MaterialInstance` 不依赖 `Renderer`。
- `MaterialInstance` 不保存 `PipelineConfig`。
- `MaterialInstance` 只保存模板、贴图、参数、descriptor set。
- `SceneObject` 和 `RenderCommand` 使用 `MaterialInstance` 语义。

### Step B4：PipelineCache 强类型 key

目标：

把当前字符串 key 替换为可维护的 `PipelineKey`。

建议新增：

```text
src/render/PipelineKey.h
```

`PipelineCache` 接口调整为：

```cpp
Pipeline& getOrCreate(const PipelineKey& key,
                      const PipelineConfig& config);
```

其中：

- `PipelineKey` 决定缓存命中。
- `PipelineConfig` 决定实际创建。

短期 key 包含：

```text
materialTemplate pointer
pass id
renderPass
subpass
samples
```

不要把整份 `PipelineConfig` 全部串成字符串。必要时为 `PipelineKey` 写 `operator==` 和 hash。

验收标准：

- `PipelineCache` 不再依赖字符串拼接。
- resize 后清空 cache 并以新 render pass 创建 pipeline。
- 当前只支持 `PassId::MainForward`。

### Step B5：pipeline 选择下沉到 MainForwardPass

目标：

完成执行计划中的关键要求：`ForwardOpaquePass` 根据 command 的 material/template 获取 pipeline。

当前状态：

```text
Application -> opaquePipeline_ -> Renderer::renderFrame -> MainForwardPass
```

目标状态：

```text
Application -> Renderer::renderFrame(queue, gui)
Renderer -> RenderFrameContext
MainForwardPass -> command.material.template -> PipelineCache
```

建议修改：

`RenderFrameContext` 增加：

```cpp
PipelineCache* pipelineCache = nullptr;
VkDescriptorSet globalDescriptorSet = VK_NULL_HANDLE;
VkDescriptorSetLayout globalDescriptorSetLayout = VK_NULL_HANDLE;
```

`Renderer` 增加内部访问：

```cpp
VkDescriptorSet globalDescriptorSet(uint32_t frameIndex) const;
```

`MainForwardPass::drawQueue()` 改为：

```text
for command in queue:
  template = command.material->materialTemplate()
  config = composePipelineConfig(template, frame.globalDescriptorSetLayout)
  pipeline = frame.pipelineCache->getOrCreate(key, config)
  if pipeline changed:
      vkCmdBindPipeline(...)
      vkCmdBindDescriptorSets(set 0 global)
  command.material->bindDescriptors(set 1)
  push material params and model
  draw mesh
```

`Application` 删除：

```text
Pipeline* opaquePipeline_
updateOpaquePipeline()
```

`Renderer::renderFrame()` 不再接收 `Pipeline&`。

验收标准：

- `Application` 不再 include 或 forward declare `Pipeline`。
- `RenderFrameContext` 不再有 `opaquePipeline`。
- `MainForwardPass` 可以为不同 material template 切换 pipeline。
- 当前场景仍然只用一个模板也能正常运行。

### Step B6：RenderQueue 排序升级

目标：

让 opaque queue 按 pipeline/template/material/mesh 排序，为后续多模板材质降低状态切换。

当前排序：

```text
material pointer
mesh pointer
```

建议改为：

```text
materialTemplate pointer
materialInstance pointer
mesh pointer
```

如果暂时没有强 sort key，可以先在 `RenderQueue::sortOpaque()` 中直接比较指针。

验收标准：

- opaque draw 顺序稳定。
- 相同模板的 draw 尽量连续。
- 不改变透明物体行为；透明队列后续单独做。

### Step B7：清理过渡接口

目标：

移除第一刀留下的临时入口。

清理列表：

```text
Scene::primaryMaterialTemplate()
Application::updateOpaquePipeline()
RenderFrameContext::opaquePipeline
Material 兼容 alias
PipelineCache::setRenderPass() 如果改成 Renderer/Pass 持有 cache，可以再评估是否保留
```

保留条件：

- 如果某个接口只为当前过渡状态存在，并且没有后续合理职责，就删除。
- 如果接口表达稳定边界，例如 `PipelineCache::clear()`，可以保留。

验收标准：

- pipeline 创建路径只存在于 render/pass 层。
- 场景层不再提供“主材质模板”这种渲染器专用概念。
- 文档中的当前边界和代码一致。

---

## 4. 文件级改动清单

### 4.1 新增文件

预计新增：

```text
src/render/MaterialTextureSlot.h
src/render/FallbackTextures.h
src/render/FallbackTextures.cpp
src/render/MaterialInstance.h
src/render/MaterialInstance.cpp
src/render/PipelineKey.h
```

已有新增：

```text
src/render/MaterialTemplate.h
src/render/MaterialTemplate.cpp
src/render/PipelineCache.h
src/render/PipelineCache.cpp
```

### 4.2 重点修改文件

```text
src/render/MaterialTemplate.*
src/render/PipelineCache.*
src/render/RenderFrame.h
src/render/Renderer.h/.cpp
src/render/pass/MainForwardPass.h/.cpp
src/render/RenderCommand.h
src/render/RenderQueue.cpp
src/render/GltfLoader.h/.cpp
src/render/GltfAsset.h
src/scene/SceneObject.h
src/scene/Scene.h/.cpp
src/scene/BuiltinScenes.cpp
src/app/Application.h/.cpp
```

---

## 5. 提交建议

建议拆成 5 个可运行提交：

```text
1. refactor: add material texture slots
2. refactor: introduce material instances
3. refactor: use typed pipeline keys
4. refactor: select pipelines in forward pass
5. refactor: clean material pipeline transition APIs
```

每个提交都应满足：

- `cmake --build build-debug` 通过。
- 当前 demo 可以运行。
- change log 记录实际改动和遗留边界。

---

## 6. 验证矩阵

每个 step 后至少检查：

```text
cmake --build build-debug
git diff --check
Viking Room 运行
Sheen Chair 运行
场景切换
窗口 resize
GUI 显示
validation layer 无新增明显错误
```

Step B2 后额外检查：

```text
缺 normal texture 的模型仍能显示
缺 metallicRoughness texture 的模型仍能显示
缺 emissive texture 的模型仍能显示
```

Step B5 后额外检查：

```text
MainForwardPass 内可以从 RenderCommand 取得 material template
Application 不再持有 Pipeline
RenderFrameContext 不再持有 opaquePipeline
```

---

## 7. 风险和处理

| 风险 | 处理 |
|---|---|
| descriptor layout binding 增加后 shader 不匹配 | Vulkan 允许 layout 包含 shader 未使用 binding；先只保证 shader 使用的 binding 0 正确 |
| glTF texture slot 色彩空间不一致 | baseColor/emissive 用 sRGB；normal/metallicRoughness/occlusion 后续应改 UNORM，第一版如沿用现有 Texture 需在 change log 标明 |
| pipeline cache key 不完整导致复用错误 pipeline | Step B4 使用强类型 key，先包含 render pass、pass id、template、samples |
| resize 后旧 pipeline 持有旧 render pass | resize 时先 `vkDeviceWaitIdle`，再清空 cache，最后以新 render pass 重建 |
| pipeline 选择下沉后 global set 绑定时机错误 | 在 pipeline 切换时按当前 pipeline layout 重新绑定 set 0 |
| 材质重命名影响面过大 | 先引入 `MaterialInstance`，必要时短期保留 `using Material = MaterialInstance` |

---

## 8. B-complete 最终验收标准

完成 B-complete 时必须满足：

- `MaterialTemplate` 只表达模板级信息：shader、layout、render state。
- `MaterialInstance` 只表达实例级信息：textures、PBR factors、descriptor sets。
- `MaterialInstance` 不依赖 `Renderer`。
- `MaterialInstance` 不保存 `PipelineConfig`。
- `PipelineCache` 使用强类型 `PipelineKey`。
- `MainForwardPass` 根据 `RenderCommand` 的材质模板选择 pipeline。
- `Application` 不再创建、持有或传递 opaque pipeline。
- glTF material slots 有统一 fallback。
- 当前 demo 场景、glTF 场景、GUI、resize 全部正常。

满足以上标准后，再进入 Phase E：SceneGraph 和 glTF 层级完善。
