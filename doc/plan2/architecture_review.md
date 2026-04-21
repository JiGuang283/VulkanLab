# 新架构方案评审与修订

## 1. 当前架构现状（第一步完成后）

### 1.1 已完成的清理工作

| 项目 | 状态 |
|------|------|
| `vulkan_utils.h` 消除 | ✅ 已删除，符号拆分至 `Config.h`、`VulkanTypes.h`、`Vertex.h`、`UniformData.h` |
| `VK_CHECK` 宏统一 | ✅ `VulkanCheck.h`，全项目替换 |
| VMA 启用 | ✅ `Buffer`/`Image` 已使用 `VmaAllocation`，`Device` 暴露 `allocator()` |
| GLFW 隔离 | ✅ `VulkanContext` 用 `SurfaceCreator` 回调，`SwapChain` 用 `ExtentProvider` 回调，core/ 和 render/ 零 GLFW 引用 |
| `Application` 类 | ✅ 替换旧 `HelloTriangleApplication`，RAII 析构无需手动 cleanup |
| 同步修复 | ✅ `renderFinished` 按 swapchain image index 索引 |

### 1.2 当前类关系图

```
Application
├── Window              (GLFW 封装)
├── InputManager        (GLFW 输入)
├── VulkanContext        (Instance + DebugMessenger + Surface)
├── Device              (Physical/Logical Device + VMA)
├── SwapChain           (交换链 + ImageViews)
├── Renderer            (帧同步 + RenderPass + Framebuffer + 命令缓冲 + UBO)
├── Texture             (stb_image 加载 → Image + Sampler)
├── Material            (DescriptorSetLayout + Pipeline + DescriptorSets)
├── Mesh                (顶点/索引缓冲，fromOBJ)
├── Scene               (vector<SceneObject>，遍历渲染)
└── Camera              (FPS 风格，view/proj 矩阵)
```

### 1.3 当前优势

1. **GLFW 完全隔离**：core/ 和 render/ 不依赖 GLFW，`SurfaceCreator`/`ExtentProvider` 回调模式已实现了 plan2 中 `IWindow` 想达到的核心目的
2. **VMA 已集成**：Buffer/Image 已经用 VMA，无需再做 P0 迁移
3. **RAII 析构链正确**：`unique_ptr` 按声明逆序销毁，无需手动 cleanup
4. **同步正确**：renderFinished 按 image index 索引，fence/semaphore 使用符合规范
5. **Push constant model matrix**：已有 per-object transform 支持
6. **Namespace 统一**：全部在 `vkr` 命名空间内

### 1.4 当前瓶颈

| 瓶颈 | 位置 | 影响 |
|------|------|------|
| Renderer 职责过重 | `Renderer.h` ~110 行，混合帧同步、RenderPass、Framebuffer、UBO、Color/Depth 资源 | 添加新 Pass 必须侵入 Renderer |
| Pipeline 硬编码 | `Pipeline.cpp` vertex layout、blend state、MSAA 全固定 | 不同材质无法用不同管线配置 |
| Material 绑死 Pipeline | `Material` 构造时创建 `Pipeline`，descriptor layout 也固定为 1 UBO + 1 sampler | 无法支持多纹理材质 |
| Mesh 只支持 OBJ | `Mesh::fromOBJ()` 固定 `Vertex{pos,color,texCoord}` | 无法加载 glTF 等其他格式 |
| Scene 无层级 | 扁平 `vector<SceneObject>` | 无法表达节点树 |
| 无资源缓存 | 相同贴图/模型会重复加载 | 效率差，多物体共享困难 |

---

## 2. plan2 方案评审

### 2.1 已经由第一步解决的设计项

plan2 中以下设计**不再需要**，因为当前代码已经实现（或更好的替代方案）：

| plan2 设计 | 当前现状 | 结论 |
|-----------|---------|------|
| `IWindow` 抽象接口 | `SurfaceCreator` + `ExtentProvider` 回调已隔离 GLFW | **不需要**。回调式耦合比接口继承更轻量，且当前只有 GLFW 后端，抽象接口是过早泛化 |
| `Instance` 类拆分 | `VulkanContext` 管 Instance + Debug + Surface | **不需要拆**。Instance 和 Surface 总是成对创建/销毁，拆分增加复杂度无实际收益 |
| `SwapChain` 去 GLFW | 已完成，用 `ExtentProvider` 回调 | **已完成** |
| VMA 启用（P0） | Buffer/Image 已用 VMA | **已完成** |
| `VK_CHECK` 宏 | 已全面应用 | **已完成** |
| 拆分 `vulkan_utils.h` | 已完全消除 | **已完成** |

### 2.2 过度设计的部分

| plan2 设计 | 问题 |
|-----------|------|
| `InputEvent` 事件队列 + `InputSystem::consume()` | 当前只有 FPS 相机和 ESC 退出，事件队列系统在集成 ImGui 之前完全不需要。ImGui 本身通过 `ImGui::GetIO().WantCaptureMouse` 判断冲突，不需要 consume 模式 |
| `RenderTechnique` 虚基类 + `ForwardTechnique` / `DeferredTechnique` | 过早抽象。当前只有 Forward 一种路径，Deferred 是遥远的目标。抽象接口可以在第二种技术真正需要时再提取 |
| `PipelineConfig` 数据驱动 + hash 缓存 | 全功能 `PipelineConfig` 结构体（含序列化、hash）对当前规模是重量级过度工程。更实际的做法是让 `Pipeline` 构造函数接受更多参数 |
| `DescriptorAllocator` 所有材质公用 | 当前材质类型单一（1 UBO + 1 sampler），全局 descriptor allocator 在只有一种 layout 时收益为零 |
| `ResourceManager` 缓存 + 引用计数 | `shared_ptr` 已经提供引用计数。缓存层在资源种类极少时（1 mesh + 1 texture）不值得 |
| Engine 作为顶层容器 | 当前 `Application` 已经完成了 Engine 的职责，改名不带来架构价值 |

### 2.3 值得保留的核心思路

| plan2 设计 | 价值 | 修订方式 |
|-----------|------|---------|
| **Renderer 拆分** | 帧同步 vs RenderPass/Framebuffer 职责分离 | 保留，但简化为 FrameSync 提取（不做 RenderPass 工厂） |
| **Pipeline 参数化** | 不同材质需要不同管线配置 | 保留，但用结构体参数而非 hash 缓存 |
| **Material 与 Pipeline 解耦** | Material 只管参数，Pipeline 可变 | 保留核心思路 |
| **ImGui 集成** | 调试/可视化必需 | 保留，简化为 Renderer 添加 ImGui Pass |
| **多格式模型加载** | glTF 是刚需 | 保留，先阶段化加载 |

---

## 3. 修订后的架构方案

### 3.1 设计原则

1. **按需抽象**：只在第二个使用者出现时才提取接口/基类
2. **增量演进**：每一步都能编译运行，不做大拆大建
3. **学习导向**：保持代码可读，避免层次过深

### 3.2 目标架构（修订版）

```
Application                          // 不改名，无需 Engine 壳
├── Window                           // 保持现状
├── InputManager                     // 保持现状
├── VulkanContext                     // 保持现状
├── Device                           // 保持现状
├── SwapChain                        // 保持现状
│
├── FrameSync                        // 从 Renderer 拆出：帧同步 + 命令缓冲
│   ├── per-frame: fence, semaphore(imageAvailable), commandBuffer
│   └── per-image: semaphore(renderFinished)
│
├── Renderer                         // 瘦身：只管 RenderPass + Framebuffer + Color/Depth
│   ├── RenderPass (color+depth+resolve)
│   ├── Framebuffers
│   ├── Color/Depth Image
│   ├── Viewport/Scissor
│   └── recreateSwapChain()
│
├── Pipeline                         // 参数化：接受 PipelineConfig 结构体
│   └── PipelineConfig { vertex layout, shaders, blend, cull, depth, msaa }
│
├── Material                         // 不持有 Pipeline，持有 PipelineConfig + Descriptors
│   ├── PipelineConfig               // 声明需要什么管线
│   ├── textures[]                   // 支持多纹理
│   └── descriptorSets[]
│
├── Mesh                             // 不绑定 Vertex 结构体
│   ├── fromOBJ()                    // 保留
│   └── fromGLTF()                   // 新增
│
├── Scene                            // 增加节点树（可选）
├── Camera                           // 保持现状
└── [GuiSystem]                      // ImGui 集成（独立步骤）
```

### 3.3 各模块详细设计

#### A. FrameSync（从 Renderer 剥离）

```cpp
// src/core/FrameSync.h
class FrameSync {
public:
    FrameSync(Device& device, SwapChain& swapChain);
    ~FrameSync();

    struct FrameContext {
        VkCommandBuffer cmd;
        uint32_t frameIndex;    // 0..MAX_FRAMES_IN_FLIGHT-1
        uint32_t imageIndex;    // swapchain image index
    };

    // 返回 nullopt 表示跳帧（窗口最小化或需要重建交换链）
    std::optional<FrameContext> beginFrame();
    void endFrame(const FrameContext& ctx);

    void notifyResize() { framebufferResized_ = true; }
    bool needsSwapChainRecreation() const;         // Renderer 查询并执行重建
    void onSwapChainRecreated();                    // 重建 renderFinished 信号量

    uint32_t maxFramesInFlight() const { return MAX_FRAMES_IN_FLIGHT; }

private:
    struct PerFrame {
        VkCommandBuffer cmd;
        VkSemaphore     imageAvailable;
        VkFence         inFlight;
    };

    Device*    device_;
    SwapChain* swapChain_;
    VkCommandPool commandPool_;
    std::array<PerFrame, MAX_FRAMES_IN_FLIGHT> frames_;
    std::vector<VkSemaphore> renderFinished_; // per swapchain image
    uint32_t currentFrame_ = 0;
    bool framebufferResized_ = false;
};
```

**为什么这样拆**：
- FrameSync 管的是"何时渲染"——fence 等待、acquire、submit、present
- Renderer 管的是"渲染什么"——RenderPass、Framebuffer、Attachment

拆分后 Renderer 不再接触信号量和 fence，只被传入 `VkCommandBuffer` 和 `imageIndex`。

#### B. PipelineConfig + Pipeline 参数化

```cpp
// src/core/PipelineConfig.h
struct VertexLayout {
    VkVertexInputBindingDescription              binding;
    std::vector<VkVertexInputAttributeDescription> attributes;
};

struct PipelineConfig {
    std::string         vertShaderPath;
    std::string         fragShaderPath;
    VertexLayout        vertexLayout;          // 不再硬编码 Vertex::get*
    VkPolygonMode       polygonMode  = VK_POLYGON_MODE_FILL;
    VkCullModeFlags     cullMode     = VK_CULL_MODE_BACK_BIT;
    VkFrontFace         frontFace    = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool                depthTest    = true;
    bool                depthWrite   = true;
    VkCompareOp         depthCompare = VK_COMPARE_OP_LESS;
    bool                blendEnable  = false;
    VkSampleCountFlagBits msaa       = VK_SAMPLE_COUNT_1_BIT;

    // PushConstant / DescriptorSetLayout 由外部传入
    std::vector<VkPushConstantRange>    pushConstants;
    std::vector<VkDescriptorSetLayout>  descriptorLayouts;
};

// Pipeline 构造改为接受 PipelineConfig
class Pipeline {
public:
    Pipeline(Device& device, VkRenderPass renderPass,
             const PipelineConfig& config);
    // ...
};
```

**不做 PipelineManager/hash 缓存**：当前至多 2-3 种管线（opaque、alpha-blend、wireframe），手动管理即可。等管线种类超过 10 种再考虑缓存。

#### C. Material 解耦

```cpp
// 现在：Material 构造时硬编码 layout 并创建 Pipeline
// 目标：Material 只管参数和 descriptor，Pipeline 由外部（或延迟）创建

class Material {
public:
    // 构造不再创建 Pipeline，只设置参数
    Material(Device& device, uint32_t maxFramesInFlight);

    void setBaseColorTexture(std::shared_ptr<Texture> tex);
    void setBaseColorFactor(glm::vec4 factor);
    void setPipelineConfig(PipelineConfig config);

    // 绑定时需要已构建的 Pipeline
    void bind(VkCommandBuffer cmd, uint32_t frameIndex,
              VkPipeline pipeline, VkPipelineLayout layout) const;

    // 惰性构建 descriptor（首次 bind 或 texture 变更时）
    void updateDescriptors(Renderer& renderer);

    const PipelineConfig& pipelineConfig() const;

private:
    PipelineConfig config_;
    std::shared_ptr<Texture> baseColorTex_;
    glm::vec4 baseColorFactor_{1.0f};
    VkDescriptorSetLayout layout_;
    VkDescriptorPool pool_;
    std::vector<VkDescriptorSet> sets_;
    bool dirty_ = true;
};
```

**渲染流程变化**：

```cpp
// 现在 Scene::render():
obj.material->bind(cmd, frameIndex);             // 内部绑 pipeline + descriptor
obj.mesh->bind(cmd);
obj.mesh->draw(cmd);

// 目标 Scene::render():
VkPipeline pipeline = pipelineCache[obj.material->pipelineConfig()]; // 或直接存在 SceneObject 里
vkCmdBindPipeline(cmd, GRAPHICS, pipeline);
obj.material->bind(cmd, frameIndex, pipeline, layout);
obj.mesh->bind(cmd);
obj.mesh->draw(cmd);
```

#### D. Mesh 泛化 + glTF 支持

```cpp
// Mesh 构造函数不变（接受 raw data），添加新的工厂方法
class Mesh {
public:
    // 已有
    Mesh(Device& device, FrameSync& sync, const void* vertexData,
         VkDeviceSize vertexSize, const uint32_t* indexData, uint32_t indexCount);
    static std::unique_ptr<Mesh> fromOBJ(Device&, FrameSync&, const std::string& path);

    // 新增
    static std::unique_ptr<Mesh> fromVertices(Device&, FrameSync&,
        const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);

    void bind(VkCommandBuffer cmd) const;
    void draw(VkCommandBuffer cmd) const;

private:
    std::unique_ptr<Buffer> vertexBuffer_;
    std::unique_ptr<Buffer> indexBuffer_;
    uint32_t indexCount_;
};

// glTF 加载独立为 GltfLoader（不进 Mesh 类）
// src/render/GltfLoader.h
struct GltfLoadResult {
    struct Primitive {
        std::unique_ptr<Mesh>    mesh;
        int                      materialIndex; // -1 = 默认材质
    };
    struct MaterialData {
        std::string baseColorTexturePath; // 空 = 无贴图
        glm::vec4   baseColorFactor{1.0f};
        bool        doubleSided = false;
    };
    std::vector<Primitive>     primitives;
    std::vector<MaterialData>  materials;
    // 节点层级（可选，阶段 2 再做）
};

class GltfLoader {
public:
    static GltfLoadResult load(Device& device, FrameSync& sync,
                               const std::string& path);
};
```

#### E. ImGui 集成

```cpp
// src/gui/GuiSystem.h
class GuiSystem {
public:
    GuiSystem(Device& device, Window& window, VkRenderPass renderPass,
              uint32_t imageCount);
    ~GuiSystem();

    void beginFrame();
    void render(VkCommandBuffer cmd); // 在主 RenderPass 内部或单独 subpass
    void onResize();

private:
    VkDescriptorPool imguiPool_;
};
```

**关键决策**：ImGui 渲染在主 RenderPass 的**最后一个 subpass**（或简单地在 `endRenderPass` 之前画）。不单独创建 RenderPass——对于 Forward 渲染器，共用 RenderPass 更简单且足够。等到需要 post-processing 时再分离。

---

## 4. 实施顺序（修订版）

### 第二步：FrameSync 提取

**目标**：将帧同步逻辑从 Renderer 分离，Renderer 瘦身。

```
改动文件：
  新增：src/core/FrameSync.h, src/core/FrameSync.cpp
  修改：Renderer.h/cpp（移除帧同步代码），Application.cpp（持有 FrameSync）
  
外部接口变化：
  之前：cmd = renderer->beginFrame();  renderer->endFrame();
  之后：ctx = frameSync->beginFrame(); ... frameSync->endFrame(ctx);
        renderer->beginRenderPass(ctx.cmd, ctx.imageIndex);
  
验证：编译通过 + 运行效果不变 + validation layer 无报错
```

### 第三步：Pipeline 参数化

**目标**：Pipeline 构造参数化，为不同材质使用不同管线配置做准备。

```
改动文件：
  新增：src/core/PipelineConfig.h
  修改：Pipeline.h/cpp（构造函数改为接受 PipelineConfig）
  修改：Material.cpp（构造 PipelineConfig 传给 Pipeline）
  
验证：编译通过 + 运行效果不变
      手动测试：改 PipelineConfig.cullMode 看效果
```

### 第四步：glTF 加载

**目标**：支持加载简单的 glTF/glb 模型（带 baseColorTexture）。

```
改动文件：
  新增：external/tinygltf/（header-only 库）
  新增：src/render/GltfLoader.h, src/render/GltfLoader.cpp
  修改：Application.cpp（根据文件扩展名选择 OBJ 或 glTF）
  修改：Config.h（modelPath 改为 .glb 路径）
  
预备：下载一个简单的 glb 测试模型（如 BoxTextured.glb）
验证：加载 glb 文件渲染正确
```

### 第五步：Material 解耦

**目标**：Material 不持有 Pipeline，支持多纹理，descriptor layout 可变。

```
改动文件：
  修改：Material.h/cpp（移除 Pipeline 持有，改为接受外部 Pipeline）
  修改：Scene.cpp（渲染时查找 Pipeline）
  可能新增：简单 Pipeline 查找表（map<PipelineConfig hash, Pipeline>）

验证：编译通过 + 多材质渲染正确
```

### 第六步：ImGui 集成

**目标**：基础 ImGui 集成，显示 FPS 和基本调试信息。

```
改动文件：
  新增：external/imgui/（库文件）
  新增：src/gui/GuiSystem.h, src/gui/GuiSystem.cpp
  修改：Application.cpp（初始化 GuiSystem，帧循环中调用）
  修改：InputManager 或 Application（ImGui 输入优先消费）

验证：窗口中显示 ImGui 面板 + 输入不冲突
```

### 可选后续步骤

- **资源缓存**：当实际出现重复加载时引入 `ResourceManager`
- **RenderTechnique 抽象**：当第二种渲染路径（如 Deferred）真正需要时提取
- **InputEvent 队列**：当输入路由逻辑复杂到 if/else 无法维护时重构
- **PipelineManager hash 缓存**：当管线种类超过手动管理能力时引入

---

## 5. 与 plan2 的差异总结

| 领域 | plan2 原方案 | 修订版 | 理由 |
|------|-------------|-------|------|
| 窗口抽象 | `IWindow` 虚接口 | 保持 `Window` + 回调 | 只有 GLFW 后端，回调已隔离 |
| VulkanContext | 拆为 `Instance` | 保持 `VulkanContext` | Instance+Surface 生命周期绑定 |
| 帧同步 | `FrameSync` | ✅ **保留** | 这是最有价值的拆分 |
| RenderPass | `RenderPass` 工厂 + `RenderPassDesc` | 保持 Renderer 内建 | 只有一种 Forward Pass |
| 渲染技术 | `RenderTechnique` 虚基类 | 不做 | 只有 Forward，等 Deferred 时再提取 |
| Pipeline | `PipelineManager` + hash 缓存 | `PipelineConfig` 结构体参数化 | 管线数量极少，手动管理 |
| Material | 全重写，不持有 Pipeline | ✅ **保留思路**，渐进重构 | 分步做，先参数化后解耦 |
| 输入系统 | `InputEvent` 队列 + consume | 保持 `InputManager` | ImGui 有自己的输入方案 |
| 资源管理 | `ResourceManager` 全局缓存 | 按需引入 | 当前资源太少不需要 |
| GUI | `GuiSystem` 独立 RenderPass | `GuiSystem` 共用主 RenderPass | Forward 下不需要单独 Pass |
| 顶层容器 | `Engine` 类 | 保持 `Application` | 改名无架构价值 |
| 实施步骤 | 6 层从底向上 | 5 步从最有价值的拆分开始 | 增量演进，每步可验证 |

**核心理念差异**：plan2 是"目标架构一步到位"的思路，修订版是"按需演进"的思路。每一步改动都应有明确的功能驱动（而非架构美学驱动）。
