## 当前架构的核心矛盾

当前代码是「**教程代码的 RAII 改良版**」—— 每个类做到了资源自管理，但 **类与类之间的关系仍然是教程式的线性串联**：

```
App 手动创建一切 → Renderer 独占唯一 RenderPass → Material 绑死 Pipeline → 单场景单物体
```

要成为「有 GUI 的多功能渲染器」，核心矛盾是：

| 当前 | 目标 |
|------|------|
| 1 个 RenderPass，硬编码 3 个 attachment | 多 Pass（GBuffer / Lighting / ImGui / Shadow），每个 attachment 配置不同 |
| Material = Pipeline + Descriptors 绑死 | Pipeline 可运行时切换，Material 只管参数 |
| Renderer 既管帧同步又管 RenderPass 又管资源创建 | 职责分离：帧同步、Pass 编排、资源上传各自独立 |
| App 直接 `new` 所有资源 | 资源从配置/文件驱动，支持热加载 |
| 输入直接驱动相机 | 输入 → GUI 拦截 → 剩余事件传给应用 |

---

## 新架构设计

```
┌─────────────────────────────────────────────────────────┐
│                      Application                        │
│  持有 Engine，驱动主循环，定义场景内容                       │
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────┐
│                       Engine                            │
│  顶层容器：初始化顺序、主循环调度、模块生命周期               │
│  持有以下所有子系统的 unique_ptr                           │
└──┬────┬────┬────┬────┬────┬────┬───────────────────────┘
   │    │    │    │    │    │    │
   ▼    ▼    ▼    ▼    ▼    ▼    ▼
 Window Device Swap  Frame Graph  GUI  Resource
       Chain Sync  System      Manager
```

### 层级 0：平台层（Platform）

```cpp
// 抽象窗口接口 —— 隔离 GLFW
class IWindow {
public:
    virtual ~IWindow() = default;
    virtual bool        shouldClose() const = 0;
    virtual void        pollEvents() = 0;
    virtual VkExtent2D  framebufferSize() const = 0;
    virtual VkSurfaceKHR createSurface(VkInstance instance) const = 0;
    virtual std::vector<const char*> requiredExtensions() const = 0;
    virtual void*       nativeHandle() const = 0;  // ImGui 需要
};

class GlfwWindow : public IWindow { ... };  // 唯一接触 GLFW 的地方

// 输入事件用事件队列，不再轮询
struct InputEvent {
    enum Type { KeyPress, KeyRelease, MouseMove, MouseButton, Scroll };
    Type type;
    int key; float x, y;
};

class InputSystem {
public:
    void pollEvents(IWindow&);
    bool consume(InputEvent& out);  // GUI 优先消费，剩余给应用
    bool isKeyHeld(Key key) const;
    glm::vec2 mouseDelta() const;
};
```

**为什么**：当前 `VulkanContext`、`SwapChain` 都持有 `GLFWwindow*`，导致 Vulkan 层被 GLFW 穿透。`createSurface` 和 `requiredExtensions` 由窗口提供，Vulkan 层完全不知道 GLFW 存在。

---

### 层级 1：GPU 上下文（GPU Context）

```cpp
// VulkanContext 改为只管 Instance + Debug，不管 Surface
class Instance {
public:
    Instance(const AppInfo& info, bool enableValidation);
    VkInstance handle() const;
};

// Device 保持现状，但移除对 vulkan_utils.h 的依赖
class Device {
public:
    Device(Instance&, VkSurfaceKHR);
    VkDevice        logicalDevice() const;
    VmaAllocator    allocator() const;        // VMA 真正启用后
    VkQueue         graphicsQueue() const;
    VkQueue         presentQueue() const;
    VkQueue         transferQueue() const;    // 新增：异步传输
    // ...
};

// SwapChain 不再持有 GLFWwindow*
class SwapChain {
public:
    SwapChain(Device&, VkSurfaceKHR, VkExtent2D initialExtent);
    void recreate(VkExtent2D newExtent);     // 尺寸由外部传入
    // ...
};
```

---

### 层级 2：GPU 资源（Resources）— 最关键的拆分

```cpp
// Buffer / Image 改用 VMA（P0）
class Buffer {
    VmaAllocation allocation_;  // 替代 VkDeviceMemory
    // ...
};

class Image {
    VmaAllocation allocation_;
    // ...
};

// ★ 新增：Shader 模块管理
class ShaderModule {
public:
    ShaderModule(Device&, const std::string& spvPath);
    VkShaderModule handle() const;
    VkShaderStageFlagBits stage() const;  // 自动从文件名推断
};

// ★ 新增：资源管理器（缓存 + 引用计数）
class ResourceManager {
public:
    ResourceManager(Device&, FrameSync&);

    std::shared_ptr<Texture>      loadTexture(const std::string& path);
    std::shared_ptr<Mesh>         loadMesh(const std::string& path);
    std::shared_ptr<ShaderModule> loadShader(const std::string& path);

private:
    std::unordered_map<std::string, std::weak_ptr<Texture>>      texCache_;
    std::unordered_map<std::string, std::weak_ptr<Mesh>>         meshCache_;
    std::unordered_map<std::string, std::weak_ptr<ShaderModule>> shaderCache_;
};
```

---

### 层级 3：Pipeline 系统 — 解耦的核心

这是当前架构改动最大、收益最高的部分：

```cpp
// Pipeline 配置描述（数据驱动，可序列化）
struct PipelineConfig {
    std::string          vertShader;
    std::string          fragShader;
    VkPolygonMode        polygonMode   = VK_POLYGON_MODE_FILL;
    VkCullModeFlags      cullMode      = VK_CULL_MODE_BACK_BIT;
    bool                 depthTest     = true;
    bool                 depthWrite    = true;
    VkSampleCountFlagBits msaa         = VK_SAMPLE_COUNT_1_BIT;
    uint32_t             colorAttachmentCount = 1;  // MRT 时 > 1
    std::vector<VkFormat> colorFormats;
    VkFormat              depthFormat;
    // ... blend state, push constant ranges, descriptor layouts ...

    bool operator==(const PipelineConfig&) const;
    size_t hash() const;
};

// ★ Pipeline 缓存 —— 运行时按需创建，ImGui 切换时查缓存
class PipelineManager {
public:
    PipelineManager(Device&, ResourceManager&);

    // 返回已缓存或新建的 Pipeline
    VkPipeline getOrCreate(const PipelineConfig& config, VkRenderPass pass);

    void clearCache();  // 窗口 resize 时 RenderPass 重建，需清缓存

private:
    std::unordered_map<size_t, VkPipeline> cache_;
};
```

```cpp
// ★ Material 重定义 —— 只管"材质参数"，不管 Pipeline
class Material {
public:
    Material(Device&, DescriptorAllocator&);

    void setTexture(uint32_t binding, std::shared_ptr<Texture> tex);
    void setFloat(const std::string& name, float value);
    void setVec4(const std::string& name, glm::vec4 value);
    void setPipelineConfig(PipelineConfig config);  // 声明想用什么管线

    const PipelineConfig& pipelineConfig() const;

    void bindDescriptors(VkCommandBuffer cmd, VkPipelineLayout layout,
                         uint32_t frameIndex) const;

private:
    PipelineConfig          config_;
    std::shared_ptr<Texture> textures_[8];
    std::unique_ptr<Buffer>  paramBuffer_;  // 材质参数 UBO
    VkDescriptorSet          sets_[MAX_FRAMES_IN_FLIGHT];
};
```

**关键变化**：Material 不持有 Pipeline，只持有 `PipelineConfig` 描述。渲染时由 `PipelineManager` 根据描述查缓存拿到实际 Pipeline。ImGui 修改 `PipelineConfig` 的字段（wireframe、MSAA 等），下一帧自动生效。

---

### 层级 4：渲染框架（Render Graph）— 多 Pass 的核心

```cpp
// ★ RenderPass 描述（数据驱动）
struct AttachmentDesc {
    VkFormat            format;
    VkSampleCountFlagBits samples;
    VkAttachmentLoadOp  loadOp;
    VkImageLayout       initialLayout;
    VkImageLayout       finalLayout;
    bool                isDepth = false;
    bool                isResolve = false;
};

struct RenderPassDesc {
    std::vector<AttachmentDesc> attachments;
    // subpass 配置...
};

// ★ 渲染 Pass 抽象
class RenderPass {
public:
    RenderPass(Device&, const RenderPassDesc& desc);
    VkRenderPass handle() const;
    // ...
};

// ★ 帧同步独立出来（从 Renderer 剥离）
class FrameSync {
public:
    FrameSync(Device&, SwapChain&);

    struct FrameContext {
        VkCommandBuffer  cmd;
        uint32_t         frameIndex;
        uint32_t         imageIndex;
        VkCommandPool    commandPool;
    };

    FrameContext beginFrame();
    void         endFrame(FrameContext&);
    void         waitIdle();

    // 单次命令（用 Fence，不用 QueueWaitIdle）
    VkCommandBuffer beginTransfer();
    void            endTransfer(VkCommandBuffer);

private:
    // per-frame: fence + semaphores + command buffer
};

// ★ RenderTechnique —— 编排多个 Pass
class RenderTechnique {
public:
    virtual ~RenderTechnique() = default;
    virtual void setup(Device&, SwapChain&, PipelineManager&) = 0;
    virtual void render(const FrameSync::FrameContext& ctx,
                        Scene& scene, Camera& camera) = 0;
    virtual void onResize(VkExtent2D newExtent) = 0;
    virtual const char* name() const = 0;  // ImGui 显示用
};

class ForwardTechnique : public RenderTechnique {
    // 当前的逻辑：1 RenderPass (color+depth+resolve)
    void render(...) override {
        beginRenderPass → scene.render → endRenderPass
    }
};

class DeferredTechnique : public RenderTechnique {
    // GBuffer Pass → Lighting Pass
    void render(...) override {
        beginGBufferPass → scene.renderGeometry → endPass
        beginLightingPass → drawFullscreenQuad → endPass
    }
};
```

---

### 层级 5：GUI 系统

```cpp
class GuiSystem {
public:
    GuiSystem(Device&, IWindow&, FrameSync&);
    ~GuiSystem();

    void beginFrame();   // ImGui_ImplVulkan_NewFrame + NewFrame
    void endFrame(VkCommandBuffer cmd); // Render + RenderDrawData

    // 独立 RenderPass，最后画在 swapchain image 上
    VkRenderPass renderPass() const;

private:
    VkDescriptorPool imguiPool_;
    VkRenderPass     imguiRenderPass_;
    std::vector<VkFramebuffer> imguiFBs_;
};
```

**Input 协作**：

```cpp
// 帧循环中
inputSystem.pollEvents(window);
gui.beginFrame();

// ImGui 面板
if (ImGui::Begin("Renderer")) {
    // 切换渲染技术
    const char* techniques[] = {"Forward", "Deferred"};
    if (ImGui::Combo("Technique", &currentTech, techniques, 2)) {
        switchTechnique(currentTech);
    }
    // 切换 wireframe
    ImGui::Checkbox("Wireframe", &settings.wireframe);
    // 调相机参数
    ImGui::SliderFloat("FOV", &settings.fov, 30.0f, 120.0f);
}
ImGui::End();

// 如果 ImGui 没消费输入，传给相机
if (!ImGui::GetIO().WantCaptureMouse) {
    camera.rotate(inputSystem.mouseDelta() * sensitivity);
}
if (!ImGui::GetIO().WantCaptureKeyboard) {
    // WASD 移动
}
```

---

### 层级 6：应用层（Engine / App）

```cpp
class Engine {
public:
    Engine(const Config& config);
    ~Engine();

    void run();

    // 子系统访问
    Device&          device();
    ResourceManager& resources();
    PipelineManager& pipelines();
    GuiSystem&       gui();
    InputSystem&     input();
    Scene&           scene();

private:
    // 初始化顺序即声明顺序（RAII 析构逆序）
    Config                          config_;
    std::unique_ptr<GlfwWindow>     window_;
    std::unique_ptr<Instance>       instance_;
    VkSurfaceKHR                    surface_;
    std::unique_ptr<Device>         device_;
    std::unique_ptr<SwapChain>      swapChain_;
    std::unique_ptr<FrameSync>      frameSync_;
    std::unique_ptr<ResourceManager> resources_;
    std::unique_ptr<PipelineManager> pipelines_;
    std::unique_ptr<InputSystem>    input_;
    std::unique_ptr<GuiSystem>      gui_;

    // 可切换的渲染技术
    std::unique_ptr<RenderTechnique> technique_;

    Scene   scene_;
    Camera  camera_;
};
```

```cpp
// main.cpp — 简洁的应用入口
int main() {
    vkr::Config config;
    config.windowWidth = 1280;
    config.windowHeight = 720;
    config.enableValidation = true;

    vkr::Engine engine(config);

    // 加载资源
    auto mesh = engine.resources().loadMesh("models/viking_room.obj");
    auto tex  = engine.resources().loadTexture("textures/viking_room.png");
    auto mat  = std::make_shared<vkr::Material>(engine.device(), ...);
    mat->setTexture(0, tex);

    engine.scene().addObject({mesh, mat, glm::mat4(1.0f)});

    engine.run();
}
```

---

## 完整依赖图（新架构）

```
         main.cpp
            │
            ▼
        ┌─Engine─────────────────────────────────┐
        │                                         │
   ┌────┴────┐   ┌──────────┐   ┌──────────────┐ │
   │ IWindow  │   │ Instance │   │   Config     │ │
   │ (GLFW)  │   └────┬─────┘   └──────────────┘ │
   └────┬────┘        │                           │
        │        ┌────▼─────┐                     │
        │        │  Device   │◄── VMA             │
        │        └────┬─────┘                     │
        │             │                           │
   ┌────▼─────┐  ┌───▼──────┐  ┌──────────────┐  │
   │ SwapChain │  │FrameSync │  │ResourceManager│ │
   └──────────┘  └──────────┘  └──────┬───────┘  │
                                      │           │
                              ┌───────▼────────┐  │
                              │PipelineManager │  │
                              └───────┬────────┘  │
                                      │           │
              ┌───────────────────────▼────┐      │
              │    RenderTechnique         │      │
              │  ┌─Forward──────────────┐  │      │
              │  │ 1 RenderPass         │  │      │
              │  └──────────────────────┘  │      │
              │  ┌─Deferred─────────────┐  │      │
              │  │ GBuffer + Lighting   │  │      │
              │  └──────────────────────┘  │      │
              └────────────────────────────┘      │
                                                  │
              ┌──────────────┐  ┌──────────┐      │
              │  GuiSystem   │  │InputSystem│     │
              │  (ImGui)     │  └──────────┘      │
              └──────────────┘                    │
                                                  │
              ┌──────────┐  ┌────────┐            │
              │  Scene   │  │ Camera │            │
              └──────────┘  └────────┘            │
        └─────────────────────────────────────────┘

GLFW 边界线：只有 GlfwWindow 接触 GLFW
Vulkan 边界线：RenderTechnique 以上不直接调用 vkCmd*
```

---

## 与当前代码的对应关系 & 迁移路径

| 当前类 | 新架构去向 | 改动程度 |
|--------|-----------|---------|
| vulkan_utils.h | **删除** → 拆为 Config.h + VulkanTypes.h + Vertex.h | 重写 |
| `VulkanContext` | **拆分** → `Instance`（只管 VkInstance）+ Surface 创建移到 `IWindow` | 中等 |
| `Device` | **保持**，移除 vulkan_utils.h 依赖，VMA 真正启用 | 小改 |
| `SwapChain` | **改接口**，不持有 `GLFWwindow*`，尺寸由外部传入 | 小改 |
| `Buffer` / `Image` | **改内部**，用 VMA 替代 `vkAllocateMemory` | 中等 |
| `Pipeline` | **降级**为内部实现，对外暴露 `PipelineManager` + `PipelineConfig` | 重写 |
| `Renderer` | **拆分** → `FrameSync`（帧同步）+ `RenderTechnique`（Pass 编排） | 重写 |
| `Material` | **重定义**，不持有 Pipeline，只管参数和 Descriptor | 重写 |
| `Mesh` / `Texture` | **保持**，移入 ResourceManager 管理 | 小改 |
| `Scene` / `Camera` | **保持** | 不变 |
| `Window` / `InputManager` | **改接口** → `IWindow` + `InputSystem` | 中等 |
| `App` | **替换** → `Engine` 类 | 重写 |

---

## 推荐实施顺序

```
第一步：基础债务清理（不改架构）
  ├─ P0: VMA 启用
  ├─ P1: 拆分 vulkan_utils.h
  └─ P3: VK_CHECK 宏

第二步：解耦核心（架构改动开始）
  ├─ Renderer 拆分为 FrameSync + RenderPass 工厂
  ├─ Pipeline 与 Material 解耦 → PipelineManager + PipelineConfig
  └─ SwapChain / VulkanContext 去 GLFW

第三步：GUI 与运行时能力
  ├─ ImGui 集成（独立 RenderPass）
  ├─ InputSystem 事件队列 + ImGui 消费优先
  └─ RenderSettings 面板

第四步：多 Pass 框架
  ├─ RenderTechnique 抽象
  ├─ ForwardTechnique（当前逻辑迁入）
  └─ DeferredTechnique（新实现）

第五步：资源系统
  ├─ ResourceManager 缓存
  ├─ DescriptorAllocator 统一管理
  └─ 场景从文件加载
```

第一步可以马上开始，不影响现有功能。每一步结束都能编译运行验证。