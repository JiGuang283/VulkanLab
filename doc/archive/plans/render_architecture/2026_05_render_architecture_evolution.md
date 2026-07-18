# VulkanLab 后续开发架构完善设计（2026-05）

## 0. 结论摘要

当前项目已经完成了 Vulkan 教程式渲染器最重要的第一步：窗口、Vulkan instance/device/swapchain、VMA Buffer/Image、FrameSync、Texture/Mesh/Material、glTF 资源加载、ImGui 面板和多场景切换都已经拆到了独立模块里。底层 RAII 质量不错，代码也还能读。

但从“继续加功能”的角度看，现在的主干仍是单通道前向渲染：`Application` 直接驱动 `Renderer::beginRenderPass()`，`Scene::render()` 直接遍历对象并发出 `vkCmd*`，`Material` 直接依赖 `Renderer` 的 per-frame UBO，`Pipeline` 由第一个材质的 descriptor layout 拼出来。这个结构能跑 demo，但没有稳定的扩展面。

要支持阴影、后处理、延迟渲染、骨骼动画、实例化、剔除等进阶功能，最小必要架构应补齐五块地基：

1. **RenderQueue / RenderCommand**：Scene 不再直接 draw，而是收集可排序、可剔除、可分 Pass 的绘制命令。
2. **Pass 抽象 / 轻量 RenderGraph**：Renderer 不再只知道一个主 VkRenderPass，而是按 Pass 编排 Shadow、Forward、Gui、PostProcess 等阶段。
3. **资源句柄与资源库**：Texture、Mesh、MaterialInstance、RenderTarget 等运行时资源从 `shared_ptr` 散落改成 `Handle + Pool`，便于缓存、重载、延迟释放和调试。
4. **MaterialTemplate / MaterialInstance 分离**：Pipeline layout、shader、descriptor layout 属于模板；贴图和参数属于实例；descriptor pool 统一分配。
5. **SceneGraph + Light 系统**：保留 glTF node 层级、局部/世界矩阵、灯光组件和 bounds，为动画、剔除、阴影和多光源铺路。

不建议现在直接做完整 ECS、工业级 FrameGraph、多线程 command buffer 录制或复杂 shader variant 系统。当前仓库规模下，先做“可演进的轻量架构”收益最高。

---

## 1. 当前架构事实

### 1.1 当前模块关系

```text
main.cpp
  -> app/Application
       -> window/Window
       -> window/InputManager
       -> core/VulkanContext
       -> core/Device
       -> core/SwapChain
       -> core/FrameSync
       -> render/Renderer
            owns: main VkRenderPass, framebuffers, MSAA color image, depth image, global UBOs
       -> core/Pipeline
            built once from first scene material's completed PipelineConfig
       -> render/GuiSystem
       -> scene/Scene
            owns: textures/materials/meshes shared_ptr arrays + flat SceneObject array
```

当前一帧主流程在 `Application::mainLoop()` 中大致是：

```text
poll input
update scene
draw imgui widgets
FrameSync::beginFrame()
updateGlobalUBO(frameIndex)
Renderer::beginRenderPass(cmd, imageIndex)
Scene::render(cmd, frameIndex, opaquePipeline)
GuiSystem::render(cmd)
Renderer::endRenderPass(cmd)
FrameSync::endFrame(ctx)
```

这个流程清楚，但它把“本帧有哪些 pass、哪些对象可见、按什么顺序画、哪些资源读写、哪些描述符属于全局/材质/对象”全部压扁到了单个固定通道里。

### 1.2 当前优点

| 领域 | 现状优点 |
|---|---|
| Vulkan 生命周期 | `Buffer` / `Image` 支持 RAII 和 move，VMA 接入正确，析构顺序大体安全。 |
| 帧同步 | `FrameSync` 把 fence/semaphore/command buffer/acquire/present 封装出来，调用点少。 |
| 交换链资源 | `Renderer::recreateSwapChain()` 已经有独立入口，MSAA color/depth/framebuffer 可重建。 |
| 资源加载 | `Texture` 支持 mipmap；`GltfLoader` 已经能解析 glTF 纹理、材质参数、primitive 和 node world transform。 |
| 场景工厂 | `SceneFactory` 已经把场景创建和 Application 主循环分离，便于后续换成资源库/场景描述文件。 |
| UI 集成 | `GuiSystem` 独立，ImGui descriptor pool 和 Vulkan backend 没有污染核心渲染类。 |

### 1.3 当前关键问题

| 问题 | 当前表现 | 直接后果 |
|---|---|---|
| Renderer 是单通道对象 | `Renderer` 拥有主 render pass、framebuffer、MSAA/depth image、全局 UBO，并只提供 begin/end render pass。 | Shadow/GBuffer/PostProcess 没有自然落点。 |
| Scene 直接提交 draw | `Scene::render()` 内部绑定 pipeline、material descriptor、push constants、mesh bind/draw。 | 无法排序、剔除、合批、按 pass 分类或多线程录制。 |
| Material 依赖 Renderer | `Material` 构造需要 `Renderer&`，descriptor set 中直接写 `renderer_->uniformBufferHandle(i)`。 | 材质不能脱离当前主 pass/全局 UBO；descriptor set layout 和资源绑定职责混乱。 |
| Pipeline 由首个材质决定 | Application 用首个对象材质的 `pipelineConfig()` 创建单个 `opaquePipeline_`。 | 多材质模板、多 shader、多 pass pipeline cache 无法表达。 |
| Descriptor pool 分散 | 每个 `Material` 自己创建 descriptor pool 和 per-frame sets。 | 材质数量上来后 pool 数量膨胀，也无法统一回收/重用。 |
| 无资源句柄 | `Scene` 通过 `shared_ptr<Texture/Material/Mesh>` 保活，GPU 资源没有 generation handle。 | 无资源缓存、热重载、延迟释放、失效检测和引用诊断。 |
| 场景图被压平 | `GltfLoader` 已经遍历 node 计算 world matrix，但最终只产出扁平 `SceneObject{mesh, material, world}`。 | glTF 层级、局部 TRS、动画 channel target、层级 bounds 都丢失。 |
| 无灯光语义 | `GlobalUBO` 只有 view/proj，shader/scene 中没有 Light、Shadow、Environment。 | PBR、阴影、延迟、IBL 都缺入口。 |

---

## 2. 目标架构原则

### 2.1 数据流从“立即 draw”改成“收集 -> 编排 -> 编码”

推荐目标数据流：

```text
Application
  updates Scene + Camera + UI
       |
       v
SceneGraph / Components
  update transforms, lights, animations
       |
       v
RenderSceneBuilder
  produce RenderWorld snapshot: camera, lights, visible/renderable candidates
       |
       v
RenderQueue
  cull + classify + sort RenderCommands
       |
       v
RenderPipeline / RenderGraph
  execute ShadowPass -> ForwardPass -> GuiPass/PostProcessPass
       |
       v
Core Vulkan objects
```

核心变化是：Scene 只表达世界，RenderQueue 表达本帧要画什么，Pass 负责怎么画，Core 负责 Vulkan 对象和命令。

### 2.2 保持轻量，不做一步到位的大引擎

当前只需要以下轻量版：

- Pass 列表 + 显式资源声明，而不是完整自动调度 FrameGraph。
- SceneNode + 简单组件，而不是完整 ECS。
- `Handle<T> + ResourcePool<T>`，而不是复杂 GC/引用追踪系统。
- 单线程 command buffer 录制，先不做 secondary command buffer 并行。
- 文件名/枚举驱动 shader/material template，先不做 permutation 爆炸。

### 2.3 Vulkan 概念与引擎概念分离

文档里 “Pass” 指引擎层 pass，如 `ShadowPass`、`ForwardOpaquePass`、`GuiPass`。它不等同于 Vulkan `VkRenderPass`。早期可以仍用 `VkRenderPass` 实现图形 pass，但上层接口要避免被单个 swapchain render pass 锁死。

---

## 3. 建议目标分层

### 3.1 app 层

职责：创建系统、注册场景、驱动主循环。

保留：

- `Application`
- `Config`

建议新增：

- `EngineContext` 或 `RuntimeContext`：把 `Device`、`SwapChain`、`FrameSync`、`ResourceManager`、`Renderer` 等基础设施引用聚合，降低 Application 手工传参压力。

注意：`EngineContext` 是过渡性 Facade，不应变成全局变量。构造时传引用，生命周期仍由 Application 控制。

### 3.2 core 层

职责：Vulkan/RHI 基础设施，不认识 Scene、Material 语义。

保留并强化：

- `VulkanContext`
- `Device`
- `SwapChain`
- `FrameSync`
- `Buffer`
- `Image`
- `Pipeline`

建议新增：

```text
core/
  ResourceHandle.h       // Handle<T>, generation, invalid handle
  ResourcePool.h         // slot pool, delayed release hook
  DescriptorAllocator.h  // descriptor pool pages
  DescriptorLayoutCache.h
  PipelineCache.h        // PipelineKey -> Pipeline
  CommandContext.h       // 可从 FrameSync 中逐步拆出一次性命令/帧命令
  VulkanException.h
```

`FrameSync` 现在同时负责帧 acquire/present 和 single-time command。短期可保留；中期建议把 upload command helper 拆到 `CommandContext`，让纹理/mesh 上传不依赖“帧同步系统”。

### 3.3 resource 层

当前没有单独 resource 目录，资源加载散在 `Texture`、`Mesh::fromOBJ`、`GltfLoader` 和场景工厂中。

建议新增：

```text
resource/
  ResourceManager.h/.cpp
  TextureLoader.h/.cpp
  MeshLoader.h/.cpp
  GltfSceneLoader.h/.cpp
  ShaderLibrary.h/.cpp
```

职责：磁盘资源到运行时资源的缓存与去重。

第一阶段不必强行移动全部代码；可以先让 `ResourceManager` 包装现有 `Texture` / `Mesh` / `GltfLoader`。

### 3.4 scene 层

职责：世界表达，不直接调用 Vulkan。

建议演进为：

```text
scene/
  Scene.h/.cpp
  SceneNode.h/.cpp
  Transform.h
  MeshRenderer.h
  Light.h/.cpp
  Camera.h/.cpp
  Bounds.h
  Animation.h/.cpp       // 后续
```

目标对象：

```cpp
using NodeId = Handle<SceneNodeTag>;

struct Transform {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    glm::mat4 localMatrix() const;
};

struct SceneNode {
    NodeId parent;
    std::vector<NodeId> children;
    Transform local;
    glm::mat4 world{1.0f};
    bool dirty = true;
};

struct MeshRenderer {
    Handle<MeshTag> mesh;
    Handle<MaterialInstanceTag> material;
    Bounds localBounds;
    NodeId node;
};
```

`SceneObject` 可以先保留为兼容层，但新渲染路径应逐渐改成 `SceneNode + MeshRenderer`。

### 3.5 render 层

职责：从场景快照生成 GPU 命令。

建议新增：

```text
render/
  Renderer.h/.cpp              // 一帧调度器，不再拥有所有 pass 细节
  RenderFrame.h/.cpp           // 当前帧上下文：cmd, frameIndex, imageIndex, dynamic buffers
  RenderWorld.h                // camera/lights/renderables 的只读快照
  RenderCommand.h
  RenderQueue.h/.cpp
  RenderPipeline.h/.cpp
  RenderGraph.h/.cpp           // 轻量版
  MaterialTemplate.h/.cpp
  MaterialInstance.h/.cpp
  DescriptorSetManager.h/.cpp
  pass/
    IRenderPass.h
    ForwardOpaquePass.h/.cpp
    GuiPass.h/.cpp
    ShadowPass.h/.cpp
    PostProcessPass.h/.cpp
```

`Renderer` 的目标职责变成：

```text
Renderer::render(scene, camera)
  -> build RenderWorld
  -> build/cull/sort RenderQueue
  -> begin frame
  -> execute RenderPipeline passes
  -> end frame
```

---

## 4. 关键模块设计

### 4.1 ResourceHandle + ResourcePool

必要设计模式：Handle/Pool。

当前 `shared_ptr` 适合保活，但不适合引擎级资源管理：无法检测悬空引用、无法统一延迟销毁、无法资源热重载，也不利于 RenderQueue 中存储轻量 ID。

建议句柄：

```cpp
template <typename Tag>
struct Handle {
    uint32_t index = UINT32_MAX;
    uint32_t generation = 0;

    bool valid() const { return index != UINT32_MAX; }
    explicit operator bool() const { return valid(); }
};

struct TextureTag {};
struct MeshTag {};
struct MaterialTemplateTag {};
struct MaterialInstanceTag {};
struct RenderTargetTag {};
```

资源池：

```cpp
template <typename T, typename Tag>
class ResourcePool {
public:
    using HandleT = Handle<Tag>;

    HandleT insert(T value);
    T* get(HandleT handle);
    const T* get(HandleT handle) const;
    void release(HandleT handle);
    bool alive(HandleT handle) const;

private:
    struct Slot {
        std::optional<T> value;
        uint32_t generation = 1;
    };
    std::vector<Slot> slots_;
    std::vector<uint32_t> freeList_;
};
```

落地顺序：

1. 先给 `Texture` 和 `Mesh` 做 pool，不改变底层类本身。
2. RenderCommand 中只存 `Handle<MeshTag>` / `Handle<MaterialInstanceTag>`。
3. `Scene` 中从 `shared_ptr` 过渡到 handle。过渡期可以同时保留旧数组，避免一次性大改。
4. 最后把 glTF/OBJ 加载入口改为 `ResourceManager::loadTexture/loadMesh/loadGltfScene`。

延迟释放：Vulkan 资源不能在 GPU 仍使用时立即销毁。`ResourcePool::release` 早期可以要求调用前 `vkDeviceWaitIdle`；中期接入 per-frame deferred deletion queue：

```text
release(handle)
  -> move resource into deletionQueue[currentFrame + MAX_FRAMES_IN_FLIGHT]
  -> frame begin 时销毁安全帧之前的资源
```

### 4.2 DescriptorSet 分层

当前 Material descriptor set 同时绑定全局 UBO 和材质贴图：

```text
binding 0 = GlobalUBO
binding 1 = baseColor texture
```

这会让所有材质实例重复写全局 UBO descriptor，也让全局数据与材质数据生命周期绑死。

建议固定 descriptor set 语义：

```text
set 0: Frame / View 全局数据
  binding 0 = GlobalFrameUBO or SSBO
  binding 1 = Light buffer
  binding 2 = Shadow maps / samplers（后续）

set 1: Material 数据
  binding 0 = material parameter buffer
  binding 1..N = textures

set 2: Object / Instance 数据（后续）
  binding 0 = object transform buffer or dynamic UBO / SSBO
```

短期仍可用 push constants 传 model matrix 和少量材质参数，但 `set 0` 和 `set 1` 要先拆开。

新增 `DescriptorSetManager`：

```cpp
class DescriptorSetManager {
public:
    VkDescriptorSet allocate(VkDescriptorSetLayout layout);
    void resetFramePools(uint32_t frameIndex);
    void release(VkDescriptorSet set); // 可选，早期可不单独 free
};
```

它负责复用 descriptor pool pages，取代“一个 Material 一个 descriptor pool”。

### 4.3 MaterialTemplate / MaterialInstance

必要设计模式：Template/Instance。

当前 `Material` 同时做了三件事：

1. 定义 descriptor layout。
2. 持有 per-frame descriptor sets。
3. 存储材质参数和 texture。

建议拆成：

```cpp
struct MaterialTemplateDesc {
    std::string name;
    std::string vertShaderPath;
    std::string fragShaderPath;
    VertexLayout vertexLayout;
    RenderState state;
    MaterialLayout layout; // texture slots, parameter buffer layout
};

class MaterialTemplate {
public:
    VkDescriptorSetLayout materialSetLayout() const;
    VkPipelineLayout pipelineLayoutFor(PassId pass) const;
    Handle<PipelineTag> pipelineFor(PassId pass, RenderPassFormatKey key) const;
};

struct MaterialInstance {
    Handle<MaterialTemplateTag> templateHandle;
    MaterialParams params;
    std::array<Handle<TextureTag>, kMaxMaterialTextures> textures;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
};
```

`MaterialTemplate` 决定 shader、pipeline layout、descriptor layout、渲染状态；`MaterialInstance` 只决定某个模型使用哪些贴图和参数。

对 glTF PBR，建议材质 slot 固定为：

```text
BaseColor
Normal
MetallicRoughness
Occlusion
Emissive
```

缺失贴图统一指向 fallback texture，如 white/black/flat-normal。

### 4.4 PipelineConfigBuilder 和 PipelineCache

必要设计模式：Builder。

当前 `PipelineConfig` 是纯 struct，并且会被 Material 构造阶段修改 `descriptorLayouts`。这让 pipeline key 很难稳定。

建议拆出：

```cpp
struct RenderState {
    VkCullModeFlags cullMode;
    VkFrontFace frontFace;
    bool depthTest;
    bool depthWrite;
    VkCompareOp depthCompare;
    bool blendEnable;
    VkSampleCountFlagBits samples;
};

struct PipelineKey {
    Handle<MaterialTemplateTag> materialTemplate;
    PassId pass;
    VkFormat colorFormat;
    VkFormat depthFormat;
    VkSampleCountFlagBits samples;
    RenderState state;
};
```

`PipelineCache` 负责：

```cpp
class PipelineCache {
public:
    Pipeline& getOrCreate(const PipelineKey& key);
    void invalidateShaders(const std::string& path); // 后续热重载
};
```

早期可以只支持当前 opaque pipeline，先把 key 结构和 cache 入口建起来。

### 4.5 RenderCommand / RenderQueue

必要设计模式：Command + Strategy。

Scene 不再直接 `vkCmdBindPipeline`，而是产出本帧绘制命令：

```cpp
enum class RenderQueueType {
    Opaque,
    AlphaTest,
    Transparent,
    ShadowCaster,
    Overlay,
};

struct RenderCommand {
    Handle<MeshTag> mesh;
    Handle<MaterialInstanceTag> material;
    glm::mat4 world;
    Bounds worldBounds;
    RenderQueueType queue;
    uint64_t sortKey = 0;
};
```

`RenderQueue`：

```cpp
class RenderQueue {
public:
    void clear();
    void add(RenderCommand command);
    void cull(const Frustum& frustum, const ICullingStrategy& strategy);
    void sort(const ISortStrategy& strategy);

    std::span<const RenderCommand> opaque() const;
    std::span<const RenderCommand> transparent() const;
    std::span<const RenderCommand> shadowCasters() const;
};
```

排序策略：

```text
Opaque:       pass -> pipeline/material -> mesh -> front-to-back
Transparent: pass -> pipeline/material -> back-to-front
Shadow:      pipeline/material -> mesh
```

早期不必做复杂 sort key，先实现稳定顺序和材质优先即可。

### 4.6 Pass 抽象与轻量 RenderGraph

必要设计模式：Strategy/Template Method 的轻量变体。

不要现在做完整 RenderGraph 自动资源推导。先做显式 Pass 接口：

```cpp
struct RenderFrameContext {
    VkCommandBuffer cmd;
    uint32_t frameIndex;
    uint32_t imageIndex;
    VkExtent2D extent;
};

struct RenderPassContext {
    Device& device;
    SwapChain& swapChain;
    ResourceManager& resources;
    DescriptorSetManager& descriptors;
    PipelineCache& pipelines;
};

class IRenderPass {
public:
    virtual ~IRenderPass() = default;
    virtual std::string_view name() const = 0;
    virtual void onResize(const SwapChain& swapChain) = 0;
    virtual void execute(const RenderFrameContext& frame,
                         const RenderWorld& world,
                         const RenderQueue& queue) = 0;
};
```

第一批 pass：

```text
ForwardOpaquePass
  owns/uses: main color/depth attachments, framebuffer, render pass compatibility
  consumes: queue.opaque()

GuiPass
  consumes: ImGui draw data
  early version: still records inside ForwardOpaquePass render pass as final sub-step

ShadowPass
  owns/uses: depth image array/cascade texture, shadow framebuffer
  consumes: queue.shadowCasters()

PostProcessPass
  owns/uses: fullscreen pipeline, HDR input image, swapchain output
```

轻量 `RenderPipeline`：

```cpp
class RenderPipeline {
public:
    void addPass(std::unique_ptr<IRenderPass> pass);
    void onResize(const SwapChain& swapChain);
    void execute(const RenderFrameContext& frame,
                 const RenderWorld& world,
                 const RenderQueue& queue);
};
```

后续如果 pass 数量变多，再把它升级为真正的 RenderGraph：pass 声明 read/write resources，graph 自动插 barrier、决定执行顺序和资源复用。

### 4.7 RenderTarget / Attachment 资源

阴影、GBuffer、后处理都需要 offscreen image。建议新增 `RenderTarget` 或 `TransientImage`：

```cpp
struct RenderTargetDesc {
    uint32_t width;
    uint32_t height;
    VkFormat format;
    VkImageUsageFlags usage;
    VkSampleCountFlagBits samples;
    std::string debugName;
};

class RenderTarget {
public:
    Image& image();
    VkImageView view() const;
    VkFormat format() const;
};
```

早期由各 Pass 自己持有 RenderTarget；中期再交给 RenderGraph 统一创建和复用。

### 4.8 SceneGraph

必要设计模式：Composite。

当前 glTF loader 已经能递归 node 并算 world matrix，但最终压平为 `SceneObject`。下一步应保留 node：

```cpp
class Scene {
public:
    NodeId createNode(NodeId parent, Transform local);
    MeshRendererId addMeshRenderer(NodeId node,
                                   Handle<MeshTag> mesh,
                                   Handle<MaterialInstanceTag> material,
                                   Bounds localBounds);
    LightId addLight(NodeId node, Light light);

    void updateWorldTransforms();
    void collectRenderables(RenderWorldBuilder& builder) const;
};
```

dirty propagation：

```text
setLocalTransform(node)
  -> mark node and descendants dirty

updateWorldTransforms()
  -> root to leaf update world matrix only for dirty branches
```

这样 glTF animation 后续只需要修改 node local transform，不需要直接改每个 `SceneObject::transform`。

### 4.9 Light / Shadow

Light 先作为 scene component，不要直接塞进 Renderer：

```cpp
enum class LightType { Directional, Point, Spot };

struct Light {
    LightType type;
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    float innerCone = 0.0f;
    float outerCone = 0.0f;
    bool castsShadow = false;
};
```

`LightManager` 或 `RenderWorldBuilder` 每帧把 scene light 转成 GPU 紧凑结构：

```cpp
struct GpuLight {
    glm::vec4 position_type;   // xyz + type
    glm::vec4 direction_range; // xyz + range
    glm::vec4 color_intensity; // rgb + intensity
    glm::vec4 params;          // cone/shadow index/etc
};
```

阴影第一阶段只做方向光单张 shadow map：

```text
ShadowPass
  input: directional light, shadow caster queue
  output: depth texture

ForwardOpaquePass
  input: shadow depth texture + light buffer + opaque queue
  output: swapchain color/depth
```

等基础稳定后再做 cascaded shadow maps、point light cube shadow 或 clustered lights。

### 4.10 剔除与实例化

剔除建议作为 RenderQueue 的策略，而不是写进 Scene：

```cpp
class ICullingStrategy {
public:
    virtual bool visible(const CameraFrustum& frustum,
                         const Bounds& bounds) const = 0;
};

class FrustumCullingStrategy : public ICullingStrategy {};
class NoCullingStrategy : public ICullingStrategy {};
```

实例化不应提前改 Mesh/Material。等 RenderQueue 有了之后，可以按 `(mesh, material, pipeline)` 分组：

```text
RenderQueue commands
  -> group by mesh + material
  -> upload instance transforms
  -> vkCmdDrawIndexed(..., instanceCount)
```

因此实例化依赖 RenderQueue，不应该在当前 `Scene::render()` 时代硬塞。

---

## 5. 推荐目录演进

保持现有目录基础上增量新增：

```text
src/
  app/
    Application.*
    Config.h
    EngineContext.h             // 新增，可选

  core/
    Buffer.*
    Device.*
    FrameSync.*
    Image.*
    Pipeline.*
    PipelineConfig.h
    PipelineConfigBuilder.h     // 新增
    PipelineCache.*             // 新增
    DescriptorAllocator.*       // 新增
    DescriptorLayoutCache.*     // 新增
    ResourceHandle.h            // 新增
    ResourcePool.h              // 新增
    VulkanException.*           // 新增

  resource/                     // 新增目录
    ResourceManager.*
    TextureLoader.*
    MeshLoader.*
    GltfSceneLoader.*
    ShaderLibrary.*

  render/
    Renderer.*
    RenderFrame.*               // 新增
    RenderWorld.h               // 新增
    RenderCommand.h             // 新增
    RenderQueue.*               // 新增
    RenderPipeline.*            // 新增
    RenderGraph.*               // 新增，轻量版
    DescriptorSetManager.*      // 新增
    MaterialTemplate.*          // 新增
    MaterialInstance.*          // 新增
    Mesh.*
    Texture.*
    pass/
      IRenderPass.h
      ForwardOpaquePass.*
      GuiPass.*
      ShadowPass.*
      PostProcessPass.*

  scene/
    Scene.*
    SceneNode.*                 // 新增
    Transform.h                 // 新增
    MeshRenderer.h              // 新增
    Light.*                     // 新增
    Bounds.h                    // 新增
    Camera.*
```

---

## 6. 分阶段路线图

每个阶段结束都应保持现有 demo 能跑。不要开一个长期不可运行的大重构分支。

### Phase A：工程地基，不改变画面

目标：先把后续重构要用的底座放进来。

1. 引入日志系统和增强 `VK_CHECK`。
2. 新增 `ResourceHandle.h` / `ResourcePool.h`，先写单元级自测或简单断言，不立即大规模替换。
3. 新增 `DescriptorAllocator` / `DescriptorSetManager`，先让旧 `Material` 可选使用统一 pool。
4. 新增 `PipelineConfigBuilder`，让 pipeline config 构造不再散落 mutation。

验收：

- 现有所有场景画面不变。
- 项目日志有级别/模块名。
- 能创建并校验一个 `Handle<TextureTag>` / `Handle<MeshTag>`。
- 旧 `Material` 至少不再每个实例创建完全独立的大量 descriptor pool，或已有替换入口。

### Phase B：材质与资源解耦

目标：拆开 MaterialTemplate / MaterialInstance，为多 shader、多贴图和多 pipeline 做准备。

1. 新增 `MaterialTemplate`，承载 shader path、vertex layout、render state、descriptor layout。
2. 新增 `MaterialInstance`，承载 texture handles、PBR factors、descriptor set。
3. 新增 fallback textures：white、black、flat normal。
4. `GltfLoader` 产出 material instances，而不是每个 glTF material 创建一个强耦合旧 `Material`。
5. `PipelineCache` 以 template + pass + attachment format 创建 pipeline。

验收：

- `MaterialInstance` 不再持有 `Renderer*`。
- global UBO descriptor 从 material set 拆出去。
- 多个材质实例能共享同一个 template 和 pipeline layout。

### Phase C：RenderQueue 替代 Scene::render

目标：Scene 不再直接发 Vulkan draw。

1. 新增 `RenderCommand` / `RenderQueue`。
2. `Scene::collectRenderCommands(RenderQueue&)` 替代 `Scene::render()`。
3. `ForwardOpaquePass` 或临时 `Renderer::drawQueue()` 负责编码 command。
4. 增加基础 sort：opaque 按 material/pipeline，再按 mesh。

验收：

- `Scene.cpp` 中不再直接调用 `vkCmdBindPipeline` / `vkCmdDrawIndexed`。
- 同一画面输出不变。
- RenderQueue 能打印本帧 draw count、material count、mesh count。

### Phase D：Pass 抽象和轻量 RenderPipeline

目标：当前主 pass 和 GUI pass 从 Renderer 中拆出去。

1. 新增 `IRenderPass`。
2. 把 `Renderer` 中主 color/depth/framebuffer/renderpass 逻辑迁入 `ForwardOpaquePass` 或 `MainForwardPass`。
3. `GuiSystem` 由 `GuiPass` 包装执行。早期仍可在 Forward pass render pass 内最后调用。
4. `Renderer` 变成 frame orchestration：begin frame -> build queue -> execute pipeline -> end frame。

验收：

- `Renderer` 不再直接暴露 `beginRenderPass/endRenderPass` 给 Application。
- Application 主循环只调用类似 `renderer_->render(*currentScene_, camera_)`。
- swapchain resize 通过 `RenderPipeline::onResize()` 分发给各 pass。

### Phase E：SceneGraph 和 glTF 层级

目标：保留 node 层级，为动画和剔除做准备。

1. 新增 `SceneNode` / `Transform` / dirty world matrix update。
2. `GltfLoader` 或 `GltfSceneLoader` 不再只产出扁平 `SceneObject`，而是创建 node 树和 mesh renderer components。
3. `Scene::collectRenderCommands` 从 node + component 生成 command，并计算 world bounds。
4. 过渡期保留 `SceneObject` 支持 OBJ demo。

验收：

- glTF 层级模型世界变换正确。
- 修改父节点 transform 时子节点自动跟随。
- Bounds 可用于 RenderQueue 剔除。

### Phase F：Light、Shadow、后处理

目标：正式打开进阶画面能力。

1. 新增 `Light` component 和 GPU light buffer。
2. Forward shader 使用 light buffer 做基础 Blinn/Phong 或 PBR direct lighting。
3. 新增 `ShadowPass`，先支持一个方向光 shadow map。
4. Forward pass 采样 shadow map。
5. 新增 `PostProcessPass`，先做 Tonemap/Gamma，再扩展 Bloom/FXAA。

验收：

- 场景中可创建方向光/点光。
- 方向光阴影可见，且 ShadowPass 是独立 pass。
- 后处理 pass 能读取 offscreen color 并输出到 swapchain。

### Phase G：性能与高级功能

目标：基于前面地基添加真正进阶功能。

- Frustum culling。
- GPU timestamp profiler。
- Instancing。
- Indirect draw。
- Skeletal animation。
- Deferred / Clustered Forward+。
- Shader 热重载。

---

## 7. 设计模式使用边界

只建议在下面这些位置使用模式：

| 模式 | 使用位置 | 必要性 |
|---|---|---|
| Handle/Pool | GPU resource 和 scene runtime object | 解决生命周期、失效检测、延迟释放。 |
| Template/Instance | Material | 解决 pipeline/layout 共享和参数实例化。 |
| Command | RenderCommand | 解决 Scene 与 Vulkan draw call 解耦。 |
| Strategy | Culling 和 Sorting | 解决剔除/排序策略可替换。 |
| Composite | SceneNode | 解决 glTF 层级、动画、父子 transform。 |
| Builder | PipelineConfig | 解决 pipeline 配置易错和散落 mutation。 |
| Facade | EngineContext/Renderer | 只作为过渡，降低 Application 复杂度。 |

暂不建议：

- 完整 ECS：现在只有 mesh/light/camera 几类组件，SceneNode + vectors 足够。
- 工业级 RenderGraph：当前 pass 数量少，手工 pipeline 更清楚。
- Service Locator 全局化：容易掩盖依赖，除非只是过渡用 `EngineContext`。
- 多线程 command buffer：RenderQueue 和 Pass 边界稳定后再考虑。

---

## 8. 关键接口草案

### 8.1 Renderer 新入口

```cpp
class Renderer {
public:
    Renderer(RendererCreateInfo createInfo);

    void render(Scene& scene, Camera& camera);
    void onSwapChainRecreated();

private:
    RenderWorldBuilder worldBuilder_;
    RenderQueue queue_;
    RenderPipeline pipeline_;
};
```

Application 不再操作 Vulkan render pass：

```cpp
while (!window.shouldClose()) {
    update(dt);
    gui.beginFrame();
    drawGui();
    renderer.render(*currentScene_, camera_);
    input.endFrame();
}
```

### 8.2 RenderWorld

```cpp
struct RenderView {
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec3 cameraPosition;
    CameraFrustum frustum;
};

struct RenderWorld {
    RenderView view;
    std::span<const GpuLight> lights;
};
```

### 8.3 ForwardOpaquePass

```cpp
class ForwardOpaquePass : public IRenderPass {
public:
    void onResize(const SwapChain& swapChain) override;
    void execute(const RenderFrameContext& frame,
                 const RenderWorld& world,
                 const RenderQueue& queue) override;

private:
    void createRenderPass();
    void createFramebuffers();
    void bindFrameSet(VkCommandBuffer cmd, uint32_t frameIndex);
    void drawCommand(VkCommandBuffer cmd, const RenderCommand& command);
};
```

### 8.4 Scene 收集命令

```cpp
void Scene::collectRenderCommands(RenderQueue& queue) const {
    for (const auto& renderer : meshRenderers_) {
        const auto& node = nodes_.get(renderer.node);
        queue.add(RenderCommand{
            .mesh = renderer.mesh,
            .material = renderer.material,
            .world = node.world,
            .worldBounds = transformBounds(renderer.localBounds, node.world),
            .queue = renderer.isTransparent ? RenderQueueType::Transparent
                                            : RenderQueueType::Opaque,
        });
    }
}
```

---

## 9. 风险与回滚策略

| 风险 | 缓解 |
|---|---|
| 一次性改动太大，画面长时间不可运行 | 按 Phase 推进，每 phase 保持 demo 可运行。 |
| 抽象太多影响学习项目可读性 | 每个新抽象都必须替代一个真实痛点；保留旧路径做短期兼容。 |
| Material 拆分影响 glTF loader 和场景工厂 | 先让旧 `Material` 包装新 `MaterialInstance`，再逐步替换调用点。 |
| RenderPass/VkRenderPass 名称混淆 | 文档和代码中把引擎 pass 命名为 `IRenderPass` 或 `RenderPassNode`，底层 Vulkan 保持 `VkRenderPass`。 |
| 资源句柄引入后生命周期 bug | generation 校验 + debug assert + 延迟释放队列。 |
| Shadow/PostProcess 过早引入导致架构不稳 | 必须等 RenderQueue 和 Pass 抽象稳定后再做。 |

---

## 10. 推荐下一步实际工作

最推荐的前三个实现步骤：

1. **日志 + VulkanException**：低风险，立刻提升调试质量。
2. **Material descriptor 解耦**：先拆 `set 0` global 和 `set 1` material，去掉 `Material -> Renderer` 依赖。
3. **RenderQueue 替换 Scene::render**：让 Scene 不再直接提交 Vulkan 命令，这是所有后续功能的总开关。

这三步完成后，`Renderer` 拆 Pass、SceneGraph、Light/Shadow 都会自然很多。尤其是 RenderQueue 一旦存在，剔除、排序、实例化、shadow caster 分类都能作为局部功能继续加，不会再冲击整个主循环。

---

## 11. 完成定义

当这份架构演进完成到 Phase F，项目应满足：

- Application 不直接 begin/end Vulkan render pass。
- Scene 不直接调用 `vkCmd*`。
- MaterialInstance 不依赖 Renderer。
- Descriptor set 至少分为 global set 和 material set。
- Texture/Mesh/MaterialInstance 可用 handle 引用。
- glTF node 层级和 transform 被保留。
- Renderer 能执行多个 pass。
- Light 数据能上传到 GPU。
- ShadowPass 和 ForwardPass 通过明确资源连接协作。
- 后处理能读取 offscreen color 并输出到 swapchain。

达到这些条件后，再做延迟渲染、骨骼动画、实例化、剔除、Bloom、GPU profiler 都会是“添加 pass/组件/策略”，而不是继续改动 Application、Scene、Material、Renderer 四处互相牵连。