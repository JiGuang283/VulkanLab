# 同步机制分析与修复方案

## 1. 当前架构概览

### 1.1 同步原语清单

```
MAX_FRAMES_IN_FLIGHT = 2

FrameData (× 2，按 currentFrame_ 索引):
├── imageAvailable  — VkSemaphore  （acquire → submit 等待）
├── renderFinished  — VkSemaphore  （submit 信号 → present 等待）
├── inFlight        — VkFence      （submit 信号 → CPU 等待）
├── commandBuffer   — VkCommandBuffer
└── uniformBuffer   — Buffer*
```

### 1.2 帧循环时序

```
currentFrame_ = 0, 1, 0, 1, ...  （在 endFrame() 末尾递增）

beginFrame():
  ① vkWaitForFences(inFlight[currentFrame_])       // CPU 等 GPU 完成第 N-2 帧
  ② vkAcquireNextImageKHR(imageAvailable[currentFrame_]) → currentImageIndex_
  ③ vkResetFences(inFlight[currentFrame_])
  ④ 录制命令...

endFrame():
  ⑤ vkQueueSubmit:
       waitSemaphores  = { imageAvailable[currentFrame_] }
       signalSemaphores = { renderFinished[currentFrame_] }  ← 问题根源
       signalFence      = inFlight[currentFrame_]
  ⑥ vkQueuePresentKHR:
       waitSemaphores  = { renderFinished[currentFrame_] }
       imageIndex      = currentImageIndex_
  ⑦ currentFrame_ = (currentFrame_ + 1) % 2
```

## 2. Bug 分析：renderFinished 信号量重用违规

### 2.1 问题本质

`renderFinished` 信号量按 **in-flight frame index**（0/1 交替）索引，
但 `vkQueuePresentKHR` **不提供 fence 也不 signal 信号量**，
因此 **没有任何机制** 能确认 present 操作何时真正释放了 `renderFinished`。

CPU 端的 `vkWaitForFences(inFlight)` 只保证 **`vkQueueSubmit` 完成**（即 GPU 渲染完成），
但 Vulkan 规范并不保证这也意味着同一帧的 `vkQueuePresentKHR` 已经完成。
Present 是一个异步操作，它对 `renderFinished` 的等待可能仍在进行中。

### 2.2 触发场景

假设交换链有 3 张图像（indices 0, 1, 2），MAX_FRAMES_IN_FLIGHT = 2：

```
时间线:
  帧 A: frame=0, acquire→image 0, submit signal renderFinished[0], present image 0
  帧 B: frame=1, acquire→image 1, submit signal renderFinished[1], present image 1
  帧 C: frame=0, WaitFence[0] ✓ (submit A 完成)
         → 但 present A  可能还没完成，renderFinished[0] 可能仍被 swapchain 持有
         acquire→image 2
         vkQueueSubmit signal renderFinished[0]  ← VUID-vkQueueSubmit-pSignalSemaphores-00067 !!
```

**关键洞察**：`vkAcquireNextImageKHR` 返回 image 2（不是 image 0），
这意味着 image 0 的 present 操作 **尚未完成**，image 0 尚未被"归还"，
因此绑定在 image 0 那次 present 上的 `renderFinished[0]` 仍可能处于 pending 状态。

验证层报告的信息完全印证了这一点：
```
Swapchain image 0 was presented but was not re-acquired,
so VkSemaphore 0x180000000018 may still be in use
and cannot be safely reused with image index 2.
```

### 2.3 为什么"通常能跑"

大多数驱动实现中，`vkWaitForFences` 完成时 present 操作碰巧也已经完成（因为 present 依赖渲染完成后才能执行）。但这不是规范保证的行为——在某些驱动或高帧率 MAILBOX 模式下就会出问题。

## 3. 修复方案

### 方案 A：renderFinished 按 swapchain image index 索引（推荐）

这是 Vulkan 官方指南推荐的标准做法
（[swapchain_semaphore_reuse](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)）：

**核心思路**：当 `vkAcquireNextImageKHR` 返回 image index N 时，
可以保证 **上一次使用 image N 的 present 操作已完成**，
因此绑定在 image N 上的 `renderFinished[N]` 已经安全释放，可以重用。

#### 改动内容

##### Renderer.h

```cpp
struct FrameData {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore     imageAvailable = VK_NULL_HANDLE;  // 按 frame index 索引 ✓
    VkFence         inFlight = VK_NULL_HANDLE;         // 按 frame index 索引 ✓
    std::unique_ptr<Buffer> uniformBuffer;
};

// renderFinished 从 FrameData 中分离出来
std::array<FrameData, MAX_FRAMES_IN_FLIGHT>  frames_;

// 新增：按 swapchain image 数量分配
std::vector<VkSemaphore> renderFinishedSemaphores_;    // 按 image index 索引
```

##### Renderer.cpp — createSyncObjects()

```cpp
void Renderer::createSyncObjects() {
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkDevice d = device_->logicalDevice();
    for (auto &f : frames_) {
        VK_CHECK(vkCreateSemaphore(d, &semaphoreInfo, nullptr, &f.imageAvailable));
        VK_CHECK(vkCreateFence(d, &fenceInfo, nullptr, &f.inFlight));
    }

    // renderFinished 按 swapchain image 数量分配
    renderFinishedSemaphores_.resize(swapChain_->imageCount());
    for (auto &sem : renderFinishedSemaphores_) {
        VK_CHECK(vkCreateSemaphore(d, &semaphoreInfo, nullptr, &sem));
    }
}
```

##### Renderer.cpp — endFrame()

```cpp
void Renderer::endFrame() {
    VkDevice d = device_->logicalDevice();
    VK_CHECK(vkEndCommandBuffer(frames_[currentFrame_].commandBuffer));

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = { frames_[currentFrame_].imageAvailable };
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores    = waitSemaphores;
    submitInfo.pWaitDstStageMask  = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &frames_[currentFrame_].commandBuffer;

    // 关键：用 currentImageIndex_ 而非 currentFrame_ 来索引 renderFinished
    VkSemaphore signalSemaphores[] = { renderFinishedSemaphores_[currentImageIndex_] };
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = signalSemaphores;

    VK_CHECK(vkQueueSubmit(device_->graphicsQueue(), 1, &submitInfo,
                           frames_[currentFrame_].inFlight));

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores    = signalSemaphores;  // = renderFinished[imageIndex]

    VkSwapchainKHR swapChains[] = { swapChain_->handle() };
    presentInfo.swapchainCount  = 1;
    presentInfo.pSwapchains     = swapChains;
    presentInfo.pImageIndices   = &currentImageIndex_;

    // ... present + resize handling ...
}
```

##### Renderer.cpp — 析构函数

```cpp
Renderer::~Renderer() {
    vkDeviceWaitIdle(device_->logicalDevice());
    cleanupSwapChainResources();

    VkDevice d = device_->logicalDevice();
    for (auto &f : frames_) {
        vkDestroySemaphore(d, f.imageAvailable, nullptr);
        vkDestroyFence(d, f.inFlight, nullptr);
        f.uniformBuffer.reset();
    }
    for (auto sem : renderFinishedSemaphores_) {
        vkDestroySemaphore(d, sem, nullptr);
    }

    vkDestroyRenderPass(d, renderPass_, nullptr);
    vkDestroyCommandPool(d, commandPool_, nullptr);
}
```

##### Renderer.cpp — recreateSwapChain()

交换链重建时，image 数量可能变化，需要重建 renderFinished 信号量：

```cpp
void Renderer::recreateSwapChain() {
    vkDeviceWaitIdle(device_->logicalDevice());
    cleanupSwapChainResources();

    // 销毁旧的 renderFinished（数量可能变化）
    VkDevice d = device_->logicalDevice();
    for (auto sem : renderFinishedSemaphores_) {
        vkDestroySemaphore(d, sem, nullptr);
    }
    renderFinishedSemaphores_.clear();

    swapChain_->recreate();
    createColorResources();
    createDepthResources();
    createFramebuffers();

    // 按新的 image 数量重建
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    renderFinishedSemaphores_.resize(swapChain_->imageCount());
    for (auto &sem : renderFinishedSemaphores_) {
        VK_CHECK(vkCreateSemaphore(d, &semInfo, nullptr, &sem));
    }
}
```

#### 正确性证明

```
前提：vkAcquireNextImageKHR 返回 image index N
 ⇒ Vulkan 保证：上一次 present image N 的操作已完成
 ⇒ 上一次 present 对 renderFinished[N] 的等待已完成
 ⇒ renderFinished[N] 已回到 unsignaled 状态
 ⇒ vkQueueSubmit 可以安全 signal renderFinished[N] ✓
```

### 方案 B：VK_EXT_swapchain_maintenance1（备选）

使用扩展允许 `vkQueuePresentKHR` 指定 fence，
可以直接在 CPU 侧等待 present 完成。但该扩展：
- 并非所有驱动都支持
- 需要额外的扩展启用逻辑
- 会改变关机销毁的行为（validation 层会要求用 present fence 而非 WaitIdle）

**不推荐作为首选方案**，但可以作为未来增强。

## 4. 其他同步问题审计

### 4.1 imageAvailable 信号量 — 按 frame index 索引 ✓

`imageAvailable[currentFrame_]` 在 `vkAcquireNextImageKHR` 中 signal，
在同一帧的 `vkQueueSubmit` 中 wait。每次使用前都经过了 `vkWaitForFences`，
确保上一次使用该 frame slot 的 submit 已完成。**安全**。

### 4.2 inFlight fence — 按 frame index 索引 ✓

`vkWaitForFences` → `vkResetFences` → `vkQueueSubmit(signal fence)`，
经典双缓冲 fence 模式。**安全**。

### 4.3 Subpass dependency

```cpp
dependency.srcStageMask = COLOR_ATTACHMENT_OUTPUT | LATE_FRAGMENT_TESTS;
dependency.dstStageMask = COLOR_ATTACHMENT_OUTPUT | EARLY_FRAGMENT_TESTS;
dependency.srcAccessMask = COLOR_ATTACHMENT_WRITE | DEPTH_STENCIL_ATTACHMENT_WRITE;
dependency.dstAccessMask = COLOR_ATTACHMENT_WRITE | DEPTH_STENCIL_ATTACHMENT_WRITE;
```

srcSubpass = EXTERNAL → dstSubpass = 0。
确保 acquire 的 image 在 color/depth 写入前可用。**正确**。

### 4.4 Swapchain 重建

```cpp
void Renderer::recreateSwapChain() {
    vkDeviceWaitIdle(device_->logicalDevice());   // 全停
    cleanupSwapChainResources();
    swapChain_->recreate();
    // ...
}
```

`vkDeviceWaitIdle` 确保所有 GPU 工作完成后再重建。
注意：严格按 Vulkan 规范，WaitIdle 不保证 present 完成（见 2.3），
但实际驱动中这是安全的，且 validation 层不会对此报错。**可接受**。

### 4.5 单次命令提交

```cpp
void Renderer::endSingleTimeCommands(VkCommandBuffer cmd) {
    vkQueueSubmit(graphicsQueue, ..., VK_NULL_HANDLE);   // 无 fence
    vkQueueWaitIdle(graphicsQueue);                       // 暴力等待
}
```

功能正确但效率较低。初始化阶段使用可以接受，
但如果将来在运行时频繁调用应改为异步+fence。**当前可接受**。

### 4.6 关机时序

```cpp
// Application::~Application()
vkDeviceWaitIdle(device_->logicalDevice());
// 然后 unique_ptr 按逆序析构

// Renderer::~Renderer()
vkDeviceWaitIdle(device_->logicalDevice());
// 销毁同步对象
```

双重 WaitIdle 保障。实际上 Application 的 WaitIdle 已经足够。**安全**。

## 5. 总结

| 问题 | 严重程度 | 状态 |
|------|---------|------|
| renderFinished 按 frame index 索引导致 present 信号量重用违规 | **高** — 违反 Vulkan spec，部分驱动/场景下会崩溃 | 需修复 |
| imageAvailable 索引方式 | 无 | ✓ 正确 |
| inFlight fence 索引方式 | 无 | ✓ 正确 |
| Subpass dependency | 无 | ✓ 正确 |
| 交换链重建同步 | 低 — 理论瑕疵，实际安全 | 可接受 |
| 单次命令 QueueWaitIdle | 低 — 性能，非正确性 | 可接受 |

**唯一需要修复的问题**：将 `renderFinished` 信号量从 `FrameData`（按 frame index 索引）
分离为独立数组（按 swapchain image index 索引），即本文方案 A。
