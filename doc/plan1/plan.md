# Vulkan 渲染器重构计划

## 一、当前架构分析

### 1.1 现状概览

当前代码忠实地跟随 vulkan-tutorial.com 教程，完成了以下功能：
- Vulkan 实例与验证层
- 物理/逻辑设备选择
- 交换链与图像视图
- 图形管线（含深度测试、MSAA）
- RenderPass / Framebuffer
- 命令池与命令缓冲
- 顶点/索引缓冲（staging buffer 方式）
- Uniform Buffer + Descriptor Sets
- 纹理贴图（含 Mipmap 生成）
- OBJ 模型加载
- 帧同步（Semaphore + Fence，双缓冲）

### 1.2 主要问题

| 问题 | 说明 |
|------|------|
| **God Object** | `HelloTriangleApplication` 一个类包含 30+ 成员变量、40+ 方法，承担所有职责 |
| **零抽象** | 直接调用原始 Vulkan API，没有任何 RAII 封装，cleanup 需手动逆序销毁 |
| **硬编码** | 窗口大小、模型路径、纹理路径、shader 路径均为常量硬编码 |
| **单管线/单材质** | 只有一个 Pipeline、一个 DescriptorSetLayout，无法支持多材质/多 Pass |
| **无相机系统** | MVP 矩阵在 `updateUniformBuffer` 中硬编码 |
| **无场景管理** | 只加载单个模型，无 Scene Graph / ECS |
| **无资源管理** | Buffer、Image、Sampler 等无统一的生命周期管理 |
| **文件拆分不彻底** | 虽然按功能拆分了 .cpp，但全都是同一个类的方法，耦合度极高 |

---

## 二、重构目标

> 将教程代码逐步演进为一个 **可拓展的前向渲染器（Forward Renderer）**，支持多物体、多材质、多 Pass，并为后续 Deferred Rendering / PBR 等高级特性留出接口。

### 核心原则
1. **渐进式重构** — 每个阶段可独立编译运行，不做大爆炸重写
2. **RAII 优先** — 所有 Vulkan 资源用 RAII 包装，消灭手动 cleanup
3. **单一职责** — 每个类只做一件事
4. **数据驱动** — 管线配置、材质参数、场景描述均可从外部数据加载

---

## 三、目标目录结构

```
src/
├── core/                       # Vulkan 基础设施（与渲染逻辑无关）
│   ├── VulkanContext.h/cpp     # Instance + DebugMessenger + Surface
│   ├── Device.h/cpp            # PhysicalDevice + LogicalDevice + Queues
│   ├── SwapChain.h/cpp         # SwapChain + ImageViews + Recreate
│   ├── CommandManager.h/cpp    # CommandPool + 单次命令辅助
│   ├── Buffer.h/cpp            # VkBuffer RAII 封装（Vertex/Index/Uniform/Staging）
│   ├── Image.h/cpp             # VkImage + VkImageView RAII 封装
│   ├── Sampler.h/cpp           # VkSampler RAII 封装
│   ├── DescriptorManager.h/cpp # DescriptorPool + DescriptorSetLayout + DescriptorSet 管理
│   ├── Pipeline.h/cpp          # Pipeline + PipelineLayout RAII（支持配置化创建）
│   ├── RenderPass.h/cpp        # RenderPass RAII
│   ├── Synchronization.h/cpp   # Semaphore / Fence 管理
│   └── VulkanTypes.h           # 公共类型定义（QueueFamilyIndices 等）
│
├── rendering/                  # 渲染层
│   ├── Renderer.h/cpp          # 渲染器主类（帧循环、命令录制调度）
│   ├── RenderFrame.h/cpp       # 单帧资源（CommandBuffer + 同步对象 + UBO）
│   ├── Material.h/cpp          # 材质（Pipeline + DescriptorSet + 参数）
│   ├── Mesh.h/cpp              # 网格（VertexBuffer + IndexBuffer）
│   ├── Texture.h/cpp           # 纹理资源管理（加载 + Mipmap）
│   └── UniformData.h/cpp       # Uniform 数据定义与更新
│
├── scene/                      # 场景层
│   ├── Scene.h/cpp             # 场景容器
│   ├── SceneObject.h/cpp       # 场景物体（Transform + Mesh + Material 引用）
│   ├── Camera.h/cpp            # 相机（透视/正交、控制器）
│   └── Light.h/cpp             # 光源（方向光、点光源、聚光灯）
│
├── resource/                   # 资源管理
│   ├── ResourceManager.h/cpp   # 统一资源缓存（纹理、模型、Shader）
│   ├── ModelLoader.h/cpp       # OBJ/glTF 模型加载
│   └── ShaderManager.h/cpp     # Shader 编译/缓存
│
├── window/                     # 窗口抽象
│   ├── Window.h/cpp            # GLFW 窗口封装 + 输入回调
│   └── InputManager.h/cpp      # 键鼠输入状态
│
├── app/                        # 应用层
│   ├── Application.h/cpp       # 应用入口（初始化、主循环）
│   └── Config.h                # 全局配置（可从文件加载）
│
└── main.cpp
```

---

## 四、分阶段实施计划

### 阶段 0：准备工作（基础设施）
> 不改动现有功能，只做编译和工具链准备。

- [ ] 引入内存分配器 [VulkanMemoryAllocator (VMA)](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)，后续所有 Buffer/Image 分配统一使用 VMA
- [ ] CMakeLists.txt 调整为按子目录组织源文件（`aux_source_directory` 或子 CMakeLists）
- [ ] 确保 Debug/Release 均可正常编译运行

---

### 阶段 1：RAII 封装 Vulkan 核心资源
> 目标：消灭 cleanup() 中的手动销毁，每个 Vulkan 对象拥有独立的 RAII 包装类。

#### 1.1 VulkanContext（Instance + Debug + Surface）
```cpp
class VulkanContext {
public:
    VulkanContext(GLFWwindow* window);  // 创建 instance, debugMessenger, surface
    ~VulkanContext();                    // 自动销毁

    VkInstance       instance()  const;
    VkSurfaceKHR     surface()   const;
};
```
- 将 `createInstance()`、`setupDebugMessenger()`、`createSurface()` 移入
- 验证层逻辑保留在内部

#### 1.2 Device（PhysicalDevice + LogicalDevice）
```cpp
class Device {
public:
    Device(VulkanContext& ctx);
    ~Device();

    VkDevice              logicalDevice()   const;
    VkPhysicalDevice      physicalDevice()  const;
    VkQueue               graphicsQueue()   const;
    VkQueue               presentQueue()    const;
    QueueFamilyIndices    queueFamilies()   const;
    VkSampleCountFlagBits maxMsaaSamples()  const;

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props) const;
};
```
- 将 `pickPhysicalDevice()`、`createLogicalDevice()` 等移入
- `findMemoryType()` 作为 Device 的公共方法

#### 1.3 Buffer / Image RAII
```cpp
class Buffer {
public:
    Buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage,
           VkMemoryPropertyFlags memProps);
    ~Buffer();
    
    void upload(const void* data, VkDeviceSize size);  // staging + copy
    VkBuffer handle() const;
};
```
```cpp
class Image {
public:
    Image(Device& device, uint32_t w, uint32_t h, uint32_t mipLevels,
          VkSampleCountFlagBits samples, VkFormat format, VkImageUsageFlags usage);
    ~Image();

    void transitionLayout(VkImageLayout oldLayout, VkImageLayout newLayout);
    VkImage     handle()    const;
    VkImageView imageView() const;
};
```

#### 1.4 SwapChain RAII
```cpp
class SwapChain {
public:
    SwapChain(Device& device, VkSurfaceKHR surface, GLFWwindow* window);
    ~SwapChain();

    void recreate();
    VkSwapchainKHR       handle()       const;
    VkFormat             imageFormat()   const;
    VkExtent2D           extent()        const;
    const std::vector<VkImageView>& imageViews() const;
};
```

#### 1.5 完成后验收
- `cleanup()` 函数体应为空或仅保留 `glfwTerminate()`
- 所有资源销毁由析构函数自动完成
- 运行结果与重构前完全一致

---

### 阶段 2：渲染流程抽象
> 目标：将帧循环和命令录制从应用层分离。

#### 2.1 Renderer 类
```cpp
class Renderer {
public:
    Renderer(Device& device, SwapChain& swapChain);
    ~Renderer();

    // 每帧调用
    VkCommandBuffer beginFrame();    // acquire image, begin command buffer
    void endFrame();                 // end command buffer, submit, present
    
    bool isFrameInProgress() const;
    VkRenderPass    renderPass()   const;
    uint32_t        frameIndex()   const;
    
private:
    void createRenderPass();
    void createFramebuffers();
    void createSyncObjects();
    void createDepthResources();
    void createColorResources();  // MSAA
    
    // per-frame resources
    std::vector<RenderFrame> frames_;
    uint32_t currentFrame_ = 0;
};
```

#### 2.2 RenderFrame
```cpp
struct RenderFrame {
    VkCommandBuffer commandBuffer;
    VkSemaphore     imageAvailable;
    VkSemaphore     renderFinished;
    VkFence         inFlight;
    Buffer          uniformBuffer;        // per-frame UBO
    VkDescriptorSet descriptorSet;
};
```

#### 2.3 从 Application 中剥离渲染逻辑
- `drawFrame()` → `Renderer::beginFrame()` / `endFrame()`
- `recordCommandBuffer()` → 由外部（Application 或 Scene）驱动
- `recreateSwapChain()` → `SwapChain::recreate()` + `Renderer` 重建帧缓冲

---

### 阶段 3：资源与场景管理
> 目标：支持多物体、多纹理。

#### 3.1 Mesh 类
```cpp
class Mesh {
public:
    static Mesh fromOBJ(Device& device, const std::string& path);

    void bind(VkCommandBuffer cmd) const;
    void draw(VkCommandBuffer cmd) const;

private:
    Buffer vertexBuffer_;
    Buffer indexBuffer_;
    uint32_t indexCount_;
};
```

#### 3.2 Texture 类
```cpp
class Texture {
public:
    Texture(Device& device, const std::string& path);  // 加载 + mipmap
    ~Texture();

    VkImageView  imageView() const;
    VkSampler    sampler()   const;
};
```

#### 3.3 Material 类
```cpp
class Material {
public:
    Material(Device& device, VkRenderPass renderPass,
             const std::string& vertShader, const std::string& fragShader);

    void bind(VkCommandBuffer cmd, uint32_t frameIndex) const;
    void setTexture(uint32_t binding, const Texture& texture);
    void updateUniforms(uint32_t frameIndex, const void* data, size_t size);

private:
    Pipeline           pipeline_;
    VkDescriptorSetLayout setLayout_;
    std::vector<VkDescriptorSet> descriptorSets_;
};
```

#### 3.4 Scene 与 SceneObject
```cpp
struct SceneObject {
    std::shared_ptr<Mesh>     mesh;
    std::shared_ptr<Material> material;
    glm::mat4                 transform;
};

class Scene {
public:
    void addObject(SceneObject obj);
    void render(VkCommandBuffer cmd, uint32_t frameIndex, const Camera& camera);
    
private:
    std::vector<SceneObject> objects_;
};
```

#### 3.5 Camera
```cpp
class Camera {
public:
    void setPosition(glm::vec3 pos);
    void lookAt(glm::vec3 target);
    void setPerspective(float fov, float aspect, float near, float far);

    glm::mat4 viewMatrix()       const;
    glm::mat4 projectionMatrix() const;
    
    void processKeyboard(/* ... */);
    void processMouse(/* ... */);
};
```

---

### 阶段 4：窗口与输入解耦
> 目标：将 GLFW 依赖隔离。

#### 4.1 Window 类
```cpp
class Window {
public:
    Window(uint32_t width, uint32_t height, const std::string& title);
    ~Window();

    bool shouldClose() const;
    void pollEvents();
    VkExtent2D extent() const;
    GLFWwindow* handle() const;

    // 注册回调
    void setResizeCallback(std::function<void(int, int)> cb);
    void setKeyCallback(std::function<void(int, int, int, int)> cb);
};
```

#### 4.2 InputManager
```cpp
class InputManager {
public:
    bool isKeyDown(int key) const;
    glm::vec2 mouseDelta() const;
    void update();  // 每帧调用，重置 delta
};
```

---

### 阶段 5：高级渲染特性（未来拓展）
> 以下为长期规划，在前 4 个阶段完成后按需实现。

| 特性 | 说明 |
|------|------|
| **PBR 材质** | Metallic-Roughness 工作流，替换当前的纯纹理采样 |
| **多光源** | 支持方向光/点光/聚光灯，UBO 或 SSBO 传递光源数据 |
| **Shadow Map** | 额外 RenderPass 渲染深度图，PCF 软阴影 |
| **Deferred Rendering** | G-Buffer Pass + Lighting Pass，需要多个 Color Attachment |
| **天空盒 / IBL** | Cubemap 加载，环境光照 |
| **glTF 加载** | 替代 OBJ，支持 PBR 材质、骨骼动画 |
| **ImGui 集成** | 叠加 UI 层，调试参数实时调节 |
| **Compute Shader** | 粒子系统、后处理（Bloom、Tone Mapping） |
| **多线程命令录制** | Secondary Command Buffer + 线程池 |

---

## 五、重构优先级与依赖关系

```
阶段 0 (VMA + 构建)
  │
  ▼
阶段 1 (RAII 封装)
  │
  ├──► 1.1 VulkanContext
  ├──► 1.2 Device
  ├──► 1.3 Buffer / Image
  └──► 1.4 SwapChain
  │
  ▼
阶段 2 (渲染抽象)
  │
  ├──► 2.1 Renderer
  ├──► 2.2 RenderFrame
  └──► 2.3 Application 瘦身
  │
  ▼
阶段 3 (资源 & 场景)      阶段 4 (窗口 & 输入)
  │                         │
  ├──► 3.1 Mesh             ├──► 4.1 Window
  ├──► 3.2 Texture          └──► 4.2 InputManager
  ├──► 3.3 Material
  ├──► 3.4 Scene
  └──► 3.5 Camera
  │
  ▼
阶段 5 (高级渲染 - 按需)
```

**阶段 3 与阶段 4 可并行推进。**

---

## 六、每个阶段的验收标准

| 阶段 | 验收标准 |
|------|----------|
| 0 | VMA 集成编译通过，现有功能不变 |
| 1 | `cleanup()` 为空，所有资源自动销毁，渲染结果不变 |
| 2 | `Application` 类不再直接持有任何 VkCommandBuffer/Fence/Semaphore，帧循环由 Renderer 驱动 |
| 3 | 可在场景中放置多个不同模型+不同纹理的物体，相机可用键鼠控制 |
| 4 | GLFW 仅在 `Window` 类中被引用，其余代码零 GLFW 依赖 |
| 5 | 按具体特性单独验收 |

---

## 七、命名与编码规范

- **类名**：PascalCase（`VulkanContext`, `SwapChain`）
- **方法名**：camelCase（`beginFrame()`, `createBuffer()`）
- **成员变量**：后缀下划线（`device_`, `swapChain_`）
- **常量**：`k` 前缀或全大写（`kMaxFramesInFlight`）
- **文件名**：PascalCase 与类名一致（`VulkanContext.h/cpp`）
- **命名空间**：`vkr`（Vulkan Renderer 缩写），所有类放入此命名空间
- **include guard**：使用 `#pragma once`
- **禁止全局变量**：当前 `vulkan_utils.h` 中的 `inline` 全局常量移入 `Config` 或命名空间 scope

---

## 八、重构注意事项

1. **每次只移动一块功能**，移动后立即编译运行验证
2. **先提取再重构** — 先把方法从大类中搬到新类，保持逻辑不变；确认能跑后再优化内部实现
3. **保留旧接口做过渡** — 新旧类可以并存一段时间，旧方法标记 `[[deprecated]]`
4. **Vulkan 对象销毁顺序很重要** — RAII 析构顺序即声明的逆序，设计类时注意成员声明顺序
5. **VMA 替换手动内存分配时**，逐步替换 `vkAllocateMemory`/`vkFreeMemory` 为 `vmaCreateBuffer`/`vmaDestroyBuffer`
6. **Shader 反射**（可选）— 可用 spirv-reflect 自动生成 DescriptorSetLayout，减少硬编码
