## 阶段 2.3 Application 瘦身 — 具体操作步骤

### 目标

将 App 中仍残留的 **非应用层** 资源搬入正确的归属类，消灭 `cleanup()` 中的大部分手动 `vkDestroy*` 调用，清理历史注释代码。完成后：

- `VmaAllocator` 由 `Device` 拥有，App 不再持有
- `VkPipeline` + `VkPipelineLayout` 由新的 RAII 类 `Pipeline` 管理
- `drawFrame()` 内联回 `mainLoop()`，删除 `vulkan_drawframe.cpp`
- `vulkan_device.cpp` 中遗留的 `createAllocator()` 删除
- `app.h` 中所有已注释掉的旧声明清除
- `cleanup()` 中手动 `vkDestroy` 减少到仅剩 descriptor 相关（留给阶段 3）

---

### 前置分析

#### App 当前仍持有的 Vulkan 资源

| 资源 | 类型 | 是否手动销毁 | 本阶段动作 |
|------|------|-------------|-----------|
| `allocator` | `VmaAllocator` | ✅ `vmaDestroyAllocator` | → 移入 Device |
| `graphicsPipeline` | `VkPipeline` | ✅ `vkDestroyPipeline` | → 移入 Pipeline RAII |
| `pipelineLayout` | `VkPipelineLayout` | ✅ `vkDestroyPipelineLayout` | → 移入 Pipeline RAII |
| `descriptorSetLayout` | `VkDescriptorSetLayout` | ✅ `vkDestroyDescriptorSetLayout` | 暂留 App（阶段 3 → Material） |
| `descriptorPool` | `VkDescriptorPool` | ✅ `vkDestroyDescriptorPool` | 暂留 App（阶段 3 → DescriptorManager） |
| `descriptorSets` | `vector<VkDescriptorSet>` | 随 pool 销毁 | 暂留 App |
| `textureSampler` | `VkSampler` | ✅ `vkDestroySampler` | 暂留 App（阶段 3 → Texture） |
| `textureImage_` | `unique_ptr<Image>` | RAII ✅ | 不动 |
| `vertexBuffer_` | `unique_ptr<Buffer>` | RAII ✅ | 不动 |
| `indexBuffer_` | `unique_ptr<Buffer>` | RAII ✅ | 不动 |

#### App 当前方法分布

| 源文件 | 方法 | 行数 | 本阶段动作 |
|--------|------|------|-----------|
| `vulkan_drawframe.cpp` | `drawFrame()` | ~42 | 内联到 mainLoop，删除文件 |
| `vulkan_pipeline.cpp` | `createGraphicsPipeline()`, `createShaderModule()` | ~210 | 移入 `core/Pipeline.cpp` |
| `vulkan_device.cpp` | `createAllocator()` | ~8 | 移入 `Device.cpp`，删除原函数 |
| `vulkan_vertex.cpp` | `createVertexBuffer()`, `createIndexBuffer()`, `copyBuffer()` | ~85 | 暂留（阶段 3 → Mesh） |
| `vulkan_texture.cpp` | 纹理相关 5 个方法 | ~210 | 暂留（阶段 3 → Texture） |
| `vulkan_mipmap.cpp` | `generateMipmaps()` | ~80 | 暂留（阶段 3 → Texture） |
| `vulkan_uniform.cpp` | descriptor 相关 4 个方法 | ~100 | 暂留（阶段 3 → Material/DescriptorManager） |
| `vulkan_model.cpp` | `loadModel()` | ~50 | 暂留（阶段 3 → ModelLoader） |

---

### 步骤 1：VmaAllocator 移入 Device

**原理**：VmaAllocator 是设备级资源，需要 `VkInstance`、`VkPhysicalDevice`、`VkDevice` 来创建。当前 Buffer 和 Image 通过 `Device&` 获取 `logicalDevice()` / `physicalDevice()`，未来也应从 Device 获取 allocator。将 allocator 放在 Device 中可以让 Buffer/Image 后续迁移到 VMA 时更自然。

#### 1a. 修改 Device.h

```cpp
// 新增 include
#include "vk_mem_alloc.h"

class Device {
public:
    Device(VulkanContext& ctx);
    ~Device();   // 析构中调用 vmaDestroyAllocator

    // ... 现有接口 ...

    VmaAllocator allocator() const { return allocator_; }   // 新增

private:
    // ... 现有成员 ...
    VmaAllocator allocator_ = VK_NULL_HANDLE;               // 新增

    void createAllocator();                                  // 新增（私有）
};
```

#### 1b. 修改 Device.cpp

- 在 `Device::Device()` 构造函数末尾调用 `createAllocator()`
- 将 `vulkan_device.cpp` 中 `HelloTriangleApplication::createAllocator()` 的逻辑搬入 `Device::createAllocator()`:
  ```cpp
  void Device::createAllocator() {
      VmaAllocatorCreateInfo info{};
      info.physicalDevice   = physicalDevice_;
      info.device           = device_;
      info.instance         = ctx_.instance();
      info.vulkanApiVersion = VK_API_VERSION_1_0;
      if (vmaCreateAllocator(&info, &allocator_) != VK_SUCCESS) {
          throw std::runtime_error("failed to create VMA allocator!");
      }
  }
  ```
- 在 `Device::~Device()` 中，在 `vkDestroyDevice` **之前** 调用 `vmaDestroyAllocator(allocator_)`

#### 1c. 修改 app.h / app.cpp

- 删除 `VmaAllocator allocator` 成员
- 删除 `void createAllocator()` 声明
- `initVulkan()` 中删除 `createAllocator()` 调用（Device 构造时已自动创建）
- `cleanup()` 中删除 `vmaDestroyAllocator(allocator)`
- `#include "vk_mem_alloc.h"` 从 `app.h` 中删除（不再直接使用 VMA 类型）

#### 1d. 删除 vulkan_device.cpp 中的遗留函数

- 删除 `vulkan_device.cpp` 中 `HelloTriangleApplication::createAllocator()` 函数体
- 如果 `vulkan_device.cpp` 中只剩该函数（已无其他 App 方法），则删除整个文件

#### 1e. 验证

```
cmake --build build --config Release
cmake --build build-debug --config Debug
```

运行确认功能不变。

---

### 步骤 2：Pipeline RAII 封装

**原理**：`VkPipeline` + `VkPipelineLayout` 是成对资源，当前在 App 中手动创建和销毁。封装为 RAII 类与 Buffer/Image 同级，放在 `src/core/`。

#### 2a. 创建 src/core/Pipeline.h

```cpp
#pragma once
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;

class Pipeline {
public:
    Pipeline(Device& device, VkRenderPass renderPass,
             VkDescriptorSetLayout descriptorSetLayout,
             const std::string& vertPath, const std::string& fragPath);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    VkPipeline       handle() const { return pipeline_; }
    VkPipelineLayout layout() const { return pipelineLayout_; }

private:
    static std::vector<char> readFile(const std::string& path);
    VkShaderModule createShaderModule(const std::vector<char>& code);

    Device*          device_ = nullptr;
    VkPipeline       pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
};

} // namespace vkr
```

**设计说明**：
- 构造器接收 `renderPass` 和 `descriptorSetLayout`，因为它们影响管线创建但不归 Pipeline 所有
- Shader 路径作为参数传入，不硬编码
- `readFile()` 和 `createShaderModule()` 作为内部辅助，不暴露

#### 2b. 创建 src/core/Pipeline.cpp

将 `vulkan_pipeline.cpp` 中的逻辑搬入：
- `readFile()` → `Pipeline::readFile()`（static 辅助）
- `createShaderModule()` → `Pipeline::createShaderModule()`
- `createGraphicsPipeline()` → `Pipeline::Pipeline()` 构造函数体
- 析构器中调用 `vkDestroyPipeline` + `vkDestroyPipelineLayout`

**注意事项**：
- 顶点输入格式（`Vertex::getBindingDescription()`）、MSAA 采样数（`device->msaaSamples()`）目前硬编码在管线创建中。本阶段保持现状，阶段 3+ 可引入 `PipelineConfig` 结构体参数化
- 动态状态 (viewport/scissor) 保持不变

#### 2c. 修改 app.h

```diff
- VkPipelineLayout pipelineLayout;
- VkPipeline       graphicsPipeline;
+ std::unique_ptr<vkr::Pipeline> pipeline_;

- void           createGraphicsPipeline();
- VkShaderModule createShaderModule(const std::vector<char> code);
```

新增 `#include "core/Pipeline.h"`。

#### 2d. 修改 app.cpp — initVulkan

```diff
- createGraphicsPipeline();
+ pipeline_ = std::make_unique<vkr::Pipeline>(
+     *device, renderer_->renderPass(), descriptorSetLayout,
+     "shader/vert.spv", "shader/frag.spv");
```

#### 2e. 修改 app.cpp — cleanup

```diff
- vkDestroyPipeline(d, graphicsPipeline, nullptr);
- vkDestroyPipelineLayout(d, pipelineLayout, nullptr);
+ pipeline_.reset();
```

#### 2f. 修改 vulkan_drawframe.cpp — drawFrame

```diff
- vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
+ vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->handle());

  vkCmdBindDescriptorSets(
-     cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1,
+     cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->layout(), 0, 1,
      &descriptorSets[renderer_->frameIndex()], 0, nullptr);
```

#### 2g. 删除 vulkan_pipeline.cpp

文件内容已完全迁入 `Pipeline.cpp`，删除该文件。

#### 2h. 验证

编译 Release + Debug，运行确认无回归。

---

### 步骤 3：drawFrame 内联到 mainLoop

**原理**：`vulkan_drawframe.cpp` 仅包含 ~42 行的 `drawFrame()`，该方法是应用特定的录制逻辑，不值得单独文件。将其内联到 `app.cpp` 的 `mainLoop()` 中，然后删除文件。

#### 3a. 修改 app.cpp — mainLoop

```cpp
void HelloTriangleApplication::mainLoop() {
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        VkCommandBuffer cmd = renderer_->beginFrame();
        if (!cmd) continue;

        updateUniformBuffer(renderer_->frameIndex());

        renderer_->beginRenderPass(cmd);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline_->handle());

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width  = static_cast<float>(swapChain_->extent().width);
        viewport.height = static_cast<float>(swapChain_->extent().height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = swapChain_->extent();
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        VkBuffer     vertexBuffers[] = {vertexBuffer_->handle()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(cmd, indexBuffer_->handle(), 0,
                             VK_INDEX_TYPE_UINT32);

        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->layout(),
            0, 1, &descriptorSets[renderer_->frameIndex()], 0, nullptr);
        vkCmdDrawIndexed(cmd, static_cast<uint32_t>(indices.size()),
                         1, 0, 0, 0);

        renderer_->endRenderPass(cmd);
        renderer_->endFrame();
    }

    vkDeviceWaitIdle(device->logicalDevice());
}
```

#### 3b. 修改 app.h

```diff
- void drawFrame();
```

#### 3c. 删除 vulkan_drawframe.cpp

#### 3d. 验证

编译运行。

---

### 步骤 4：清理死代码

#### 4a. app.h — 删除所有注释掉的旧声明

删除以下已注释掉的块（约 30 行）：
- `// VkInstance instance;` 等 Vulkan 核心对象注释
- `// VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;` 等设备注释
- `// void createInstance();` 等实例/调试方法注释
- `// void pickPhysicalDevice();` 等设备管理方法注释
- `// uint32_t findMemoryType(...)` 注释

#### 4b. vulkan_device.cpp — 检查并清理

如 `createAllocator()` 删除后文件只剩注释/空行，则删除整个文件。如仍有其他 App 遗留方法，保留但标注待后续阶段清理。

#### 4c. 验证

编译确认无遗漏引用。

---

### 变更后 App 状态一览

#### app.h 成员（步骤全部完成后）

```cpp
class HelloTriangleApplication {
private:
    // 窗口
    GLFWwindow* window;

    // 核心对象（RAII）
    std::unique_ptr<vkr::VulkanContext> context;
    std::unique_ptr<vkr::Device>       device;
    std::unique_ptr<vkr::SwapChain>    swapChain_;
    std::unique_ptr<vkr::Renderer>     renderer_;
    std::unique_ptr<vkr::Pipeline>     pipeline_;

    // 描述符（阶段 3 → Material / DescriptorManager）
    VkDescriptorSetLayout              descriptorSetLayout;
    VkDescriptorPool                   descriptorPool;
    std::vector<VkDescriptorSet>       descriptorSets;

    // 纹理（阶段 3 → Texture）
    std::unique_ptr<vkr::Image>        textureImage_;
    VkSampler                          textureSampler;
    uint32_t                           mipLevels;

    // 网格（阶段 3 → Mesh）
    std::unique_ptr<vkr::Buffer>       vertexBuffer_;
    std::unique_ptr<vkr::Buffer>       indexBuffer_;
    std::vector<Vertex>                vertices;
    std::vector<uint32_t>              indices;
};
```

#### cleanup()（步骤全部完成后）

```cpp
void HelloTriangleApplication::cleanup() {
    VkDevice d = device->logicalDevice();

    renderer_.reset();
    pipeline_.reset();

    vkDestroyDescriptorPool(d, descriptorPool, nullptr);
    vkDestroySampler(d, textureSampler, nullptr);
    textureImage_.reset();
    vkDestroyDescriptorSetLayout(d, descriptorSetLayout, nullptr);

    indexBuffer_.reset();
    vertexBuffer_.reset();

    // allocator 由 Device 析构自动销毁
    swapChain_.reset();
    device.reset();       // ~Device() → vmaDestroyAllocator → vkDestroyDevice
    context.reset();

    glfwDestroyWindow(window);
    glfwTerminate();
}
```

#### 删除的文件

| 文件 | 原因 |
|------|------|
| `vulkan_pipeline.cpp` | 内容迁入 `core/Pipeline.cpp` |
| `vulkan_drawframe.cpp` | 内联到 `app.cpp::mainLoop()` |
| `vulkan_device.cpp`（如果清空） | `createAllocator()` 迁入 Device |

#### 新增的文件

| 文件 | 内容 |
|------|------|
| `src/core/Pipeline.h` | Pipeline RAII 封装声明 |
| `src/core/Pipeline.cpp` | Pipeline 创建 / 销毁实现 |

---

### 阶段 2 验收标准

完成 2.1 + 2.2 + 2.3 后，阶段 2 整体验收：

| 标准 | 状态 |
|------|------|
| App 不直接持有 VkCommandBuffer / Fence / Semaphore | ✅（2.1+2.2 已完成） |
| App 不直接持有 VkPipeline / VkPipelineLayout | ✅（Pipeline RAII） |
| App 不直接持有 VmaAllocator | ✅（Device 拥有） |
| cleanup() 无 `vkDestroyPipeline` / `vkDestroyPipelineLayout` / `vmaDestroyAllocator` | ✅ |
| 无注释掉的死代码 | ✅ |
| `drawFrame()` 方法消除，录制逻辑内联在 mainLoop | ✅ |
| Release + Debug 编译通过，运行结果与重构前一致 | ✅ |
| 剩余手动 vkDestroy 仅为 descriptor / sampler / texture（明确属于阶段 3 范围） | ✅ |
