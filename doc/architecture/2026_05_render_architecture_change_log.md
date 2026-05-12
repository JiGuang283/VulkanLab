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

状态：进行中。

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

- 待用户本地运行确认。

### 3.3 当前边界

本阶段当前仍未引入：

- 独立 GUI pass。
- shadow/depth prepass。
- transparent pass。
- frame graph。
- 资源 handle 化。
- pass 之间的自动资源依赖和 barrier 管理。
