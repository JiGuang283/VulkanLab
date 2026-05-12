# VulkanLab 架构评审与功能规划（2026-04）

> 作者视角：产品经理 + 架构师
> 目标：在不推翻现有分层的前提下，指出代码质量瓶颈、推荐下一步功能路线图，并给出可落地的设计模式与重构建议。
> 评审对象：`src/` 下全部代码（截至当前 commit）。本文不参考已有的其他设计文档，单独成篇。

---

## 0. TL;DR（一页摘要）

- **现状**：仓库已完成"窗口 → 上下文 → 设备 → 交换链 → 帧同步 → 渲染器 → 场景"的线性分层，核心 Vulkan 封装（Buffer/Image/Pipeline/FrameSync）质量较好，但**渲染层仍是典型"教程式单通道"架构**：`Renderer` 是 God Object，`Material` 与 `Renderer` 强耦合，`Scene` 扁平遍历无渲染队列，glTF 节点被压平丢失层级。
- **最大阻塞**：没有 Pass / RenderGraph 抽象、没有资源句柄系统、没有灯光/阴影概念、没有材质模板/实例分离、场景图扁平。任何一项"进阶功能"（阴影、延迟、后处理、骨骼动画、实例化、剔除）目前都落不了地。
- **优先路线**：先补**地基**（日志、句柄、材质拆分、异常上下文）→ 再补**渲染基础设施**（Pass 抽象 + RenderQueue + SceneGraph + 灯光）→ 最后补**高级特性**（阴影、延迟、后处理）。
- **推荐模式**：Handle/Pool、Template-Instance、Builder、Strategy、Observer、Command（RenderCommand）、Composite（SceneGraph）、Service Locator（可选，用于替换当前 init 链）。

---

## 1. 当前架构现状盘点

### 1.1 分层与依赖

```
main.cpp
  └─ app/Application
       ├─ window/Window, InputManager
       ├─ core/VulkanContext → Device → SwapChain → FrameSync
       ├─ core/Pipeline (owned by Application)
       ├─ render/Renderer  ← 直接持有 RenderPass / Framebuffer / 深度与 MSAA 图
       ├─ render/GuiSystem
       └─ scene/Scene (内部持有 SceneObject*，每个 Object 含 Mesh + Material + Transform)
```

**优点**
- 分层边界清晰；构造/析构顺序确定，Vulkan 对象生命周期没有循环依赖。
- Buffer / Image 使用 VMA + RAII + move，风格现代。
- FrameSync 把 semaphore/fence/commandBuffer/current frame index 封装得比较干净。
- GuiSystem 把 ImGui 集成独立出来，不污染主 Renderer。

**问题**
- **Application 是手工 DI 容器**：`init()` 里硬编码 6~7 个子系统的 `make_unique` 顺序。任何新增模块都要改 `Application`，无注册机制、无测试替身入口。
- **Renderer 是 God Object**：同时拥有
  - 主 RenderPass 与 framebuffer
  - Color/Depth MSAA Image
  - 每帧 UBO 与描述符
  - single-time command 辅助函数
  - 对 Scene / GuiSystem 的调度逻辑
  这直接阻塞了多通道（阴影、G-Buffer、后处理）的引入。
- **Pipeline 单一**：`PipelineConfig` 是纯结构体且由 Material 就地 mutate，pipeline 数量上限等于"Material 实例数"，不可共享。
- **Material 与 Renderer 强耦合**：`Material` 需要 RenderPass、MSAA 采样数等 Renderer 内部状态，且自行管理描述符池，一个 Material 一个 Pool，极度浪费。
- **Scene 扁平**：`Scene::render()` 直接 for 循环发出 drawcall；push constants 每对象提交；无排序、无剔除、无批次。
- **glTF 节点被压平**：`GltfLoader` 把所有 mesh 用 identity 世界矩阵塞进 Scene，node 层级与 TRS 丢失 → 无法做骨骼/层级动画。
- **无资源句柄**：Buffer/Image/Texture 都以 `unique_ptr` 或裸指针到处传，use-after-free 没有任何保护，也没有资源缓存/去重。
- **错误处理粗糙**：`VK_CHECK` 抛通用 `runtime_error`，无 VkResult/上下文；`std::cout` 到处都是，没有级别也没有分类。
- **Input / UI 模式硬编码**：UI / CameraDrag 两态在 Application 里 if/else 切换，没有事件总线。

### 1.2 功能矩阵

| 能力 | 现状 | 备注 |
|------|------|------|
| 单通道前向 + MSAA + 深度 | ✅ | Renderer 内硬编码 |
| 基础 OBJ / glTF 加载 | ✅ | glTF 仅 baseColor，节点压平 |
| PBR 贴图（Normal/MR/AO/Emissive） | ❌ | 着色器里没有通道 |
| 灯光系统 | ❌ | 仅 UBO 内一个假定常量 |
| 阴影（shadow map） | ❌ | |
| 延迟渲染 / G-Buffer | ❌ | |
| 后处理（Tonemap / Bloom / FXAA） | ❌ | |
| 骨骼/蒙皮动画 | ❌ | 需要场景图 |
| 视锥/遮挡剔除 | ❌ | |
| 实例化、合批 | ❌ | 一对象一 draw |
| GPU profiling / 时间戳 | ❌ | 仅 CPU FPS |
| 热重载（shader / config） | ❌ | |
| 单元/集成测试 | ❌ | 0 覆盖 |
| 日志系统 | ❌ | `std::cout` |

---

## 2. 推荐添加的功能（含优先级）

优先级按"对后续路线的阻塞性 + 实现成本"打分：P0 = 必须先做；P1 = 下一阶段；P2 = 锦上添花。

### P0 地基类（没有它下一步没法做）
1. **日志系统**：替换 `std::cout`，统一分级（trace/info/warn/error）、分类（core/render/scene/gui）。推荐 `spdlog`。
2. **VulkanException + VK_CHECK 上下文**：携带 VkResult、调用点、文件/行号；为后续"设备丢失恢复"打基础。
3. **资源句柄系统**（`Handle<T>` + `ResourcePool<T>`，带 generation），先落地 Buffer/Image/Texture/Mesh。
4. **MaterialTemplate / MaterialInstance 拆分**：模板承载 pipeline layout 与 descriptor layout，实例承载参数与贴图 slot。
5. **共享 DescriptorSetManager**：按 layout 聚合池，取代"一个 Material 一个池"。

### P1 渲染基础设施（支撑后续所有画面特性）
6. **Pass 抽象 / 轻量 RenderGraph**：第一步只做"Pass 接口 + 手工编排"，不必上满血 FrameGraph；至少把当前 Renderer 拆成 `ForwardPass` 与 `GuiPass`。
7. **RenderQueue + RenderCommand**：Scene 遍历产出 command，排序后再编码，取代 `Scene::render` 直接 `vkCmd*`。
8. **SceneNode（Composite 模式）**：支持 local/world 矩阵、父子关系、脏标记；重写 GltfLoader 填充真正的节点树。
9. **Light 组件 + LightManager**：支持方向光/点光/聚光，UBO/SSBO 上传，为后续阴影/延迟做入口。
10. **FrustumCuller**（Strategy 模式预留接口）：先做 sphere-frustum 的 CPU 剔除。

### P1.5 画面能力
11. **PBR 完整贴图支持**（Normal / MR / AO / Emissive）+ 对应着色器。
12. **方向光阴影 map**（shadow pass + depth compare sampler）。
13. **后处理管线**（最少：Tonemap + Gamma；进阶：Bloom、FXAA）。

### P2 进阶 / 工程化
14. 延迟渲染 / Clustered Forward+（多光源场景）。
15. 骨骼蒙皮动画（依赖节点树）。
16. 实例化 / 间接绘制（`vkCmdDrawIndexedIndirect`）。
17. GPU 时间戳 profiler，叠加到 GuiSystem。
18. Shader / 场景热重载。
19. 单元测试框架（GoogleTest）+ 小规模 fixture（无窗 Device、Buffer、Material）。
20. 配置文件（TOML/YAML）替代 `Config.h` 硬编码。

---

## 3. 设计模式落地建议

> 原则：**只在能解决具体痛点的地方**引入模式，不为模式而模式。

### 3.1 Handle + Pool（核心痛点：资源生命周期）

**当前**：`std::unique_ptr<Buffer>` 散落在 Material/Renderer/Mesh；use-after-free 无法检测；重复加载同一个贴图会重复上 GPU。

**目标**：
```cpp
// core/ResourceHandle.h
template <typename Tag>
struct Handle {
    uint32_t index = kInvalid;
    uint32_t generation = 0;
    bool valid() const { return index != kInvalid; }
};

template <typename T>
class ResourcePool {
    std::vector<std::optional<T>> slots_;
    std::vector<uint32_t> generations_;
    std::vector<uint32_t> freeList_;
public:
    Handle<T> insert(T value);
    T* get(Handle<T> h);              // 校验 generation
    void release(Handle<T> h);
};
```
**落地顺序**：`TexturePool` → `BufferPool` → `MeshPool` → `MaterialInstancePool`。先在 `render/` 内部落地，不破坏外部 API。

### 3.2 Template / Instance（核心痛点：Material）

**当前**：`Material` 同时持有 PipelineConfig（静态）与 MaterialParams（动态），每实例一个 descriptor pool。

**目标**：
```cpp
class MaterialTemplate {
    VkPipeline pipeline;             // 由 shader+state 决定，可共享
    VkPipelineLayout layout;
    VkDescriptorSetLayout descLayout;
};

class MaterialInstance {
    std::shared_ptr<MaterialTemplate> tmpl;
    std::array<Handle<Texture>, kMaxSlots> textures;
    MaterialParams params;           // push constants 或 per-instance UBO
    VkDescriptorSet descriptorSet;   // 由 DescriptorSetManager 分配
};
```
**配套**：`MaterialTemplateCache`（以 shader key 缓存）；`DescriptorSetManager` 按 layout 分池。

### 3.3 Composite（核心痛点：场景图）

**当前**：`Scene` = `vector<SceneObject*>`，`SceneObject` 直接拿 world matrix。

**目标**：`SceneNode` 树 + 懒更新世界矩阵；`Renderable` 作为可挂载组件；`GltfLoader` 直接填节点树。为后续动画/层级裁剪打地基。

### 3.4 Strategy（核心痛点：剔除 / 排序策略可替换）

```cpp
struct ICuller { virtual void cull(const SceneView&, std::vector<RenderCommand>&) = 0; };
struct FrustumCuller : ICuller { /* ... */ };
struct NoCuller : ICuller {};                      // 调试用

struct ISortStrategy { virtual void sort(std::vector<RenderCommand>&) = 0; };
struct MaterialFirstSort : ISortStrategy {};
struct DepthFrontToBackSort : ISortStrategy {};     // 透明物
```

### 3.5 Command（核心痛点：延迟提交、排序、可并行）

`RenderCommand = { meshHandle, materialInstanceHandle, worldMatrix, sortKey }`
Scene 只 **产出** command；RenderQueue 负责 **排序** 与 **编码**；未来可直接换成多线程二级 CommandBuffer 录制。

### 3.6 Builder（核心痛点：PipelineConfig 易错）

```cpp
auto cfg = PipelineConfigBuilder()
    .withShaders("shader/opaque.vert.spv", "shader/opaque.frag.spv")
    .withVertexLayout(Vertex::layout())
    .withCullMode(VK_CULL_MODE_BACK_BIT)
    .withDepthTest(true, VK_COMPARE_OP_LESS)
    .withSamples(msaa)
    .build(); // 内部做必要的合法性断言
```

### 3.7 Observer / Event Bus（核心痛点：resize、输入模式切换）

当前 resize 需要 Application 手动调用 FrameSync/Renderer/GuiSystem；输入模式切换在 Application 里 if/else。

建议一个轻量 `EventBus`（或 observer list），至少先支撑两个事件：`SwapChainResized`、`InputModeChanged`。

### 3.8 Factory + Registry（核心痛点：场景、pipeline、材质注册）

- `SceneFactory` 现状是硬编码路径 → 改为"注册到 map<string, Creator>"，供 GUI 列表与命令行选择。
- `PipelineRegistry`：以 key 为 `<vertLayoutId, passId, materialTemplateId>` 缓存 pipeline，避免热路径重建。

### 3.9 PIMPL（可选，按需）

Device / VulkanContext 的头文件暴露了大量 Vulkan 类型，外层模块都被迫引入 `vulkan.h`。长期可对 Device/SwapChain/Renderer 做 PIMPL，把 vulkan 头限制在 `.cpp`，减少编译时间与符号污染。**注意**：这是中后期优化，不建议现在就做。

---

## 4. 工程化缺口

| 领域 | 现状 | 建议 |
|------|------|------|
| 日志 | `std::cout` 散布 | spdlog + category（已列为 P0） |
| 异常 | `runtime_error` | `VulkanException{VkResult, where, what}` |
| 断言 | 无 | 自建 `VKL_ASSERT`（debug 下 break，release 下 log） |
| 测试 | 无 | 先上 GoogleTest，优先覆盖 Buffer/Image RAII、Handle/Pool、MaterialTemplate 缓存 |
| CI | 无 | GitHub Actions：CMake 构建 + shader 编译 + ctest |
| Profiler | 无 | VkQueryPool 时间戳 + ImGui 叠加（配合 GuiSystem） |
| 配置 | `Config.h` 硬编码 | TOML（tomlplusplus）或 JSON 读取，支持热重载 |
| Shader 编译 | CMake `glslc` 静态调用 | 允许运行时重编（debug 模式）+ 缓存 SPIR-V 哈希 |
| 文档 | 仅 doc/ 下零散设计稿 | 增补 `ARCHITECTURE.md`（本文件的简化版）+ `CONTRIBUTING.md` |

---

## 5. 分阶段路线图

> 每一阶段结束都应保持"能跑、能展示、能截图"。避免长时间分支不可运行。

### Phase 1 · 地基（建议 2~3 周）
- [ ] 引入 `spdlog`，替换所有 `std::cout`，建立 category
- [ ] `VulkanException` + 带上下文的 `VK_CHECK`
- [ ] `Handle<T>` + `ResourcePool<T>`，先落地 Texture / Buffer
- [ ] `MaterialTemplate` / `MaterialInstance` 拆分 + `DescriptorSetManager`
- [ ] `PipelineConfigBuilder`
- **验收**：现有 demo 场景画面、功能不变，`Renderer` 中对 Material/Texture 的直接管理被换成句柄；内存/描述符池数量下降；日志分类可读。

### Phase 2 · 渲染骨架（3~4 周）
- [ ] 抽出 `IRenderPass` 接口；Renderer 拆成 `ForwardPass` + `GuiPass`
- [ ] `RenderCommand` + `RenderQueue`（排序策略 Strategy）
- [ ] `SceneNode` 树；重写 `GltfLoader` 生成真正节点树
- [ ] `Light` 组件 + `LightManager` + UBO/SSBO 上传
- [ ] `FrustumCuller`（Strategy 默认实现）
- **验收**：多盏灯 + 节点层级 glTF 能正确渲染；加入 1000 cube 压力场景后剔除生效、draw call 显著下降。

### Phase 3 · 画面特性（3~4 周）
- [ ] 完整 PBR 贴图通道（Normal / MR / AO / Emissive）
- [ ] 方向光 Shadow Pass（CSM 作为可选 P2）
- [ ] 后处理 Pass：Tonemap + Gamma，进阶 Bloom
- [ ] EventBus：SwapChain resize / 输入模式
- **验收**：能展示一个"PBR + 阴影 + HDR tonemap"小场景，Gui 内可切光源、曝光、bloom 强度。

### Phase 4 · 进阶 & 工程化（按需）
- [ ] 延迟 / Clustered Forward+
- [ ] 蒙皮动画
- [ ] Indirect Draw / 实例化
- [ ] GPU Profiler（VkQueryPool）
- [ ] Shader / 场景热重载
- [ ] GoogleTest + CI

---

## 6. 风险与取舍

- **过度工程化风险**：RenderGraph、ECS、完整 FrameGraph 等在现阶段收益有限；本文刻意只推**轻量 Pass 抽象 + RenderQueue**，留出未来升级空间。
- **API 稳定性**：Phase 1 的 Handle 化会影响 Renderer/Material/Scene 的接口，建议**在一个功能分支内完成**并一次性切换，避免长期双轨。
- **教育性 vs 工程性**：本仓库带有学习性质，引入过多抽象可能降低可读性。建议在 `doc/arch_review/` 下为每个新增模式补一页"为什么要它"的简短说明，保留教学价值。
- **测试成本**：Vulkan 单元测试需要无窗设备/离屏渲染；先选**纯 CPU 侧**的测试目标（Pool、Handle、Builder、CameraMatrices、FrustumCuller 数学），避免前期陷入 GPU 测试基建。

---

## 7. 结论

目前仓库的**分层与 RAII 基础扎实**，但**渲染层仍停留在教程阶段**。最关键的三项重构是：

1. **把 Renderer 解成 Pass**（解除 God Object）；
2. **把 Material 解成 Template + Instance**（解除与 Renderer 的耦合、消灭 per-material pool）；
3. **把 Scene 升级成 Node 树 + RenderCommand 队列**（为灯光/阴影/动画/剔除铺路）。

这三件事做完，后续灯光、阴影、延迟、后处理、动画、剔除、实例化等功能都只是"在 Pass/Queue 上新增节点"，而不再是"改动整个 Renderer"。
