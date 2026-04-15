## 阶段 2.1 Renderer — 具体操作步骤

### 目标

将帧循环基础设施（RenderPass、CommandPool、CommandBuffer、Framebuffer、同步对象、Depth/Color 附件、单次命令辅助）从 `HelloTriangleApplication` 搬入独立的 `vkr::Renderer` 类。搬迁后 App 通过 `beginFrame()` / `endFrame()` 驱动渲染，不再直接持有上述资源。

---

### 前置分析

#### 搬入 Renderer 的成员变量

| 当前成员（app.h） | 类型 | 说明 |
|-------------------|------|------|
| `renderPass` | `VkRenderPass` | 渲染通道 |
| `commandPool` | `VkCommandPool` | 命令池 |
| `commandBuffers` | `vector<VkCommandBuffer>` | 每帧命令缓冲（MAX_FRAMES_IN_FLIGHT 个）|
| `swapChainFramebuffers` | `vector<VkFramebuffer>` | 每张交换链图像的帧缓冲 |
| `imageAvailableSemaphores` | `vector<VkSemaphore>` | per-frame，图像获取信号 |
| `renderFinishedSemaphores` | `vector<VkSemaphore>` | per-image，渲染完成信号 |
| `inFlightFences` | `vector<VkFence>` | per-frame，CPU-GPU 同步 |
| `depthImage_` | `unique_ptr<vkr::Image>` | 深度附件 |
| `colorImage_` | `unique_ptr<vkr::Image>` | MSAA 颜色附件 |
| `currentFrame` | `uint32_t` | 当前帧索引 |
| `framebufferResized` | `bool` | 窗口大小改变标记 |

#### 搬入 Renderer 的方法

| 方法 | 当前文件 | 外部依赖 |
|------|---------|---------|
| `createRenderPass()` | vulkan_renderpass.cpp | SwapChain::imageFormat(), Device::msaaSamples(), findDepthFormat() |
| `createCommandPool()` | vulkan_commandpool.cpp | Device::queueFamilies() |
| `createCommandBuffers()` | vulkan_commandpool.cpp | commandPool (内部) |
| `createFramebuffers()` | vulkan_framebuffers.cpp | renderPass, colorImage, depthImage, SwapChain (均为内部) |
| `createSyncObjects()` | vulkan_drawframe.cpp | Device, SwapChain::imageCount() |
| `createSwapChainSemaphores()` | vulkan_drawframe.cpp | Device, SwapChain::imageCount() |
| `createColorResources()` | vulkan_msaa.cpp | Device, SwapChain::imageFormat()/extent() |
| `createDepthResources()` | vulkan_depth.cpp | Device, SwapChain::extent(), findDepthFormat(), transitionImageLayout → beginSingleTimeCommands |
| `findDepthFormat()` | vulkan_depth.cpp | Device::physicalDevice() |
| `findSupportedFormat()` | vulkan_depth.cpp | Device::physicalDevice() |
| `beginSingleTimeCommands()` | vulkan_vertex.cpp | commandPool, Device::logicalDevice() |
| `endSingleTimeCommands()` | vulkan_vertex.cpp | commandPool, Device::{logicalDevice(), graphicsQueue()} |
| `drawFrame()` → 拆分为 beginFrame/endFrame | vulkan_drawframe.cpp | 同步对象、SwapChain、CommandBuffer |
| `recreateSwapChain()` | vulkan_swapchain.cpp | SwapChain::recreate(), 上述内部资源 |
| `cleanupSwapChain()` | vulkan_swapchain.cpp | 上述内部资源 |

#### 不搬入 Renderer 的方法（留在 App）

| 方法 | 原因 |
|------|------|
| `createGraphicsPipeline()` | 管线是应用特定资源，依赖 DescriptorSetLayout、Shader 等，属于阶段 3 (Material) |
| `createDescriptorSetLayout()` | 描述符布局是应用特定的 |
| `createUniformBuffers()` | UBO 属于阶段 2.2 (RenderFrame) |
| `updateUniformBuffer()` | 同上 |
| `createDescriptionPool()` / `createDescriptorSets()` | 描述符管理属于 DescriptorManager |
| `recordCommandBuffer()` | **拆分** — RenderPass 通用部分(begin/end)进 Renderer，应用绘制命令留 App |
| `copyBuffer()` | 资源加载辅助，留在 App（通过 Renderer::beginSingleTimeCommands 执行） |
| `transitionImageLayout()` / `copyBufferToImage()` | 同上 |

---

### Renderer 公开接口设计

```cpp
// src/core/Renderer.h
#pragma once

#include "Device.h"
#include "Image.h"
#include "SwapChain.h"

#include <functional>
#include <vector>
#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace vkr {

class Renderer {
public:
    Renderer(Device& device, SwapChain& swapChain);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // ---- 帧循环 ----
    // 返回可录制的 VkCommandBuffer；若交换链需重建则返回 VK_NULL_HANDLE
    VkCommandBuffer beginFrame();
    void            endFrame();

    // ---- RenderPass 辅助 ----
    void beginRenderPass(VkCommandBuffer cmd);
    void endRenderPass(VkCommandBuffer cmd);

    // ---- 单次命令辅助 ----
    VkCommandBuffer beginSingleTimeCommands();
    void            endSingleTimeCommands(VkCommandBuffer cmd);

    // ---- 交换链重建通知（由 App 在窗口 resize 时调用）----
    void notifyResize() { framebufferResized_ = true; }

    // ---- 访问器 ----
    VkRenderPass  renderPass()  const { return renderPass_; }
    VkCommandPool commandPool() const { return commandPool_; }
    uint32_t      frameIndex()  const { return currentFrame_; }
    uint32_t      imageIndex()  const { return currentImageIndex_; }

private:
    void createRenderPass();
    void createCommandPool();
    void createCommandBuffers();
    void createFramebuffers();
    void createSyncObjects();
    void createColorResources();
    void createDepthResources();

    void cleanupSwapChainResources();
    void recreateSwapChain();

    VkFormat findDepthFormat();
    VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates,
                                 VkImageTiling tiling,
                                 VkFormatFeatureFlags features);

    // 非拥有引用
    Device*    device_;
    SwapChain* swapChain_;

    // 渲染通道
    VkRenderPass renderPass_ = VK_NULL_HANDLE;

    // 命令
    VkCommandPool                commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    // 帧缓冲
    std::vector<VkFramebuffer> framebuffers_;

    // 附件
    std::unique_ptr<Image> colorImage_;   // MSAA
    std::unique_ptr<Image> depthImage_;

    // 同步对象
    std::vector<VkSemaphore> imageAvailableSemaphores_;   // per-frame
    std::vector<VkSemaphore> renderFinishedSemaphores_;   // per-image
    std::vector<VkFence>     inFlightFences_;             // per-frame

    // 帧状态
    uint32_t currentFrame_      = 0;
    uint32_t currentImageIndex_ = 0;
    bool     framebufferResized_ = false;
    bool     frameInProgress_    = false;
};

} // namespace vkr
```

---

### App 侧变化概览

重构后 `initVulkan()` 变为：

```cpp
void HelloTriangleApplication::initVulkan() {
    context = std::make_unique<vkr::VulkanContext>(window);
    device  = std::make_unique<vkr::Device>(*context);
    createAllocator();
    swapChain_ = std::make_unique<vkr::SwapChain>(*device, context->surface(), window);
    renderer_  = std::make_unique<vkr::Renderer>(*device, *swapChain_);
    //                                            ↑ Renderer 构造时自动创建：
    //                                              renderPass, commandPool, commandBuffers,
    //                                              colorResources, depthResources,
    //                                              framebuffers, syncObjects

    createDescriptorSetLayout();
    createGraphicsPipeline();       // 使用 renderer_->renderPass()
    createTextureImage();           // 使用 renderer_->beginSingleTimeCommands()
    createTextureImageView();
    createTextureSampler();
    loadModel();
    createVertexBuffer();           // 使用 renderer_->beginSingleTimeCommands()
    createIndexBuffer();
    createUniformBuffers();
    createDescriptionPool();
    createDescriptorSets();
    // createCommandBuffers / createSyncObjects 已由 Renderer 构造器完成
}
```

重构后 `drawFrame()` 变为：

```cpp
void HelloTriangleApplication::drawFrame() {
    VkCommandBuffer cmd = renderer_->beginFrame();
    if (cmd == VK_NULL_HANDLE) return;   // 交换链重建或窗口最小化

    updateUniformBuffer(renderer_->frameIndex());

    renderer_->beginRenderPass(cmd);

    // ---- 应用自己的绘制命令 ----
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);

    VkViewport viewport{};
    viewport.width  = (float)swapChain_->extent().width;
    viewport.height = (float)swapChain_->extent().height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = swapChain_->extent();
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkBuffer vertexBuffers[] = {vertexBuffer_->handle()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(cmd, indexBuffer_->handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, 0, 1,
                            &descriptorSets[renderer_->frameIndex()], 0, nullptr);
    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
    // ---- 绘制命令结束 ----

    renderer_->endRenderPass(cmd);
    renderer_->endFrame();
}
```

重构后 `cleanup()` 变为：

```cpp
void HelloTriangleApplication::cleanup() {
    VkDevice d = device->logicalDevice();

    uniformBuffers_.clear();
    vkDestroyDescriptorPool(d, descriptorPool, nullptr);
    vkDestroySampler(d, textureSampler, nullptr);
    textureImage_.reset();
    vkDestroyDescriptorSetLayout(d, descriptorSetLayout, nullptr);
    indexBuffer_.reset();
    vertexBuffer_.reset();

    vkDestroyPipeline(d, graphicsPipeline, nullptr);
    vkDestroyPipelineLayout(d, pipelineLayout, nullptr);

    renderer_.reset();   // ~Renderer() 销毁 RenderPass/CommandPool/Framebuffers/Sync/Attachments
    vmaDestroyAllocator(allocator);
    swapChain_.reset();
    device.reset();
    context.reset();

    glfwDestroyWindow(window);
    glfwTerminate();
}
```

---

### 逐步操作

#### 步骤 1：创建 `src/core/Renderer.h`

内容见上方"公开接口设计"。

#### 步骤 2：创建 `src/core/Renderer.cpp`

将以下方法逻辑搬入（类名前缀 `HelloTriangleApplication::` → `Renderer::`，成员变量加 `_` 后缀）：

| 源方法 | 源文件 | 搬入后方法 | 改动说明 |
|--------|--------|-----------|---------|
| `createRenderPass()` | vulkan_renderpass.cpp | `Renderer::createRenderPass()` | `swapChain_->imageFormat()` 不变，`device->msaaSamples()` → `device_->msaaSamples()`，`findDepthFormat()` 变为内部调用 |
| `createCommandPool()` | vulkan_commandpool.cpp (前 14 行) | `Renderer::createCommandPool()` | `device->` → `device_->` |
| `createCommandBuffers()` | vulkan_commandpool.cpp (16-28 行) | `Renderer::createCommandBuffers()` | 同上 |
| `createFramebuffers()` | vulkan_framebuffers.cpp | `Renderer::createFramebuffers()` | `swapChainFramebuffers` → `framebuffers_`，`renderPass` → `renderPass_`，`colorImage_`/`depthImage_` 已是内部成员 |
| `createSyncObjects()` | vulkan_drawframe.cpp (3-26 行) | `Renderer::createSyncObjects()` | 合并 `createSwapChainSemaphores()` 逻辑 |
| `createColorResources()` | vulkan_msaa.cpp (34-46 行) | `Renderer::createColorResources()` | `*device` → `*device_` |
| `createDepthResources()` | vulkan_depth.cpp (3-17 行) | `Renderer::createDepthResources()` | `transitionImageLayout` 调用改为 Renderer 内的 `beginSingleTimeCommands` + 直接录制 barrier |
| `findDepthFormat()` | vulkan_depth.cpp (40-44 行) | `Renderer::findDepthFormat()` | `findSuportedFormat` → `findSupportedFormat`（修正拼写）|
| `findSuportedFormat()` | vulkan_depth.cpp (19-38 行) | `Renderer::findSupportedFormat()` | `device->physicalDevice()` → `device_->physicalDevice()` |
| `beginSingleTimeCommands()` | vulkan_vertex.cpp (40-58 行) | `Renderer::beginSingleTimeCommands()` | `commandPool` → `commandPool_` |
| `endSingleTimeCommands()` | vulkan_vertex.cpp (60-74 行) | `Renderer::endSingleTimeCommands()` | 同上 |
| `drawFrame()` | vulkan_drawframe.cpp (44-103 行) | 拆分为 `beginFrame()` + `endFrame()`，见下方 |
| `recreateSwapChain()` | vulkan_swapchain.cpp (5-23 行) | `Renderer::recreateSwapChain()` | GLFW 窗口等待改为 `swapChain_->` 代理（见设计决策 2）|
| `cleanupSwapChain()` | vulkan_swapchain.cpp (25-40 行) | `Renderer::cleanupSwapChainResources()` | 只清理 Renderer 拥有的资源 |

**drawFrame 拆分细节：**

```
┌─ beginFrame() ─────────────────────────────────────────────┐
│  1. vkWaitForFences(inFlightFences_[currentFrame_])        │
│  2. vkAcquireNextImageKHR → currentImageIndex_             │
│     ├─ OUT_OF_DATE → recreateSwapChain(), return nullptr   │
│     └─ SUBOPTIMAL → 继续                                   │
│  3. vkResetFences                                          │
│  4. vkResetCommandBuffer(commandBuffers_[currentFrame_])   │
│  5. vkBeginCommandBuffer                                   │
│  6. return commandBuffers_[currentFrame_]                  │
└────────────────────────────────────────────────────────────┘

（App 录制自己的命令：bind pipeline, set viewport, draw...）

┌─ endFrame() ───────────────────────────────────────────────┐
│  1. vkEndCommandBuffer                                     │
│  2. VkSubmitInfo:                                          │
│     wait = imageAvailableSemaphores_[currentFrame_]        │
│     signal = renderFinishedSemaphores_[currentImageIndex_] │
│     fence = inFlightFences_[currentFrame_]                 │
│  3. vkQueueSubmit                                          │
│  4. VkPresentInfoKHR:                                      │
│     wait = renderFinishedSemaphores_[currentImageIndex_]   │
│  5. vkQueuePresentKHR                                      │
│     ├─ OUT_OF_DATE / SUBOPTIMAL / resized                  │
│     │   → recreateSwapChain()                              │
│     └─ 正常                                                │
│  6. currentFrame_ = (currentFrame_ + 1) % MAX_FRAMES       │
└────────────────────────────────────────────────────────────┘
```

**beginRenderPass / endRenderPass：**

```
┌─ beginRenderPass(cmd) ─────────────────────────────────────┐
│  VkRenderPassBeginInfo:                                    │
│    renderPass = renderPass_                                │
│    framebuffer = framebuffers_[currentImageIndex_]         │
│    extent = swapChain_->extent()                           │
│    clearValues = {black, depth=1.0}                        │
│  vkCmdBeginRenderPass(cmd, ...)                            │
└────────────────────────────────────────────────────────────┘

┌─ endRenderPass(cmd) ──────────────────────────────────────┐
│  vkCmdEndRenderPass(cmd)                                   │
└────────────────────────────────────────────────────────────┘
```

**Renderer 构造器 / 析构器：**

```
Renderer(device, swapChain):
    1. createCommandPool()        // 必须最先，后续方法可能需要
    2. createRenderPass()
    3. createColorResources()
    4. createDepthResources()     // 需要 commandPool（transitionImageLayout）
    5. createFramebuffers()       // 需要 renderPass + 附件
    6. createCommandBuffers()
    7. createSyncObjects()

~Renderer():
    vkDeviceWaitIdle()
    cleanupSwapChainResources()   // 销毁 framebuffers, colorImage, depthImage,
                                  //   renderFinishedSemaphores
    // 销毁非交换链生命周期的资源：
    for each frame:
        vkDestroySemaphore(imageAvailableSemaphores_[i])
        vkDestroyFence(inFlightFences_[i])
    vkDestroyRenderPass(renderPass_)
    vkDestroyCommandPool(commandPool_)   // 自动释放关联的 CommandBuffers
```

#### 步骤 3：修改 `app.h`

1. 添加 `#include "core/Renderer.h"`
2. 添加成员 `std::unique_ptr<vkr::Renderer> renderer_;`
3. 删除以下成员变量声明：
   - `VkRenderPass renderPass`
   - `VkCommandPool commandPool`
   - `std::vector<VkCommandBuffer> commandBuffers`
   - `std::vector<VkFramebuffer> swapChainFramebuffers`
   - `std::vector<VkSemaphore> imageAvailableSemaphores`
   - `std::vector<VkSemaphore> renderFinishedSemaphores`
   - `std::vector<VkFence> inFlightFences`
   - `uint32_t currentFrame`
   - `bool framebufferResized`
   - `std::unique_ptr<vkr::Image> depthImage_`
   - `std::unique_ptr<vkr::Image> colorImage_`
4. 删除以下方法声明：
   - `createRenderPass()`
   - `createFramebuffers()`
   - `createCommandPool()`
   - `createCommandBuffers()`
   - `createSyncObjects()`
   - `createSwapChainSemaphores()`
   - `recreateSwapChain()`
   - `cleanupSwapChain()`
   - `createColorResources()`
   - `createDepthResources()`
   - `findSuportedFormat()`
   - `findDepthFormat()`
   - `beginSingleTimeCommands()`
   - `endSingleTimeCommands()`
5. 将 `framebufferResizeCallback` 改为调用 `renderer_->notifyResize()`
6. `recordCommandBuffer()` 声明删除（逻辑直接写入新 `drawFrame()` 中）

#### 步骤 4：修改 `app.cpp`

- `initVulkan()`：替换为新版本（见上方），删除已搬入 Renderer 构造器的调用
- `cleanup()`：替换为新版本（见上方）
- `framebufferResizeCallback()`：`app->framebufferResized = true` → `app->renderer_->notifyResize()`
- `mainLoop()`：保持不变

#### 步骤 5：重写 `vulkan_drawframe.cpp`

删除 `createSyncObjects()`、`createSwapChainSemaphores()`、旧 `drawFrame()` 全部内容。替换为新 `drawFrame()`（App 侧版本，调用 `renderer_->beginFrame()` / `endFrame()`）。

内容：

```cpp
#include "app.h"

void HelloTriangleApplication::drawFrame() {
    VkCommandBuffer cmd = renderer_->beginFrame();
    if (cmd == VK_NULL_HANDLE) return;

    updateUniformBuffer(renderer_->frameIndex());

    renderer_->beginRenderPass(cmd);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);

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
    vkCmdBindIndexBuffer(cmd, indexBuffer_->handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, 0, 1,
                            &descriptorSets[renderer_->frameIndex()],
                            0, nullptr);
    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);

    renderer_->endRenderPass(cmd);
    renderer_->endFrame();
}
```

#### 步骤 6：清理搬空的源文件

| 文件 | 操作 |
|------|------|
| `vulkan_renderpass.cpp` | 整个文件内容删除（方法已搬入 Renderer.cpp）|
| `vulkan_framebuffers.cpp` | 整个文件内容删除 |
| `vulkan_msaa.cpp` | `createColorResources` 删除（如果文件只有这一个方法，整个清空）|
| `vulkan_depth.cpp` | `createDepthResources` / `findSuportedFormat` / `findDepthFormat` 删除（整个清空）|
| `vulkan_swapchain.cpp` | `recreateSwapChain` / `cleanupSwapChain` 删除（文件清空或删除）|
| `vulkan_commandpool.cpp` | `createCommandPool` / `createCommandBuffers` / `recordCommandBuffer` 删除（文件清空或删除）|
| `vulkan_vertex.cpp` | `beginSingleTimeCommands` / `endSingleTimeCommands` 删除，`copyBuffer` 改为调用 `renderer_->beginSingleTimeCommands()` |

> 注意：CMake 使用 `GLOB_RECURSE`，空的 .cpp 文件不会引发编译错误但会产生空翻译单元。可保留为仅含 `#include "app.h"` 的空壳，或直接删除文件后重新 `cmake ..`。

#### 步骤 7：修改 App 中残留的 Renderer 依赖

| 消费者 | 原引用 | 替换为 |
|--------|--------|--------|
| `createGraphicsPipeline()` (vulkan_pipeline.cpp) | `renderPass` | `renderer_->renderPass()` |
| `createGraphicsPipeline()` (vulkan_pipeline.cpp) | `swapChain_->extent()` (viewport/scissor) | 不变（或删除，因为使用动态状态） |
| `copyBuffer()` (vulkan_vertex.cpp) | `beginSingleTimeCommands()` / `endSingleTimeCommands()` | `renderer_->beginSingleTimeCommands()` / `renderer_->endSingleTimeCommands()` |
| `transitionImageLayout()` (vulkan_texture.cpp) | 同上 | 同上 |
| `copyBufferToImage()` (vulkan_texture.cpp) | 同上 | 同上 |
| `generateMipmaps()` (vulkan_mipmap.cpp) | 同上 | 同上 |

#### 步骤 8：构建验证

```bash
cd build && cmake .. && cmake --build . --config Release
cd ../build-debug && cmake .. && cmake --build . --config Debug
```

---

### 设计决策与说明

**1. beginRenderPass / endRenderPass 独立出来而非嵌入 beginFrame / endFrame**

如果把 RenderPass 的 begin/end 嵌入帧循环方法，App 就无法在 RenderPass 之前/之后录制其他命令（如 compute dispatch、image transition）。分开后 App 可以选择何时 begin/end RenderPass，为未来多 Pass 留余地。

**2. recreateSwapChain 中的 GLFW 窗口等待**

当前 `recreateSwapChain()` 在开头有 `glfwGetFramebufferSize` + `glfwWaitEvents` 等待窗口恢复。Renderer 不应直接依赖 GLFW。两种方案：

- **方案 A（推荐）**：Renderer 的 `beginFrame()` 在 acquireNextImage 返回 `OUT_OF_DATE` 时调用 `recreateSwapChain()`，但在此之前检查 `swapChain_->extent()` 是否为 {0,0}（最小化时），如果是则直接返回 `VK_NULL_HANDLE`，由 App 的主循环自然重试。将 GLFW 的最小化等待逻辑保留在 App 的 `mainLoop` 或 `drawFrame` 入口。
- 方案 B：SwapChain 类增加一个 `waitForNonZeroExtent(GLFWwindow*)` 方法，Renderer 调用 `swapChain_->waitForNonZeroExtent()`。

**3. renderFinishedSemaphores 保持 per-image**

当前代码使用 per-image renderFinished 信号量（数量 = swapChain imageCount）。plan.md 中 RenderFrame 将其改为 per-frame。为减小 2.1 的变更范围，**本步骤保持 per-image 不变**，留到 2.2 (RenderFrame) 时统一改为 per-frame。

**4. transitionImageLayout 问题**

`createDepthResources()` 内部调用 `transitionImageLayout()`，后者目前是 App 的方法，使用 `beginSingleTimeCommands`。搬入 Renderer 后，Renderer 自己拥有 `beginSingleTimeCommands`，可以直接内联一段 barrier 录制逻辑，或保留一个私有的 `transitionImageLayout()` 辅助方法。推荐内联（只在 createDepthResources 一处使用）。

**5. commandPool 的 beginSingleTimeCommands 归属**

目标架构中 `CommandManager` 负责命令池和单次命令。2.1 暂时将此职责放在 Renderer 中作为公共方法，App 通过 `renderer_->beginSingleTimeCommands()` 使用。后续可提取为独立类。

**6. 空文件处理**

搬移后部分 .cpp 文件变空。建议直接删除这些文件，在 `cmake ..` 时 GLOB_RECURSE 自动剔除。需要删除的文件列表：
- `vulkan_renderpass.cpp`
- `vulkan_framebuffers.cpp`
- `vulkan_msaa.cpp`
- `vulkan_depth.cpp`
- `vulkan_swapchain.cpp`
- `vulkan_commandpool.cpp`

`vulkan_drawframe.cpp` 保留，因为新 `drawFrame()` 就写在这里。

---

### 验收标准

1. `HelloTriangleApplication` 不再直接持有 `VkRenderPass`、`VkCommandPool`、`VkCommandBuffer`、`VkFramebuffer`、`VkSemaphore`、`VkFence`、depth/color Image
2. `cleanup()` 中不再有 `vkDestroyRenderPass`、`vkDestroyCommandPool`、`vkDestroySemaphore`、`vkDestroyFence`、`vkDestroyFramebuffer` 调用
3. App 通过 `renderer_->beginFrame()` / `endFrame()` 驱动帧循环
4. `renderer_->beginSingleTimeCommands()` / `endSingleTimeCommands()` 可被 App 正常调用（createTextureImage、copyBuffer 等）
5. Release + Debug 编译通过，渲染结果与重构前一致
6. 窗口最小化/恢复/resize 行为正常
