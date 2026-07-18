# VulkanLab 渲染架构后续执行计划（2026-05）

## 0. 文档定位

本文是 `2026_05_render_architecture_evolution.md` 的执行版。

原文负责说明目标架构和长期方向；本文负责规定接下来实际怎么改、按什么顺序改、每一步改到什么程度就停。后续实现以本文为准，原架构文档作为背景和设计依据。

当前建议不要机械执行完整 `Phase B -> Phase C -> Phase D`。更稳的顺序是：

```text
B-lite -> C -> D -> B-complete -> E -> F
```

其中：

- `B-lite`：只做 descriptor set 解耦，去掉 `Material -> Renderer` 依赖。
- `C`：引入 `RenderCommand / RenderQueue`，让 `Scene` 不再直接发 Vulkan draw。
- `D`：引入 `IRenderPass / RenderPipeline`，把主 pass 和 GUI pass 从 `Renderer` 中拆出。
- `B-complete`：在边界稳定后，再补完整 `MaterialTemplate / MaterialInstance / PipelineCache / ResourceManager`。
- `E/F`：继续推进 SceneGraph、Light、Shadow、PostProcess。

这样做的目的不是改变最终架构，而是降低中间状态风险：每一步都保持 demo 可运行，避免一次性同时修改 `Application`、`Renderer`、`Scene`、`Material`、`GltfLoader`。

---

## 1. 当前基线

截至本文编写时，Phase A 的一部分已经落地：

```text
src/core/Log.*
src/core/VulkanCheck.h
src/core/VulkanException.*
src/core/ResourceHandle.h
src/core/ResourcePool.h
src/core/ResourcePoolSelfTest.*
src/core/DescriptorAllocator.*
src/core/PipelineConfigBuilder.*
```

这些内容不再作为后续重点。后续主要处理以下仍然存在的耦合：

| 位置 | 当前状态 | 需要解决的问题 |
|---|---|---|
| `Application` | 直接调用 `Renderer::beginRenderPass()`、`Scene::render()`、`GuiSystem::render()`、`Renderer::endRenderPass()` | 主循环知道太多渲染细节 |
| `Renderer` | 拥有主 `VkRenderPass`、framebuffer、MSAA/depth、global UBO，并暴露 begin/end render pass | 难以扩展到多 pass |
| `Scene` | `Scene::render()` 内部调用 `vkCmdBindPipeline`、descriptor bind、push constants、mesh draw | Scene 和 Vulkan draw call 强耦合 |
| `Material` | 构造需要 `Renderer&`，descriptor set 同时绑定 global UBO 和 texture | 材质生命周期和全局帧数据绑死 |
| `Pipeline` | 当前由首个材质提供的 `PipelineConfig` 创建 | 多材质模板、多 pipeline、多 pass 不好表达 |
| `GltfLoader` | 能遍历 node，但最终压平成 `SceneObject` | 后续动画、层级变换、剔除缺基础 |

---

## 2. 总体执行原则

### 2.1 每一步必须保持画面可运行

每个阶段结束都应满足：

- 项目能编译。
- 当前已有 demo 场景能打开。
- Vulkan validation 不新增明显错误。
- 画面结果尽量不变，除非该阶段明确要改变渲染效果。

如果某一步需要大范围修改，先拆成更小的过渡接口，不追求一次到位。

### 2.2 先切断硬耦合，再补完整抽象

优先处理这三条硬耦合：

```text
Material -> Renderer
Scene -> vkCmd*
Application -> begin/end VkRenderPass
```

完整的 `MaterialTemplate`、资源 handle 化、真正 RenderGraph 都可以晚一些。早期先建立边界。

### 2.3 允许过渡结构

为了降低风险，允许短期使用过渡结构。例如 Phase C 初期的 `RenderCommand` 可以先存裸指针：

```cpp
struct RenderCommand {
    const Mesh* mesh = nullptr;
    const Material* material = nullptr;
    glm::mat4 world{1.0f};
};
```

因为 `RenderQueue` 只在本帧有效，且 `Scene` 仍持有 `shared_ptr`，这个过渡是可控的。等 `ResourceManager` 和 handle pool 稳定后再换成 handle。

### 2.4 不提前做重型系统

在 B-lite/C/D 完成前，不做：

- 完整 ECS。
- 工业级 RenderGraph。
- 多线程 command buffer 录制。
- shader variant/permutation 系统。
- deferred rendering。
- clustered/Forward+。
- 骨骼动画。

这些功能都依赖更清楚的 Scene、Queue、Pass 边界。

---

## 3. 推荐阶段顺序

```text
Phase B-lite: Descriptor set 解耦
    |
    v
Phase C: RenderQueue 替代 Scene::render
    |
    v
Phase D: Pass 抽象和 RenderPipeline
    |
    v
Phase B-complete: MaterialTemplate / MaterialInstance / PipelineCache
    |
    v
Phase E: SceneGraph 和 glTF 层级
    |
    v
Phase F: Light / Shadow / PostProcess
```

前三个阶段是近期主线。后续所有功能都应以这三个阶段的结果为基础。

---

## 4. Phase B-lite：Descriptor Set 解耦

### 4.1 目标

只解决一个问题：`Material` 不再依赖 `Renderer`，global UBO 不再写进每个材质 descriptor set。

本阶段不要求完成：

- `MaterialTemplate`。
- `MaterialInstance`。
- `PipelineCache`。
- `ResourceManager`。
- texture/mesh handle 化。

### 4.2 目标 descriptor set 语义

先固定两层 descriptor set：

```text
set 0: Frame / View
  binding 0 = GlobalUBO

set 1: Material
  binding 0 = baseColor texture
```

后续扩展时再变成：

```text
set 0: Frame / View / Lights / Shadow maps
set 1: Material textures and parameters
set 2: Object or instance data
```

### 4.3 设计边界

`Renderer` 或一个临时 `FrameDescriptorSet` 辅助对象负责：

- 创建 global descriptor set layout。
- 为每个 frame 分配 global descriptor set。
- 把 `GlobalUBO` buffer 写入 `set 0 binding 0`。
- 在绘制前绑定 global descriptor set。

`Material` 只负责：

- 创建 material descriptor set layout。
- 分配 material descriptor set。
- 写入 texture sampler。
- 绑定 `set 1`。

`Material` 构造函数不再接收 `Renderer&`。

### 4.4 建议文件改动

优先小步修改现有文件，不急着新增大量类：

```text
src/render/Renderer.h/.cpp
  - 增加 globalDescriptorSetLayout()
  - 增加 bindGlobalDescriptors(cmd, pipelineLayout, frameIndex)
  - global UBO descriptor 写入挪到 Renderer 内部

src/render/Material.h/.cpp
  - 构造函数移除 Renderer&
  - descriptor layout 只保留 texture binding
  - bindDescriptors() 改为绑定 set 1

src/scene/BuiltinScenes.cpp
src/render/GltfLoader.h/.cpp
src/scene/SceneFactory.h
  - 传参适配，创建 Material 时不再传 Renderer

src/app/Application.cpp
  - 创建 pipeline 前，把 global layout 和 material layout 都放进 PipelineConfig
  - draw 前绑定 global descriptor set

shader/*
  - GlobalUBO 使用 layout(set = 0, binding = 0)
  - baseColor texture 使用 layout(set = 1, binding = 0)
```

如果现有 shader 编译流程依赖外部命令，修改 shader 后必须重新编译 SPIR-V。

### 4.5 推荐实施步骤

1. 在 `Renderer` 中创建 global descriptor set layout：

```cpp
VkDescriptorSetLayout Renderer::globalDescriptorSetLayout() const;
```

2. 在 `Renderer::createUniformBuffers()` 后，分配并写入 per-frame global descriptor sets。

3. 增加绑定接口：

```cpp
void Renderer::bindGlobalDescriptors(VkCommandBuffer cmd,
                                     VkPipelineLayout layout,
                                     uint32_t frameIndex) const;
```

内部绑定：

```cpp
vkCmdBindDescriptorSets(
    cmd,
    VK_PIPELINE_BIND_POINT_GRAPHICS,
    layout,
    0,
    1,
    &globalDescriptorSets_[frameIndex],
    0,
    nullptr);
```

4. 修改 `Material::createDescriptorSetLayout()`，删除 UBO binding，只保留 texture binding。

5. 修改 `Material::createDescriptorSets()`，删除 `renderer_->uniformBufferHandle()` 相关写入，只写 texture。

6. 修改 `Material::bindDescriptors()`，从 `firstSet = 1` 开始绑定。

7. 修改 pipeline layout 创建顺序：

```text
descriptorLayouts[0] = renderer.globalDescriptorSetLayout()
descriptorLayouts[1] = material.descriptorSetLayout()
```

8. 修改 shader set/binding，并重新编译 shader。

9. 运行 demo，确认画面不变。

### 4.6 验收标准

本阶段完成后必须满足：

- `Material` 构造函数不再接收 `Renderer&`。
- `Material.cpp` 不再 include `Renderer.h`。
- `Material.cpp` 不再调用 `renderer_->uniformBufferHandle()` 或 `renderer_->uniformBufferSize()`。
- global UBO descriptor 只由 `Renderer` 或 frame descriptor 管理。
- material descriptor set 只表示材质资源。
- pipeline layout 至少包含两个 set layout：global + material。
- 当前所有场景画面保持可用。

### 4.7 常见风险

| 风险 | 处理 |
|---|---|
| shader set/binding 忘记同步 | 修改 C++ descriptor layout 后立即同步 shader |
| pipeline layout descriptor 顺序错误 | 固定顺序：global 在 set 0，material 在 set 1 |
| `Material::bindDescriptors` 仍从 set 0 绑定 | 必须改成 firstSet = 1 |
| resize 时 descriptor 失效 | global UBO 如果不随 swapchain 重建，descriptor 不应被无故重建 |

---

## 5. Phase C：RenderQueue 替代 Scene::render

### 5.1 目标

让 `Scene` 不再直接调用 Vulkan draw 命令。

本阶段完成后，`Scene` 的职责变成：

```text
保存对象 -> 更新对象 -> 收集本帧 RenderCommand
```

Vulkan 命令编码移动到 render 层。

### 5.2 第一版 RenderCommand

第一版允许使用指针，避免提前引入资源 handle：

```cpp
enum class RenderQueueType {
    Opaque,
    Transparent,
};

struct RenderCommand {
    const Mesh* mesh = nullptr;
    const Material* material = nullptr;
    glm::mat4 world{1.0f};
    RenderQueueType queue = RenderQueueType::Opaque;
};
```

后续再扩展：

```cpp
Bounds worldBounds;
uint64_t sortKey;
bool castsShadow;
```

### 5.3 第一版 RenderQueue

建议新增：

```text
src/render/RenderCommand.h
src/render/RenderQueue.h
src/render/RenderQueue.cpp
```

接口先保持简单：

```cpp
class RenderQueue {
public:
    void clear();
    void add(RenderCommand command);
    void sortOpaque();

    std::span<const RenderCommand> opaque() const;

private:
    std::vector<RenderCommand> opaque_;
};
```

初期只做 opaque 队列。透明队列可以等 alpha blending 稳定后补。

### 5.4 Scene 改造

`Scene::render()` 替换为：

```cpp
void Scene::collectRenderCommands(RenderQueue& queue) const;
```

逻辑很直接：

```cpp
for (const auto& obj : objects_) {
    queue.add(RenderCommand{
        .mesh = obj.mesh.get(),
        .material = obj.material.get(),
        .world = obj.transform,
        .queue = RenderQueueType::Opaque,
    });
}
```

此时 `Scene` 仍然可以持有：

```cpp
std::vector<std::shared_ptr<Mesh>> meshes_;
std::vector<std::shared_ptr<Material>> materials_;
std::vector<SceneObject> objects_;
```

不要在这个阶段强行换成 handle。

### 5.5 Vulkan 命令编码位置

短期可以先放在 `Renderer` 中：

```cpp
void Renderer::drawQueue(VkCommandBuffer cmd,
                         uint32_t frameIndex,
                         Pipeline& pipeline,
                         const RenderQueue& queue);
```

`Scene.cpp` 中原来的 push constant block 可以移动到 render 层，例如：

```text
src/render/ForwardDrawEncoder.cpp
```

或者先放在 `Renderer.cpp`，等 Phase D 再迁入 `ForwardOpaquePass`。

### 5.6 推荐实施步骤

1. 新增 `RenderCommand.h` 和 `RenderQueue.*`。

2. 在 `Scene.h` 增加：

```cpp
void collectRenderCommands(RenderQueue& queue) const;
```

3. 把 `Scene::render()` 的对象遍历逻辑改成 command 收集。

4. 在 `Renderer` 中新增 `drawQueue()`，复制原 `Scene::render()` 的 Vulkan 编码逻辑。

5. 修改 `Application::mainLoop()`：

```text
queue.clear()
currentScene->collectRenderCommands(queue)
queue.sortOpaque()
renderer->beginRenderPass(...)
renderer->drawQueue(..., queue)
gui->render(...)
renderer->endRenderPass(...)
```

6. 确认 `Scene.cpp` 不再调用 `vkCmd*`。

7. 添加简单统计日志或 ImGui 输出：

```text
draw count
material count
mesh count
```

### 5.7 验收标准

本阶段完成后必须满足：

- `Scene.cpp` 不再调用 `vkCmdBindPipeline`、`vkCmdBindDescriptorSets`、`vkCmdPushConstants`、`vkCmdDrawIndexed`。
- `Scene` 只负责收集 `RenderCommand`。
- 当前画面输出不变。
- `RenderQueue` 至少能统计 opaque draw count。
- 后续剔除、排序、shadow caster 分类可以在 `RenderQueue` 层添加。

### 5.8 排序策略

第一版只做稳定、低风险排序：

```text
Opaque:
  material pointer
  mesh pointer
```

不要急着做复杂 sort key。等 `MaterialTemplate` 和 `PipelineCache` 出来后再改成：

```text
pass -> pipeline -> material -> mesh -> depth
```

---

## 6. Phase D：Pass 抽象和 RenderPipeline

### 6.1 目标

让 `Application` 不再直接控制 Vulkan render pass。

本阶段完成后，主流程应从：

```text
Application:
  beginFrame
  updateGlobalUBO
  renderer.beginRenderPass
  renderer.drawQueue
  gui.render
  renderer.endRenderPass
  endFrame
```

演进为：

```text
Application:
  update scene/input/gui
  renderer.render(scene, camera, gui)
```

短期也可以保留 `FrameSync::beginFrame/endFrame` 在 `Application`，但 `beginRenderPass/endRenderPass` 不应继续暴露给 `Application`。

### 6.2 新增核心类型

建议新增：

```text
src/render/RenderFrame.h
src/render/RenderPipeline.h/.cpp
src/render/pass/IRenderPass.h
src/render/pass/ForwardOpaquePass.h/.cpp
src/render/pass/GuiPass.h/.cpp
```

第一版可以不建 `RenderGraph`。

### 6.3 RenderFrameContext

```cpp
struct RenderFrameContext {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint32_t frameIndex = 0;
    uint32_t imageIndex = 0;
    VkExtent2D extent{};
};
```

后续可以继续加入：

```cpp
FrameDescriptorSet frameSet;
TransientResourceAllocator transientResources;
GpuTimestampRecorder profiler;
```

### 6.4 IRenderPass

```cpp
class IRenderPass {
public:
    virtual ~IRenderPass() = default;

    virtual std::string_view name() const = 0;
    virtual void onResize(const SwapChain& swapChain) = 0;

    virtual void execute(const RenderFrameContext& frame,
                         const RenderQueue& queue) = 0;
};
```

第一版不强制 `RenderWorld` 参数。等 Light、Camera snapshot、Shadow 等进入后再补。

### 6.5 ForwardOpaquePass

`ForwardOpaquePass` 接管当前 `Renderer` 中和主 render pass 相关的内容：

```text
VkRenderPass
framebuffers
MSAA color image
depth image
begin/end render pass
draw opaque queue
```

也可以命名为 `MainForwardPass`，避免和未来透明/alpha pass 混淆。

### 6.6 GuiPass

第一版 `GuiPass` 可以只是包装 `GuiSystem::render(cmd)`。

因为当前 ImGui 依赖主 render pass，早期有两种实现方式：

方案 A：`ForwardOpaquePass` 在 render pass 内最后调用 `GuiSystem::render()`。

方案 B：`GuiPass` 是 `ForwardOpaquePass` 的一个子步骤，由 `RenderPipeline` 明确排序但共享同一个 Vulkan render pass。

建议先用方案 A 或 B，不要为了 GUI 单独创建新的 Vulkan render pass。

### 6.7 Renderer 职责重塑

Phase D 后，`Renderer` 应变成一帧调度器：

```cpp
class Renderer {
public:
    void render(Scene& scene, Camera& camera, GuiSystem& gui);
    void onSwapChainRecreated();

private:
    RenderQueue queue_;
    RenderPipeline pipeline_;
};
```

如果暂时仍由 `Application` 管 `FrameSync`，也可以过渡为：

```cpp
void Renderer::renderFrame(const RenderFrameContext& frame,
                           Scene& scene,
                           Camera& camera,
                           GuiSystem& gui);
```

关键是 `Application` 不再调用 `beginRenderPass/endRenderPass`。

### 6.8 推荐实施步骤

1. 新增 `RenderFrameContext`。

2. 新增 `IRenderPass` 和 `RenderPipeline`：

```cpp
class RenderPipeline {
public:
    void addPass(std::unique_ptr<IRenderPass> pass);
    void onResize(const SwapChain& swapChain);
    void execute(const RenderFrameContext& frame, const RenderQueue& queue);
};
```

3. 创建 `ForwardOpaquePass`，先搬迁：

```text
Renderer::createRenderPass
Renderer::createFramebuffers
Renderer::createColorResources
Renderer::createDepthResources
Renderer::beginRenderPass
Renderer::endRenderPass
Renderer::drawQueue
```

4. `Renderer` 保留 frame UBO/global descriptor 管理，或把它拆到 `FrameResources`。短期不必同时大改。

5. 把 resize 流程改成：

```text
Renderer::onSwapChainRecreated()
  -> pipeline.onResize(swapChain)
```

6. 修改 `Application`，去掉对 `beginRenderPass/endRenderPass` 的直接调用。

### 6.9 验收标准

本阶段完成后必须满足：

- `Application.cpp` 不再调用 `Renderer::beginRenderPass()` 和 `Renderer::endRenderPass()`。
- 主 color/depth/framebuffer/render pass 逻辑不再直接堆在 `Renderer` 里。
- `Renderer` 可以执行一个 `RenderPipeline`。
- swapchain resize 可以分发到 pass。
- 当前 GUI 仍正常显示。
- 当前画面输出不变。

---

## 7. Phase B-complete：材质系统完整化

### 7.1 进入条件

只有在 B-lite、C、D 完成后，才开始本阶段。

原因是完整材质系统会同时影响：

```text
Material
PipelineConfig
Pipeline
GltfLoader
SceneFactory
RenderQueue
ForwardOpaquePass
```

如果在 Queue/Pass 边界稳定前做，会放大改动范围。

### 7.2 目标

拆出：

```text
MaterialTemplate: shader, descriptor layout, pipeline layout, render state
MaterialInstance: texture, material params, descriptor set
PipelineCache: template + pass + attachment format -> Pipeline
```

### 7.3 推荐设计

```cpp
struct MaterialTemplateDesc {
    std::string name;
    std::string vertShaderPath;
    std::string fragShaderPath;
    VertexLayout vertexLayout;
    RenderState renderState;
};

class MaterialTemplate {
public:
    VkDescriptorSetLayout materialSetLayout() const;
    const MaterialTemplateDesc& desc() const;
};

struct MaterialInstance {
    Handle<MaterialTemplateTag> templateHandle;
    MaterialParams params;
    std::array<Handle<TextureTag>, kMaxMaterialTextures> textures;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
};
```

Pipeline 不建议由 `MaterialTemplate` 自己直接持有。更清楚的边界是：

```text
MaterialTemplate 提供 shader/layout/state
PipelineCache 根据 pass 和 attachment format 创建具体 Pipeline
```

### 7.4 PipelineCache

```cpp
struct PipelineKey {
    Handle<MaterialTemplateTag> materialTemplate;
    PassId pass;
    VkFormat colorFormat;
    VkFormat depthFormat;
    VkSampleCountFlagBits samples;
    RenderState state;
};

class PipelineCache {
public:
    Pipeline& getOrCreate(const PipelineKey& key);
};
```

第一版可以只支持 `ForwardOpaquePass`。

### 7.5 glTF PBR texture slots

固定 slots：

```text
BaseColor
Normal
MetallicRoughness
Occlusion
Emissive
```

缺失贴图使用 fallback textures：

```text
white
black
flat normal
```

### 7.6 验收标准

- 多个 glTF material 可以共享同一个 `MaterialTemplate`。
- `MaterialInstance` 不依赖 `Renderer`。
- pipeline 创建不再依赖“第一个对象的材质”。
- `ForwardOpaquePass` 根据 command 的 material/template 获取 pipeline。
- 缺失贴图有统一 fallback。

---

## 8. Phase E：SceneGraph 和 glTF 层级

### 8.1 进入条件

建议在 RenderQueue 和 Pass 抽象稳定后再做。

原因是 SceneGraph 的价值主要体现在：

```text
node transform -> RenderCommand world matrix
node bounds -> culling
node animation target -> skeletal or transform animation
node light component -> RenderWorld lights
```

这些都依赖 Queue/Pass 边界。

### 8.2 目标

新增：

```text
src/scene/Transform.h
src/scene/SceneNode.h/.cpp
src/scene/MeshRenderer.h
src/scene/Bounds.h
```

`SceneObject` 可以保留为 OBJ demo 的兼容层。

### 8.3 验收标准

- glTF node 层级被保留。
- 父节点 transform 改变时，子节点 world transform 自动更新。
- `Scene::collectRenderCommands()` 从 `SceneNode + MeshRenderer` 生成命令。
- `RenderCommand` 可以携带 `worldBounds`。

---

## 9. Phase F：Light、Shadow、PostProcess

### 9.1 进入条件

必须等以下条件成立：

- Scene 不再直接 draw。
- Renderer 可以执行 pass。
- Material descriptor set 已经和 global descriptor set 解耦。

### 9.2 Light

先加入基础 light component：

```cpp
enum class LightType {
    Directional,
    Point,
    Spot,
};

struct Light {
    LightType type = LightType::Directional;
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    bool castsShadow = false;
};
```

GPU 数据放入 `set 0` 的 light buffer。

### 9.3 Shadow

第一版只做一个方向光 shadow map：

```text
ShadowPass:
  input: shadow caster queue
  output: depth texture

ForwardOpaquePass:
  input: shadow depth texture
  output: main color/depth
```

不要第一版就做 cascaded shadow maps 或 point light cube shadow。

### 9.4 PostProcess

第一版只做：

```text
ForwardOpaquePass -> offscreen color
PostProcessPass -> swapchain
```

效果先做：

```text
tonemap
gamma correction
```

Bloom、FXAA、HDR exposure 调整可以后续补。

---

## 10. 近期最小任务拆分

为了便于实际执行，近期可以拆成这些小任务：

### Task 1：Global descriptor set

范围：

```text
Renderer
PipelineConfig 构造点
shader set/binding
```

完成标准：

- global UBO 独立为 set 0。
- 当前画面不变。

### Task 2：Material texture set

范围：

```text
Material
GltfLoader
BuiltinScenes
SceneFactory
```

完成标准：

- `Material` 不再接收 `Renderer&`。
- texture 独立为 set 1。

### Task 3：RenderCommand / RenderQueue

范围：

```text
render/RenderCommand.h
render/RenderQueue.*
Scene
Application
Renderer
```

完成标准：

- `Scene.cpp` 不再调用 `vkCmd*`。
- `Renderer` 或临时 encoder 负责 draw queue。

### Task 4：ForwardOpaquePass

范围：

```text
render/pass/ForwardOpaquePass.*
render/RenderPipeline.*
Renderer
Application
```

完成标准：

- `Application` 不再 begin/end render pass。
- 主 pass 由 `ForwardOpaquePass` 执行。

### Task 5：GuiPass 包装

范围：

```text
render/pass/GuiPass.*
GuiSystem
RenderPipeline
```

完成标准：

- GUI 渲染纳入 pipeline 执行顺序。
- 不要求 GUI 独立 Vulkan render pass。

---

## 11. 每阶段回归检查

每个 task 完成后至少检查：

```text
cmake build
app launch
scene switch
swapchain resize
validation layer output
gltf model display
imgui display
```

如果某阶段出现渲染差异，先确认：

- shader set/binding 是否和 C++ descriptor layout 一致。
- pipeline layout descriptor set 顺序是否一致。
- descriptor set 绑定 firstSet 是否正确。
- swapchain resize 后 framebuffer/depth/MSAA 是否重建。
- old scene switch 是否触发了 device idle 或安全释放。

---

## 12. 文档同步规则

后续维护建议：

- `2026_05_render_architecture_evolution.md` 保持为长期架构蓝图。
- 本文保持为近期执行计划。
- 每完成一个 task，在本文对应章节追加状态。
- 如果实现中发现计划不合理，优先更新本文，再继续编码。

建议状态格式：

```text
状态：未开始 / 进行中 / 已完成 / 暂缓
完成提交：
遗留问题：
```

---

## 13. 当前推荐下一步

下一步从 `Task 1：Global descriptor set` 开始。

第一轮不要同时修改 RenderQueue 或 Pass。只处理 descriptor set 解耦：

```text
set 0: GlobalUBO
set 1: Material baseColor texture
```

这一步完成后，`Material -> Renderer` 会被切断。之后再做 RenderQueue，风险会小很多。

---

## 14. B-lite 开始前准备记录

状态：已完成准备，尚未开始代码修改。

准备日期：2026-05-12

### 14.1 基线构建

已执行：

```text
cmake --build build-debug
```

结果：

- 构建通过。
- CMake 会在构建中自动编译 `shader/shader.vert` 和 `shader/shader.frag` 到 `shader/vert.spv`、`shader/frag.spv`。
- 当前存在一个已有链接警告：`LNK4098: 默认库“MSVCRT”与其他库的使用冲突`。它不是 B-lite 改动引入的问题，后续不应把它误判为 descriptor 解耦导致的失败。

### 14.2 当前 descriptor 绑定基线

shader 当前状态：

```text
shader/shader.vert
  layout(binding = 0) uniform UniformBufferObject

shader/shader.frag
  layout(binding = 1) uniform sampler2D texSampler
```

因为 shader 里没有显式 `set = ...`，当前两者都等价于 set 0：

```text
set 0 binding 0 = GlobalUBO
set 0 binding 1 = baseColor texture
```

B-lite 目标状态：

```text
set 0 binding 0 = GlobalUBO
set 1 binding 0 = baseColor texture
```

### 14.3 当前 C++ 绑定基线

当前关键路径：

```text
Application
  -> 用首个 Material 的 PipelineConfig 创建 opaquePipeline_

Material
  -> 创建一个 descriptor set layout
  -> binding 0 写入 Renderer 的 per-frame GlobalUBO
  -> binding 1 写入 baseColor texture
  -> bindDescriptors(... firstSet = 0 ...)

Scene::render
  -> material->bindDescriptors(cmd, pipeline.layout(), frameIndex)
  -> push constants
  -> mesh draw
```

B-lite 需要改变为：

```text
Renderer 或 frame descriptor owner
  -> 拥有 global descriptor set layout
  -> 拥有 per-frame global descriptor sets
  -> bindGlobalDescriptors(... firstSet = 0 ...)

Material
  -> 只拥有 material texture descriptor set layout
  -> 只写入 texture descriptor
  -> bindDescriptors(... firstSet = 1 ...)
```

### 14.4 B-lite 第一轮建议改动范围

第一轮只触碰这些文件：

```text
shader/shader.vert
shader/shader.frag
src/render/Renderer.h
src/render/Renderer.cpp
src/render/Material.h
src/render/Material.cpp
src/app/Application.cpp
src/scene/SceneFactory.h
src/scene/BuiltinScenes.cpp
src/render/GltfLoader.h
src/render/GltfLoader.cpp
```

暂不触碰：

```text
RenderQueue
RenderPipeline
IRenderPass
MaterialTemplate
MaterialInstance
ResourceManager
SceneGraph
```

### 14.5 B-lite 开始条件

开始 B-lite 前确认：

- 当前基线构建已通过。
- 清楚当前 worktree 不是干净状态，后续改动要避免回滚已有文件。
- 本轮目标仅为 descriptor set 解耦，不引入 RenderQueue 或 Pass 抽象。

### 14.6 Git 资产提交规则

准备提交基线前，已调整模型资产规则：

```text
models/viking_room.obj      默认场景依赖，小资产，允许提交
models/SheenChair.glb       glTF 基础回归样例，允许提交
models/*.glb                其它本地大模型默认忽略
models/local/               本地临时资产目录，忽略
models/sample/              本地样例资产目录，忽略
```

原因：

- 大型 glTF 样例资产会快速膨胀仓库体积。
- B-lite 和后续架构重构不依赖额外大模型进入 Git。
- 如果某个新场景必须作为回归样例长期存在，应先在本文说明理由，再在 `.gitignore` 中显式白名单该具体文件。

同时，`main.cpp` 中的额外 glTF 场景应按“文件存在才注册”的方式处理。这样本地可以继续使用大模型测试，但 fresh checkout 不会因为缺少未提交资产而出现不可加载场景。

### 14.7 Master 基线状态

状态：已合并到 `master`，可以开始 B-lite。

基线提交：

```text
eef974d chore: prepare render architecture baseline
```

合并方式：

```text
feature/gltf-v1 -> master
fast-forward
```

验证：

```text
cmake --build build-debug
```

结果：

- `master` 构建通过。
- `shader/shader.vert` 和 `shader/shader.frag` 会在构建中自动编译。
- 本地额外大模型仍被 Git 忽略：
  - `models/ABeautifulGame.glb`
  - `models/AnisotropyBarnLamp.glb`
  - `models/CarConcept.glb`
  - `models/ChronographWatch.glb`
  - `models/DiffuseTransmissionTeacup.glb`
  - `models/PotOfCoals.glb`

B-lite 应从当前 `master` 开始，第一步仍是：

```text
Task 1: Global descriptor set
```
