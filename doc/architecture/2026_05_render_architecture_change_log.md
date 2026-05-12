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
