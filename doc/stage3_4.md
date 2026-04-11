# 阶段 3.4：Scene 与 SceneObject

## 一、目标

引入场景管理层，将 `mainLoop()` 中硬编码的"绑定材质 → 设置 viewport → 绑定 mesh → 绘制"流程抽象为 Scene 驱动的渲染循环，支持多物体渲染。

同时将 `updateUniformBuffer()` 中的 model 矩阵硬编码移入 SceneObject 的 transform，view/proj 矩阵保留在 App 层（阶段 3.5 Camera 时迁出）。

---

## 二、当前状态

`mainLoop()` 中的渲染代码：

```cpp
material_->bind(cmd, renderer_->frameIndex());
// viewport / scissor ...
mesh_->bind(cmd);
mesh_->draw(cmd);
```

- 只渲染 1 个物体
- model 矩阵在 `updateUniformBuffer` 中硬编码为绕 Z 轴旋转
- 没有物体列表，新增物体需要手动复制代码

---

## 三、类设计

### 3.1 SceneObject

```cpp
// src/scene/SceneObject.h
#pragma once
#include <glm/glm.hpp>
#include <memory>

namespace vkr {

class Mesh;
class Material;

struct SceneObject {
    std::shared_ptr<Mesh>     mesh;
    std::shared_ptr<Material> material;
    glm::mat4                 transform{1.0f};  // 默认单位矩阵
};

} // namespace vkr
```

SceneObject 是纯数据结构，不拥有资源生命周期（使用 `shared_ptr` 共享已有资源）。

### 3.2 Scene

```cpp
// src/scene/Scene.h
#pragma once
#include "SceneObject.h"
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Scene {
public:
    void addObject(SceneObject obj);

    /// 遍历所有物体，按 material 分组绑定并绘制
    void render(VkCommandBuffer cmd, uint32_t frameIndex) const;

    const std::vector<SceneObject>& objects() const { return objects_; }

private:
    std::vector<SceneObject> objects_;
};

} // namespace vkr
```

### 设计要点

1. **SceneObject 是值类型** — 内部用 `shared_ptr` 引用 Mesh/Material，拷贝开销低。
2. **`render()` 方法** — 遍历物体列表，对每个物体调用 `material->bind()` + `mesh->bind()` + `mesh->draw()`。当前阶段不做材质排序优化（物体少时无必要）。
3. **不涉及 Camera** — `render()` 不处理 view/proj 矩阵。UBO 更新仍由 App 负责（阶段 3.5 才引入 Camera）。
4. **不涉及 per-object UBO** — 当前 shader 使用单个全局 UBO (model + view + proj)。Scene 渲染多物体时，需在每个物体绘制前更新 UBO 中的 model 矩阵。为此需在 Renderer 上暴露 per-frame UBO 写入能力（已有 `mappedUniformBuffer()`），Scene 在绘制每个物体前 memcpy 该物体的 model 矩阵。

### 3.3 UBO 更新策略

当前 UBO 结构：
```cpp
struct UniformBufferObject {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
};
```

多物体渲染时，view/proj 对所有物体相同，只有 model 不同。流程：

```
对每一帧:
  写入 view + proj（一次）
  对每个 SceneObject:
    写入该物体的 model 矩阵
    bind material
    bind mesh
    draw
```

由于当前 UBO 是 persistent mapped，每次 memcpy 后 GPU 读取的就是最新值（前提是上一次 draw 已提交）。在单线程 + 顺序录制命令的场景下，这种逐物体更新 UBO 的方式是安全的：每个 `vkCmdDraw*` 录制到命令缓冲后，随后更新 UBO，然后录制下一个 draw——但这样**不安全**，因为命令缓冲是延迟执行的，所有 draw 共享同一块 UBO 内存。

**解决方案：使用 Push Constants 传递 model 矩阵**。Push Constants 是 Vulkan 中传递少量逐物体数据的标准方式，不需要额外缓冲：

- UBO 仅保留 `view` + `proj`（所有物体共享，每帧写一次）
- model 矩阵通过 `vkCmdPushConstants` 逐物体传递

这需要修改 shader 和 PipelineLayout。

### 3.4 Push Constants 方案

**shader.vert 修改：**
```glsl
// 原来: layout(binding = 0) uniform UBO { mat4 model; mat4 view; mat4 proj; } ubo;
// 改为:
layout(binding = 0) uniform UBO { mat4 view; mat4 proj; } ubo;
layout(push_constant) uniform PushConstants { mat4 model; } push;

void main() {
    gl_Position = ubo.proj * ubo.view * push.model * vec4(inPosition, 1.0);
    // ...
}
```

**PipelineLayout 修改：** Pipeline 创建时需添加 `VkPushConstantRange`。

**UBO 结构修改：**
```cpp
struct UniformBufferObject {
    glm::mat4 view;
    glm::mat4 proj;
};
```

---

## 四、实施步骤

### 步骤 1：修改 shader

修改 `shader/shader.vert`：
- UBO 中移除 `model`，仅保留 `view` + `proj`
- 新增 `layout(push_constant) uniform PushConstants { mat4 model; } push;`
- `gl_Position` 使用 `push.model` 替代 `ubo.model`

重新编译 shader（`compile.bat`）。

### 步骤 2：修改 UniformBufferObject

在 `vulkan_utils.h` 中：
```cpp
struct UniformBufferObject {
    glm::mat4 view;
    glm::mat4 proj;
};
```

移除 `model` 字段。

### 步骤 3：修改 Pipeline 以支持 Push Constants

在 `Pipeline` 构造函数中，创建 `VkPipelineLayout` 时添加 `VkPushConstantRange`：

```cpp
VkPushConstantRange pushConstantRange{};
pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
pushConstantRange.offset = 0;
pushConstantRange.size = sizeof(glm::mat4);  // 64 bytes, model matrix
```

Pipeline 构造函数签名不变，内部自动配置 push constant range。

### 步骤 4：创建 SceneObject.h

- 文件位置：`src/scene/SceneObject.h`
- 纯数据结构，`shared_ptr<Mesh>` + `shared_ptr<Material>` + `glm::mat4 transform`

### 步骤 5：创建 Scene.h / Scene.cpp

- 文件位置：`src/scene/Scene.h`、`src/scene/Scene.cpp`
- `addObject(SceneObject obj)` — 添加物体
- `render(cmd, frameIndex)` — 遍历物体：bind material → push model matrix → bind mesh → draw

```cpp
void Scene::render(VkCommandBuffer cmd, uint32_t frameIndex) const {
    for (const auto& obj : objects_) {
        obj.material->bind(cmd, frameIndex);
        vkCmdPushConstants(cmd, obj.material->pipelineLayout(),
                           VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(glm::mat4), &obj.transform);
        obj.mesh->bind(cmd);
        obj.mesh->draw(cmd);
    }
}
```

### 步骤 6：修改 app.h

- 新增 include：`#include "scene/Scene.h"`
- 将 `texture_`、`material_`、`mesh_` 改为 `shared_ptr`（Scene 需要共享引用）
- 新增成员：`vkr::Scene scene_`
- 移除方法：`updateUniformBuffer` 的 model 部分（model 由 SceneObject transform 提供）

### 步骤 7：修改 app.cpp — initVulkan

在创建 texture / material / mesh 后，构建 scene：

```cpp
auto obj = vkr::SceneObject{mesh_, material_, glm::mat4(1.0f)};
scene_.addObject(obj);
```

### 步骤 8：修改 app.cpp — mainLoop

替换手动渲染代码为：

```cpp
renderer_->beginRenderPass(cmd);

// viewport / scissor 设置
// ...

updateUniformBuffer(renderer_->frameIndex());  // 只写 view + proj
scene_.render(cmd, renderer_->frameIndex());

renderer_->endRenderPass(cmd);
```

### 步骤 9：修改 app.cpp — updateUniformBuffer

移除 model 矩阵部分：

```cpp
void HelloTriangleApplication::updateUniformBuffer(uint32_t currentImage) {
    UniformBufferObject ubo{};
    ubo.view = glm::lookAt(...);
    ubo.proj = glm::perspective(...);
    ubo.proj[1][1] *= -1;
    memcpy(renderer_->mappedUniformBuffer(currentImage), &ubo, sizeof(ubo));
}
```

model 旋转动画改为在 `mainLoop` 中每帧更新 SceneObject 的 transform：

```cpp
static auto startTime = std::chrono::high_resolution_clock::now();
auto currentTime = std::chrono::high_resolution_clock::now();
float time = ...;
scene_.objects()[0].transform = glm::rotate(glm::mat4(1.0f),
    time * glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
```

### 步骤 10：修改 app.cpp — cleanup

`scene_` 是值类型成员，自动析构。`shared_ptr` 资源在 Scene 和 App 都释放引用后自动销毁。cleanup 中移除已有的 `material_.reset()` / `texture_.reset()` / `mesh_.reset()`，改为 RAII 自动处理（或保留显式 reset 以控制顺序）。

### 步骤 11：构建验证

- 重新编译 shader（`compile.bat`）
- 两个 build 目录 `cmake ..`
- Release / Debug 编译通过
- 运行程序，旋转的 viking_room 模型渲染结果与重构前一致

---

## 五、依赖关系

```
Scene 依赖:
  ├── SceneObject (值类型)
  │     ├── shared_ptr<Mesh>
  │     └── shared_ptr<Material>
  └── VkCommandBuffer (传入)

Pipeline 修改:
  └── 新增 PushConstantRange (sizeof(mat4))

Shader 修改:
  └── model 从 UBO 移到 push_constant
```

---

## 六、对外部接口的影响

| 调用方 | 变更 |
|---|---|
| `Pipeline` 构造函数 | 内部新增 push constant range，签名不变 |
| `Material::pipelineLayout()` | 已有，Scene 用于 `vkCmdPushConstants` |
| `shader.vert` | UBO 结构变化 + push constant 新增 |
| `vulkan_utils.h` | `UniformBufferObject` 移除 `model` 字段 |
| `mainLoop()` (app.cpp) | 手动渲染代码替换为 `scene_.render()` |
| `updateUniformBuffer()` (app.cpp) | 移除 model 矩阵写入 |

---

## 七、阶段完成后 App 状态

```cpp
// app.h 成员
GLFWwindow* window;
unique_ptr<VulkanContext> context;
unique_ptr<Device>        device;
unique_ptr<SwapChain>     swapChain_;
unique_ptr<Renderer>      renderer_;
shared_ptr<Texture>       texture_;
shared_ptr<Material>      material_;
shared_ptr<Mesh>          mesh_;
Scene                     scene_;
```

方法仅剩：
```
initWindow, initVulkan, mainLoop, cleanup,
framebufferResizeCallback, updateUniformBuffer (view + proj only)
```

验证方式：保持单物体旋转动画不变。可选地在 `initVulkan` 中添加第二个物体（偏移 transform）验证多物体渲染。
