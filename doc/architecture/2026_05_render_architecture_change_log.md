# VulkanLab 渲染架构改动记录（2026-05）

## 0. 文档定位

本文只记录实际改动、验证结果和遗留事项。

执行顺序、阶段目标和验收标准放在：

```text
doc/architecture/2026_05_render_architecture_execution_plan.md
```

长期架构设计放在：

```text
doc/architecture/2026_05_render_architecture_evolution.md
```

---

## 1. B-lite：Descriptor Set 解耦

状态：已完成。

完成范围：

```text
Task 1: Global descriptor set      已完成
Task 2: Material texture set       已完成
```

### 1.1 实际改动

`Renderer`：

- 新增 global descriptor set layout。
- 新增 per-frame global descriptor sets。
- `GlobalUBO` descriptor 由 `Renderer` 写入。
- 新增 `bindGlobalDescriptors(cmd, pipelineLayout, frameIndex)`，绑定 set 0。

`Material`：

- 构造函数不再接收 `Renderer&`。
- `Material.cpp` 不再 include `Renderer.h`。
- descriptor set layout 只包含 `binding 0 = baseColor texture`。
- `bindDescriptors()` 改为绑定 set 1。

`Application`：

- 创建 pipeline 时将 descriptor set layout 顺序固定为：

```text
set 0 = renderer.globalDescriptorSetLayout()
set 1 = material.descriptorSetLayout()
```

- 绘制前先绑定 global descriptor set，再进入当前 `Scene::render()`。

`SceneFactory / BuiltinScenes / GltfLoader`：

- 场景和 glTF 加载不再为了创建材质传递 `Renderer&`。

`shader`：

```text
shader.vert: GlobalUBO -> layout(set = 0, binding = 0)
shader.frag: texSampler -> layout(set = 1, binding = 0)
```

### 1.2 验证

已执行：

```text
cmake --build build-debug
git diff --check
```

结果：

- 构建通过。
- shader 在构建中重新编译。
- `git diff --check` 无错误，仅有当前 Git 行尾提示。

运行时验证：

- 已由用户本地运行确认结果正常。

### 1.3 当前边界

本次没有引入：

- `RenderQueue`
- `IRenderPass`
- `RenderPipeline`
- `MaterialTemplate`
- `MaterialInstance`
- `ResourceManager`

下一步建议：

```text
进入 Phase C：RenderQueue 替代 Scene::render。
```

---

## 2. Phase C：RenderQueue 替代 Scene::render

状态：已完成。

分支：

```text
feature/render-queue-phase-c
```

### 2.1 实际改动

新增：

```text
src/render/RenderCommand.h
src/render/RenderQueue.h
src/render/RenderQueue.cpp
```

`RenderCommand` 当前为过渡版：

```text
mesh      = const Mesh*
material  = const Material*
world     = glm::mat4
queue     = RenderQueueType
```

`RenderQueue`：

- 支持 `clear()`。
- 支持 `add(RenderCommand)`。
- 支持 opaque 队列。
- 支持按 `material`、`mesh` 指针稳定排序。
- 提供 draw/material/mesh 计数入口。

`Scene`：

- 移除 `Scene::render()`。
- 新增 `Scene::collectRenderCommands(RenderQueue&)`。
- `Scene.cpp` 不再调用 `vkCmd*`。

`Renderer`：

- 新增 `drawQueue(cmd, frameIndex, pipeline, queue)`。
- 原 `Scene::render()` 中的 pipeline bind、material descriptor bind、push constants、mesh bind/draw 迁入 `Renderer`。

`Application`：

- 新增成员 `RenderQueue renderQueue_`。
- 每帧渲染前执行：

```text
renderQueue.clear()
currentScene.collectRenderCommands(renderQueue)
renderQueue.sortOpaque()
renderer.drawQueue(...)
```

`CMakeLists.txt`：

- `file(GLOB_RECURSE SOURCES ...)` 增加 `CONFIGURE_DEPENDS`，避免新增 `.cpp` 后未进入目标。

### 2.2 验证

已执行：

```text
cmake --build build-debug
```

结果：

- 构建通过。
- `RenderQueue.cpp` 已被纳入目标。

运行时验证：

- 已由用户本地运行确认结果正常。

### 2.3 当前边界

本阶段当前仍未引入：

- handle 化资源引用。
- culling。
- transparent 专用队列。
- Pass 抽象。
- RenderPipeline。

下一步建议：

```text
进入 Phase D：Pass 抽象和 RenderPipeline。
```

---

## 3. Phase D：Pass 抽象和 RenderPipeline

状态：已完成。

分支：

```text
feature/pass-pipeline-phase-d
```

### 3.1 实际改动

新增：

```text
src/render/RenderFrame.h
src/render/RenderPipeline.h
src/render/RenderPipeline.cpp
src/render/pass/IRenderPass.h
src/render/pass/MainForwardPass.h
src/render/pass/MainForwardPass.cpp
```

`RenderFrameContext`：

- 新增每帧渲染执行上下文，用于向 pass 传递 command buffer、frame index、image index、viewport extent，以及当前过渡期仍需要的 opaque pipeline 和 GUI 系统指针。

`IRenderPass`：

- 新增 pass 抽象接口。
- 当前接口包含 `name()`、`onResize()` 和 `execute()`。

`RenderPipeline`：

- 新增 pass 容器。
- 支持按顺序添加 pass。
- 支持交换链重建后广播 `onResize()`。
- 支持每帧顺序执行 pass。

`MainForwardPass`：

- 从 `Renderer` 接管主 `VkRenderPass`。
- 从 `Renderer` 接管 framebuffer、MSAA color attachment 和 depth attachment。
- 从 `Renderer` 接管 opaque draw queue 编码。
- 当前过渡版仍在同一个 pass 内执行 GUI 绘制。

`Renderer`：

- 不再直接持有主 framebuffer、color/depth attachment 和主 `VkRenderPass`。
- 新增 `renderFrame()`，负责绑定 global descriptor set 并调度 `RenderPipeline`。
- 保留 per-frame uniform buffer 和 global descriptor set 责任。
- `renderPass()` 改为从 `MainForwardPass` 读取，用于现阶段创建 graphics pipeline 和 GUI。

`Application`：

- 主循环不再直接调用 `beginRenderPass()`、`drawQueue()`、`gui.render()` 和 `endRenderPass()`。
- 主循环改为调用 `renderer.renderFrame(...)`。

### 3.2 验证

已执行：

```text
cmake --build build-debug
git diff --check -- . ':(exclude).vscode/settings.json'
```

结果：

- 构建通过。
- Phase D 相关文件无 whitespace 错误。
- `Application` 不再直接调用旧的 render pass begin/end 和 draw helper。

运行时验证：

- 已由用户本地运行确认结果正常。

### 3.3 当前边界

本阶段当前仍未引入：

- 独立 GUI pass。
- shadow/depth prepass。
- transparent pass。
- frame graph。
- 资源 handle 化。
- pass 之间的自动资源依赖和 barrier 管理。

---

## 4. Phase B-complete：材质系统完整化

状态：代码完成，待运行时验证。

分支：

```text
feature/material-template-phase-b-complete
```

### 4.1 实际改动

新增：

```text
doc/architecture/2026_05_b_complete_material_system_plan.md
src/render/FallbackTextures.h
src/render/FallbackTextures.cpp
src/render/MaterialTextureSlot.h
src/render/MaterialInstance.h
src/render/MaterialInstance.cpp
src/render/MaterialTemplate.h
src/render/MaterialTemplate.cpp
src/render/PipelineKey.h
src/render/PipelineCache.h
src/render/PipelineCache.cpp
```

删除：

```text
src/render/Material.h
src/render/Material.cpp
```

`B-complete 方案文档`：

- 新增 B-complete 专项执行方案。
- 明确材质实例、texture slots、typed pipeline key、pipeline 选择下沉到 pass 的完成路径。

`MaterialTemplate`：

- 新增材质模板对象。
- 接管 material descriptor set layout。
- 保存用于创建 graphics pipeline 的 `PipelineConfig`。
- 将 material descriptor set layout 追加为 pipeline layout 的 material set。
- material descriptor layout 从单一 `baseColor` 扩展为固定 PBR texture slots。

`MaterialTextureSlot / FallbackTextures`：

- 新增固定材质贴图槽位：
  - baseColor
  - normal
  - metallicRoughness
  - occlusion
  - emissive
- 新增 white、black、flatNormal fallback textures。
- 缺失 glTF texture slot 会写入 fallback texture，避免 descriptor 未写入。

`MaterialInstance`：

- 替代旧 `Material`。
- 持有 `std::shared_ptr<MaterialTemplate>`。
- 持有 `MaterialParams`。
- 持有 5 个材质 texture slot。
- 负责 per-frame material descriptor set 分配和写入。
- 不再保存 `PipelineConfig`。
- 不依赖 `Renderer`。

`Scene`：

- 新增 material template 持有列表。
- `SceneObject` 和 `RenderCommand` 改为引用 `MaterialInstance`。
- 移除过渡接口 `primaryMaterialTemplate()`。

`BuiltinScenes / GltfLoader`：

- 场景工厂先创建共享 `MaterialTemplate`。
- 同一场景内多个 `MaterialInstance` 共享同一个 `MaterialTemplate`。
- glTF 多个 material 现在共享同一套 shader/layout/state 模板。
- glTF loader 解析 baseColor、normal、metallicRoughness、occlusion、emissive texture slot。

`PipelineKey / PipelineCache`：

- 新增强类型 `PipelineKey`。
- key 当前包含 material template、pass id、render pass、subpass、MSAA samples。
- `PipelineCache` 不再使用字符串拼接 key。
- 交换链重建后清空 cache，后续 draw 以新 render pass 延迟重建 pipeline。

`DescriptorAllocator`：

- descriptor pool page 容量扩大，适配每个材质实例 5 个 sampler binding。
- `allocate()` 支持传入本次 descriptor set 的 descriptor 需求。
- allocator 在调用 `vkAllocateDescriptorSets()` 前检查当前 pool 剩余容量，不再依赖先触发 `VK_ERROR_OUT_OF_POOL_MEMORY` 后换 pool。
- 修复大型 glTF 场景切换时 validation layer 报告 sampler descriptor pool 剩余容量不足的问题。

`MainForwardPass / Renderer`：

- pipeline 选择从 `Application` 下沉到 `MainForwardPass`。
- `MainForwardPass` 按 `RenderCommand.material->materialTemplate()` 从 `PipelineCache` 获取 pipeline。
- pipeline 切换时在当前 pipeline layout 上绑定 set 0 global descriptor set。
- `RenderFrameContext` 不再持有 `opaquePipeline`，改为持有 global descriptor set、global descriptor set layout 和 `PipelineCache*`。
- `Renderer::renderFrame()` 不再接收 `Pipeline&`。

`Application`：

- 不再创建、持有或传递 opaque pipeline。
- 只持有 `PipelineCache`，并在场景切换和 swapchain 重建时清空 cache。

### 4.2 验证

已执行：

```text
cmake --build build-debug
cmake --build build --config Release
git diff --check
```

结果：

- 构建通过。
- Release 构建通过。
- 当前改动无 whitespace 错误。
- 新增 `.cpp` 已被 CMake `CONFIGURE_DEPENDS` 自动纳入目标。
- 静态检查未发现旧的“第一个对象材质创建 pipeline”路径。
- 静态检查未发现 `Application` 持有或传递 opaque pipeline。
- 仍存在既有链接警告：`LNK4098: 默认库“MSVCRT”与其他库的使用冲突`。

运行时验证：

- 待用户本地运行确认。

### 4.3 当前边界

本阶段仍未引入：

- 全量 ResourceManager / Handle 化。
- Material parameter buffer / SSBO。
- shader variant/permutation 系统。
- normal/metallicRoughness/occlusion/emissive 的 shader 使用。
- glTF 非 baseColor texture 的精确色彩空间处理；当前 descriptor slot 已接入，但 shader 尚未采样这些 slot。
