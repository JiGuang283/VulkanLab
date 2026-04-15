## 阶段 2.2 FrameData — 具体操作步骤

### 目标

将 Renderer 中分散的 per-frame 向量（`commandBuffers_`、`imageAvailableSemaphores_`、`inFlightFences_`）和 App 中的 per-frame 资源（`uniformBuffers_`）收拢为统一的 `FrameData` 结构体数组。同时将 `renderFinishedSemaphores` 从 per-image 改为 per-frame，完成同步模型的简化。

---

### 前置分析

#### 当前 per-frame 资源分布

| 资源 | 当前位置 | 索引方式 | 数量 |
|------|---------|---------|------|
| `commandBuffers_` | Renderer | `currentFrame_` | MAX_FRAMES_IN_FLIGHT (2) |
| `imageAvailableSemaphores_` | Renderer | `currentFrame_` | MAX_FRAMES_IN_FLIGHT (2) |
| `inFlightFences_` | Renderer | `currentFrame_` | MAX_FRAMES_IN_FLIGHT (2) |
| `renderFinishedSemaphores_` | Renderer | `currentImageIndex_` | swapChain imageCount (≥2) |
| `uniformBuffers_` | App (app.h) | `currentFrame` via `renderer_->frameIndex()` | MAX_FRAMES_IN_FLIGHT (2) |

#### 变更后 FrameData 结构

```cpp
struct FrameData {
    VkCommandBuffer          commandBuffer  = VK_NULL_HANDLE;
    VkSemaphore              imageAvailable = VK_NULL_HANDLE;
    VkSemaphore              renderFinished = VK_NULL_HANDLE;  // 改为 per-frame
    VkFence                  inFlight       = VK_NULL_HANDLE;
    std::unique_ptr<Buffer>  uniformBuffer;
};
```

#### 不搬入 FrameData 的资源（留在 App，阶段 3 搬入 Material/DescriptorManager）

| 资源 | 原因 |
|------|------|
| `descriptorPool` | 描述符池与 layout 紧密耦合，属于阶段 3 (DescriptorManager) |
| `descriptorSets` | 引用具体纹理和 UBO，layout 是应用特定的 |
| `descriptorSetLayout` | 与 shader binding 相关，属于 Material |

> App 仍通过 `renderer_->frameIndex()` 索引 `descriptorSets`，但 UBO 改为从 Renderer 获取。

---

### 关键设计决策

**1. renderFinishedSemaphores 从 per-image 改为 per-frame**

当前代码中 `renderFinishedSemaphores_` 数量为 swapChain 的 imageCount（通常 3），以 `currentImageIndex_` 索引。这是因为多个 in-flight 帧可能使用同一个 swapChain image，需要独立的 signal semaphore 避免冲突。

然而，在双缓冲（MAX_FRAMES_IN_FLIGHT=2）模型中，每帧已有独立的 `inFlightFence` 保证 GPU 完成后才复用资源。将 renderFinished 改为 per-frame（与 imageAvailable 一一对应）不会产生同步问题，且简化了 FrameData 的结构——所有 per-frame 资源统一生命周期。

变更影响：
- `createSyncObjects()`：不再单独创建 per-image semaphores
- `endFrame()`：`signalSemaphores` 从 `[currentImageIndex_]` 改为 `[currentFrame_]`
- `cleanupSwapChainResources()`：不再销毁 `renderFinishedSemaphores_`（它们跟随 FrameData 在析构器中销毁）

**2. uniformBuffer 由 Renderer 管理，App 通过访问器写入**

Renderer 在构造时为每个 FrameData 创建一个 UBO（持久映射），并暴露访问器：
```cpp
void* mappedUniformBuffer(uint32_t frameIndex) const;
VkBuffer uniformBufferHandle(uint32_t frameIndex) const;
```

App 的 `updateUniformBuffer()` 改为：
```cpp
memcpy(renderer_->mappedUniformBuffer(frameIdx), &ubo, sizeof(ubo));
```

App 的 `createDescriptorSets()` 使用 `renderer_->uniformBufferHandle(i)` 获取 buffer handle 来填写 `VkDescriptorBufferInfo`。

**3. UBO 大小由外部传入**

Renderer 不应知道 `UniformBufferObject` 的具体结构。UBO 大小通过构造参数传入：
```cpp
Renderer(Device& device, SwapChain& swapChain, VkDeviceSize uniformBufferSize);
```

若 `uniformBufferSize == 0` 则不创建 UBO（为未来无 UBO 场景留余地）。

**4. FrameData 作为 Renderer 私有内部类型**

`FrameData` 定义在 Renderer 内部（`private`），外部不直接访问。通过 Renderer 的访问器暴露所需字段。这保持了封装性，未来改变 FrameData 内部结构不影响 App。

---

### Renderer 新增／修改接口

```cpp
class Renderer {
public:
    // 构造器增加 uniformBufferSize 参数
    Renderer(Device& device, SwapChain& swapChain,
             VkDeviceSize uniformBufferSize);
    ~Renderer();

    // ---- 帧循环（不变）----
    VkCommandBuffer beginFrame();
    void            endFrame();

    // ---- RenderPass 辅助（不变）----
    void beginRenderPass(VkCommandBuffer cmd);
    void endRenderPass(VkCommandBuffer cmd);

    // ---- 单次命令辅助（不变）----
    VkCommandBuffer beginSingleTimeCommands();
    void            endSingleTimeCommands(VkCommandBuffer cmd);

    // ---- 交换链重建通知（不变）----
    void notifyResize() { framebufferResized_ = true; }

    // ---- 访问器（不变）----
    VkRenderPass  renderPass()  const;
    VkCommandPool commandPool() const;
    uint32_t      frameIndex()  const;
    uint32_t      imageIndex()  const;

    // ---- 新增访问器 ----
    void*    mappedUniformBuffer(uint32_t frameIndex) const;
    VkBuffer uniformBufferHandle(uint32_t frameIndex) const;

private:
    struct FrameData {
        VkCommandBuffer          commandBuffer  = VK_NULL_HANDLE;
        VkSemaphore              imageAvailable = VK_NULL_HANDLE;
        VkSemaphore              renderFinished = VK_NULL_HANDLE;
        VkFence                  inFlight       = VK_NULL_HANDLE;
        std::unique_ptr<Buffer>  uniformBuffer;
    };

    // ... 私有方法不变，新增 createUniformBuffers() ...
    void createUniformBuffers();

    // 成员变量变更：
    // 删除: commandBuffers_, imageAvailableSemaphores_,
    //        renderFinishedSemaphores_, inFlightFences_
    // 新增:
    std::array<FrameData, MAX_FRAMES_IN_FLIGHT> frames_;
    VkDeviceSize uniformBufferSize_ = 0;

    // 保留不变: renderPass_, commandPool_, framebuffers_,
    //           colorImage_, depthImage_, swapChain_, device_,
    //           currentFrame_, currentImageIndex_,
    //           framebufferResized_, frameInProgress_
};
```

---

### App 侧变化概览

#### app.h 变更

```diff
- std::vector<std::unique_ptr<vkr::Buffer>> uniformBuffers_;
  // uniformBuffers 已移入 Renderer::FrameData
```

方法声明变更：
```diff
- void createUniformBuffers();
  // 不再需要，Renderer 构造时自动创建
```

#### app.cpp initVulkan() 变更

```diff
  renderer_ = std::make_unique<vkr::Renderer>(
-     *device, *swapChain_);
+     *device, *swapChain_, sizeof(UniformBufferObject));
  createDescriptorSetLayout();
  createGraphicsPipeline();
  createTextureImage();
  createTextureImageView();
  createTextureSampler();
  loadModel();
  createVertexBuffer();
  createIndexBuffer();
- createUniformBuffers();
  createDescriptionPool();
  createDescriptorSets();
```

#### vulkan_uniform.cpp 变更

`createUniformBuffers()` — 删除整个函数。

`updateUniformBuffer()` — 简化：
```cpp
void HelloTriangleApplication::updateUniformBuffer(uint32_t currentImage) {
    // ... 计算 ubo 不变 ...
    memcpy(renderer_->mappedUniformBuffer(currentImage), &ubo, sizeof(ubo));
}
```

`createDescriptorSets()` — 修改 bufferInfo：
```cpp
bufferInfo.buffer = renderer_->uniformBufferHandle(i);
```

#### cleanup() 变更

```diff
- uniformBuffers_.clear();
  // uniformBuffers 由 Renderer 析构自动释放
```

---

### 逐步操作

#### 步骤 1：修改 `src/core/Renderer.h`

1. 在 `private` 区域顶部定义 `FrameData` 结构体
2. 添加 `#include <array>` 和前向声明需要的头文件
3. 将以下成员变量替换为 `std::array<FrameData, MAX_FRAMES_IN_FLIGHT> frames_`：
   - 删除 `std::vector<VkCommandBuffer> commandBuffers_`
   - 删除 `std::vector<VkSemaphore> imageAvailableSemaphores_`
   - 删除 `std::vector<VkSemaphore> renderFinishedSemaphores_`
   - 删除 `std::vector<VkFence> inFlightFences_`
4. 添加 `VkDeviceSize uniformBufferSize_ = 0` 成员
5. 修改构造器签名：增加 `VkDeviceSize uniformBufferSize` 参数
6. 添加私有方法声明 `void createUniformBuffers()`
7. 添加公开访问器：
   - `void* mappedUniformBuffer(uint32_t frameIndex) const`
   - `VkBuffer uniformBufferHandle(uint32_t frameIndex) const`

#### 步骤 2：修改 `src/core/Renderer.cpp`

**构造器：**
```diff
- Renderer::Renderer(Device &device, SwapChain &swapChain)
-     : device_(&device), swapChain_(&swapChain) {
+ Renderer::Renderer(Device &device, SwapChain &swapChain,
+                    VkDeviceSize uniformBufferSize)
+     : device_(&device), swapChain_(&swapChain),
+       uniformBufferSize_(uniformBufferSize) {
      createCommandPool();
      createRenderPass();
      createColorResources();
      createDepthResources();
      createFramebuffers();
      createCommandBuffers();
      createSyncObjects();
+     createUniformBuffers();
  }
```

**析构器** — 将分散的循环销毁改为遍历 `frames_`：
```cpp
~Renderer() {
    vkDeviceWaitIdle(device_->logicalDevice());
    cleanupSwapChainResources();

    VkDevice d = device_->logicalDevice();
    for (auto& f : frames_) {
        vkDestroySemaphore(d, f.imageAvailable, nullptr);
        vkDestroySemaphore(d, f.renderFinished, nullptr);
        vkDestroyFence(d, f.inFlight, nullptr);
        f.uniformBuffer.reset();
    }

    vkDestroyRenderPass(d, renderPass_, nullptr);
    vkDestroyCommandPool(d, commandPool_, nullptr);
}
```

**beginFrame()** — 所有索引改为 `frames_[currentFrame_].xxx`：
```diff
- vkWaitForFences(d, 1, &inFlightFences_[currentFrame_], ...);
+ vkWaitForFences(d, 1, &frames_[currentFrame_].inFlight, ...);

- vkAcquireNextImageKHR(d, ..., imageAvailableSemaphores_[currentFrame_], ...);
+ vkAcquireNextImageKHR(d, ..., frames_[currentFrame_].imageAvailable, ...);

- vkResetFences(d, 1, &inFlightFences_[currentFrame_]);
+ vkResetFences(d, 1, &frames_[currentFrame_].inFlight);

- vkResetCommandBuffer(commandBuffers_[currentFrame_], 0);
+ vkResetCommandBuffer(frames_[currentFrame_].commandBuffer, 0);

- vkBeginCommandBuffer(commandBuffers_[currentFrame_], &beginInfo);
+ vkBeginCommandBuffer(frames_[currentFrame_].commandBuffer, &beginInfo);

- return commandBuffers_[currentFrame_];
+ return frames_[currentFrame_].commandBuffer;
```

**endFrame()** — 索引改为 `frames_[]`，renderFinished 改为 per-frame：
```diff
- vkEndCommandBuffer(commandBuffers_[currentFrame_]);
+ vkEndCommandBuffer(frames_[currentFrame_].commandBuffer);

- VkSemaphore waitSemaphores[] = {imageAvailableSemaphores_[currentFrame_]};
+ VkSemaphore waitSemaphores[] = {frames_[currentFrame_].imageAvailable};

- submitInfo.pCommandBuffers = &commandBuffers_[currentFrame_];
+ submitInfo.pCommandBuffers = &frames_[currentFrame_].commandBuffer;

  // renderFinished 改为 per-frame
- VkSemaphore signalSemaphores[] = {renderFinishedSemaphores_[currentImageIndex_]};
+ VkSemaphore signalSemaphores[] = {frames_[currentFrame_].renderFinished};

- vkQueueSubmit(..., inFlightFences_[currentFrame_]);
+ vkQueueSubmit(..., frames_[currentFrame_].inFlight);
```

**createCommandBuffers()** — 分配到 `frames_` 数组：
```cpp
void Renderer::createCommandBuffers() {
    std::vector<VkCommandBuffer> buffers(MAX_FRAMES_IN_FLIGHT);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = MAX_FRAMES_IN_FLIGHT;

    if (vkAllocateCommandBuffers(device_->logicalDevice(), &allocInfo,
                                 buffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate command buffers!");
    }
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        frames_[i].commandBuffer = buffers[i];
    }
}
```

**createSyncObjects()** — 全部写入 `frames_`，不再创建 per-image semaphores：
```cpp
void Renderer::createSyncObjects() {
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkDevice d = device_->logicalDevice();
    for (auto& f : frames_) {
        if (vkCreateSemaphore(d, &semaphoreInfo, nullptr, &f.imageAvailable) != VK_SUCCESS ||
            vkCreateSemaphore(d, &semaphoreInfo, nullptr, &f.renderFinished) != VK_SUCCESS ||
            vkCreateFence(d, &fenceInfo, nullptr, &f.inFlight) != VK_SUCCESS) {
            throw std::runtime_error("failed to create sync objects for a frame!");
        }
    }
}
```

**新增 createUniformBuffers()：**
```cpp
void Renderer::createUniformBuffers() {
    if (uniformBufferSize_ == 0) return;
    for (auto& f : frames_) {
        f.uniformBuffer = std::make_unique<Buffer>(
            *device_, uniformBufferSize_,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        f.uniformBuffer->map();
    }
}
```

**新增访问器实现：**
```cpp
void* Renderer::mappedUniformBuffer(uint32_t frameIndex) const {
    return frames_[frameIndex].uniformBuffer->mappedData();
}

VkBuffer Renderer::uniformBufferHandle(uint32_t frameIndex) const {
    return frames_[frameIndex].uniformBuffer->handle();
}
```

**cleanupSwapChainResources()** — 删除 `renderFinishedSemaphores_` 销毁代码：
```diff
  void Renderer::cleanupSwapChainResources() {
      VkDevice d = device_->logicalDevice();
-     for (auto semaphore : renderFinishedSemaphores_) {
-         vkDestroySemaphore(d, semaphore, nullptr);
-     }
-     renderFinishedSemaphores_.clear();

      for (auto framebuffer : framebuffers_) {
          vkDestroyFramebuffer(d, framebuffer, nullptr);
      }
      framebuffers_.clear();
      colorImage_.reset();
      depthImage_.reset();
  }
```

#### 步骤 3：修改 `src/app.h`

1. 删除 `std::vector<std::unique_ptr<vkr::Buffer>> uniformBuffers_` 成员
2. 删除 `void createUniformBuffers()` 方法声明

#### 步骤 4：修改 `src/app.cpp`

`initVulkan()`：
```diff
- renderer_ = std::make_unique<vkr::Renderer>(*device, *swapChain_);
+ renderer_ = std::make_unique<vkr::Renderer>(
+     *device, *swapChain_, sizeof(UniformBufferObject));
```

删除 `createUniformBuffers()` 调用行。

`cleanup()`：
```diff
- uniformBuffers_.clear();
```

#### 步骤 5：修改 `src/vulkan_uniform.cpp`

**删除 `createUniformBuffers()` 函数**（整个函数体删除）。

**修改 `updateUniformBuffer()`：**
```diff
- memcpy(uniformBuffers_[currentImage]->mappedData(), &ubo, sizeof(ubo));
+ memcpy(renderer_->mappedUniformBuffer(currentImage), &ubo, sizeof(ubo));
```

**修改 `createDescriptorSets()` 中的 bufferInfo：**
```diff
- bufferInfo.buffer = uniformBuffers_[i]->handle();
+ bufferInfo.buffer = renderer_->uniformBufferHandle(i);
```

#### 步骤 6：构建验证

```bash
cd build && cmake .. && cmake --build . --config Release
cd ../build-debug && cmake .. && cmake --build . --config Debug
```

运行验证渲染结果与重构前一致。

---

### 受影响文件汇总

| 文件 | 操作 | 改动规模 |
|------|------|---------|
| `src/core/Renderer.h` | 修改 | 中 — 新增 FrameData 结构体，替换成员变量，新增访问器 |
| `src/core/Renderer.cpp` | 修改 | 大 — 所有 `xxxSemaphores_[i]`/`commandBuffers_[i]` 索引替换为 `frames_[i].xxx`，新增 createUniformBuffers |
| `src/app.h` | 修改 | 小 — 删除 2 项 |
| `src/app.cpp` | 修改 | 小 — 构造器参数、删除 2 行 |
| `src/vulkan_uniform.cpp` | 修改 | 中 — 删除 createUniformBuffers，修改 updateUniformBuffer 和 createDescriptorSets |

---

### 验收标准

1. `HelloTriangleApplication` 不再持有 `uniformBuffers_` 成员
2. `Renderer` 内部使用 `std::array<FrameData, MAX_FRAMES_IN_FLIGHT>` 管理所有 per-frame 资源
3. `renderFinishedSemaphores` 为 per-frame（数量 = MAX_FRAMES_IN_FLIGHT = 2），不再是 per-image
4. `cleanupSwapChainResources()` 中无 semaphore 销毁代码（semaphore 生命周期与 FrameData 一致，在析构器销毁）
5. App 通过 `renderer_->mappedUniformBuffer()` 写入 UBO 数据
6. App 通过 `renderer_->uniformBufferHandle()` 获取 buffer handle 创建 descriptor set
7. Release + Debug 编译通过，渲染结果与重构前一致
8. 窗口 resize / 最小化行为正常
