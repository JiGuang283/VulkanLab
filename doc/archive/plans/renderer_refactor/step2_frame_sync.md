# 第二步：FrameSync 提取

## 目标

将帧同步逻辑（fence / semaphore / acquire / submit / present / command buffer 管理）从 `Renderer` 提取到新类 `FrameSync`。Renderer 瘦身为只管 RenderPass / Framebuffer / Color-Depth 资源。

## 前置状态

当前 `Renderer` 混合了两类职责：
- **帧同步**：`beginFrame()`（WaitFence → Acquire → ResetFence → BeginCmdBuf） / `endFrame()`（EndCmdBuf → Submit → Present）
- **渲染资源**：RenderPass、Framebuffer、Color/Depth Image、Viewport/Scissor、UBO

## 改动清单

### A. 新建 `src/core/FrameSync.h`

```cpp
#pragma once

#include <array>
#include <optional>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class SwapChain;

inline constexpr int MAX_FRAMES_IN_FLIGHT = 2;

class FrameSync {
  public:
    struct FrameContext {
        VkCommandBuffer cmd;
        uint32_t        frameIndex;  // 0..MAX_FRAMES_IN_FLIGHT-1
        uint32_t        imageIndex;  // swapchain image index
    };

    FrameSync(Device &device, SwapChain &swapChain);
    ~FrameSync();

    FrameSync(const FrameSync &) = delete;
    FrameSync &operator=(const FrameSync &) = delete;

    /// 返回 nullopt 表示本帧应跳过（窗口最小化或交换链需重建）
    std::optional<FrameContext> beginFrame();
    void                        endFrame(const FrameContext &ctx);

    void notifyResize() { framebufferResized_ = true; }

    /// Renderer 在 endFrame 之后查询，如果为 true 则执行交换链重建
    bool swapChainNeedsRecreation() const { return swapChainOutOfDate_; }
    void onSwapChainRecreated();

    // ---- 单次命令辅助（资源上传用）----
    VkCommandBuffer beginSingleTimeCommands();
    void            endSingleTimeCommands(VkCommandBuffer cmd);

    // ---- GPU 传输辅助 ----
    void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size);

    // ---- 访问器 ----
    VkCommandPool commandPool() const { return commandPool_; }

  private:
    struct PerFrame {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore     imageAvailable = VK_NULL_HANDLE;
        VkFence         inFlight = VK_NULL_HANDLE;
    };

    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();

    Device    *device_;
    SwapChain *swapChain_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;

    std::array<PerFrame, MAX_FRAMES_IN_FLIGHT> frames_;
    std::vector<VkSemaphore> renderFinished_;  // 按 swapchain image index 索引

    uint32_t currentFrame_ = 0;
    bool     framebufferResized_ = false;
    bool     swapChainOutOfDate_ = false;
};

} // namespace vkr
```

### B. 新建 `src/core/FrameSync.cpp`

从 `Renderer.cpp` 搬运以下函数（逻辑不变，只改归属）：

| 来源（Renderer 方法） | 目标（FrameSync 方法） | 改动 |
|---|---|---|
| `createCommandPool()` | `createCommandPool()` | 不变 |
| `createCommandBuffers()` | `createCommandBuffers()` | 不变 |
| `createSyncObjects()` | `createSyncObjects()` | 不变 |
| `beginFrame()` | `beginFrame()` → 返回 `optional<FrameContext>` | 不调用 `recreateSwapChain()`，改为设置 `swapChainOutOfDate_ = true` 并返回 `nullopt` |
| `endFrame()` | `endFrame(const FrameContext& ctx)` | 不调用 `recreateSwapChain()`，改为设置 `swapChainOutOfDate_ = true` |
| `beginSingleTimeCommands()` | `beginSingleTimeCommands()` | 不变 |
| `endSingleTimeCommands()` | `endSingleTimeCommands()` | 不变 |
| `copyBuffer()` | `copyBuffer()` | 不变 |
| 析构中的 semaphore/fence 释放 | 析构函数 | 不变 |

关键差异——**交换链重建不在 FrameSync 内部触发**：

```cpp
std::optional<FrameSync::FrameContext> FrameSync::beginFrame() {
    VkExtent2D ext = swapChain_->extent();
    if (ext.width == 0 || ext.height == 0)
        return std::nullopt;

    VkDevice d = device_->logicalDevice();
    vkWaitForFences(d, 1, &frames_[currentFrame_].inFlight, VK_TRUE, UINT64_MAX);

    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(
        d, swapChain_->handle(), UINT64_MAX,
        frames_[currentFrame_].imageAvailable, VK_NULL_HANDLE, &imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        swapChainOutOfDate_ = true;
        return std::nullopt;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        throw std::runtime_error("failed to acquire swap chain image!");

    vkResetFences(d, 1, &frames_[currentFrame_].inFlight);
    vkResetCommandBuffer(frames_[currentFrame_].commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(frames_[currentFrame_].commandBuffer, &beginInfo));

    return FrameContext{frames_[currentFrame_].commandBuffer, currentFrame_, imageIndex};
}

void FrameSync::endFrame(const FrameContext &ctx) {
    VkDevice d = device_->logicalDevice();
    VK_CHECK(vkEndCommandBuffer(ctx.cmd));

    // Submit
    VkSemaphore waitSems[]   = {frames_[ctx.frameIndex].imageAvailable};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signalSems[] = {renderFinished_[ctx.imageIndex]};

    VkSubmitInfo submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount   = 1;
    submitInfo.pWaitSemaphores      = waitSems;
    submitInfo.pWaitDstStageMask    = waitStages;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &ctx.cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = signalSems;

    VK_CHECK(vkQueueSubmit(device_->graphicsQueue(), 1, &submitInfo,
                           frames_[ctx.frameIndex].inFlight));

    // Present
    VkSwapchainKHR swapChains[] = {swapChain_->handle()};
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores    = signalSems;
    presentInfo.swapchainCount     = 1;
    presentInfo.pSwapchains        = swapChains;
    presentInfo.pImageIndices      = &ctx.imageIndex;

    VkResult result = vkQueuePresentKHR(device_->presentQueue(), &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
        framebufferResized_) {
        framebufferResized_ = false;
        swapChainOutOfDate_ = true;
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("failed to present swap chain image!");
    }

    currentFrame_ = (currentFrame_ + 1) % MAX_FRAMES_IN_FLIGHT;
}

void FrameSync::onSwapChainRecreated() {
    VkDevice d = device_->logicalDevice();
    for (auto sem : renderFinished_)
        vkDestroySemaphore(d, sem, nullptr);
    renderFinished_.clear();

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    renderFinished_.resize(swapChain_->imageCount());
    for (auto &sem : renderFinished_)
        VK_CHECK(vkCreateSemaphore(d, &semInfo, nullptr, &sem));

    swapChainOutOfDate_ = false;
}
```

### C. 修改 `Renderer.h` / `Renderer.cpp`

**移除的内容**：
- `struct FrameData`（整个）
- `std::array<FrameData, ...> frames_`
- `std::vector<VkSemaphore> renderFinishedSemaphores_`
- `VkCommandPool commandPool_`
- `uint32_t currentFrame_`, `currentImageIndex_`, `framebufferResized_`, `frameInProgress_`
- `MAX_FRAMES_IN_FLIGHT` 常量（移到 FrameSync.h）
- 方法：`beginFrame()`, `endFrame()`, `createCommandPool()`, `createCommandBuffers()`, `createSyncObjects()`, `beginSingleTimeCommands()`, `endSingleTimeCommands()`, `copyBuffer()`, `notifyResize()`

**保留的内容**：
- `createRenderPass()`
- `createFramebuffers()`, `cleanupSwapChainResources()`
- `createColorResources()`, `createDepthResources()`
- `createUniformBuffers()`
- `beginRenderPass()`, `endRenderPass()`
- `findDepthFormat()`, `findSupportedFormat()`
- `recreateSwapChain()`（内部不再管 sync 对象，只管 framebuffer / color / depth 重建）

**新的 Renderer**：

```cpp
// Renderer.h（瘦身后）
#pragma once
#include "core/Buffer.h"
#include "core/Device.h"
#include "core/Image.h"
#include "core/SwapChain.h"
#include "core/FrameSync.h"

#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Renderer {
  public:
    Renderer(Device &device, SwapChain &swapChain, FrameSync &frameSync,
             VkDeviceSize uniformBufferSize);
    ~Renderer();

    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    void beginRenderPass(VkCommandBuffer cmd, uint32_t imageIndex);
    void endRenderPass(VkCommandBuffer cmd);

    /// 交换链重建（由 Application 在帧循环中调用）
    void recreateSwapChain();

    VkRenderPass  renderPass() const { return renderPass_; }

    void        *mappedUniformBuffer(uint32_t frameIndex) const;
    VkBuffer     uniformBufferHandle(uint32_t frameIndex) const;
    VkDeviceSize uniformBufferSize() const { return uniformBufferSize_; }

  private:
    void createRenderPass();
    void createFramebuffers();
    void createColorResources();
    void createDepthResources();
    void createUniformBuffers();
    void cleanupSwapChainResources();

    VkFormat findDepthFormat();
    VkFormat findSupportedFormat(const std::vector<VkFormat> &candidates,
                                 VkImageTiling tiling,
                                 VkFormatFeatureFlags features);

    Device    *device_;
    SwapChain *swapChain_;
    FrameSync *frameSync_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
    std::unique_ptr<Image> colorImage_;
    std::unique_ptr<Image> depthImage_;

    // UBO（仍按 MAX_FRAMES_IN_FLIGHT 索引）
    std::vector<std::unique_ptr<Buffer>> uniformBuffers_;
    VkDeviceSize uniformBufferSize_ = 0;
};

} // namespace vkr
```

**注意**：`beginRenderPass` 签名变了——新增 `imageIndex` 参数（之前从内部成员 `currentImageIndex_` 取，现在由调用者传入）。

### D. 修改 `Application.h` / `Application.cpp`

```cpp
// Application.h — 新增 FrameSync 成员
#include "core/FrameSync.h"
// ...
std::unique_ptr<SwapChain>     swapChain_;
std::unique_ptr<FrameSync>     frameSync_;    // 新增，在 swapChain 后、renderer 前
std::unique_ptr<Renderer>      renderer_;
```

```cpp
// Application.cpp — init()
frameSync_ = std::make_unique<FrameSync>(*device_, *swapChain_);
renderer_  = std::make_unique<Renderer>(*device_, *swapChain_, *frameSync_,
                                         sizeof(GlobalUBO));

window_->setResizeCallback([this](int, int) { frameSync_->notifyResize(); });
```

```cpp
// Application.cpp — mainLoop() 帧循环
auto ctx = frameSync_->beginFrame();
if (!ctx) {
    // 交换链需要重建
    if (frameSync_->swapChainNeedsRecreation()) {
        renderer_->recreateSwapChain();
        frameSync_->onSwapChainRecreated();
        camera_.setAspect(static_cast<float>(swapChain_->extent().width) /
                          static_cast<float>(swapChain_->extent().height));
    }
    continue;
}

updateUniforms(ctx->frameIndex);

// ... update transforms ...

renderer_->beginRenderPass(ctx->cmd, ctx->imageIndex);
scene_.render(ctx->cmd, ctx->frameIndex);
renderer_->endRenderPass(ctx->cmd);

frameSync_->endFrame(*ctx);

// endFrame 之后也检查是否需要重建
if (frameSync_->swapChainNeedsRecreation()) {
    renderer_->recreateSwapChain();
    frameSync_->onSwapChainRecreated();
    camera_.setAspect(...);
}
```

### E. 修改 `Material`、`Texture`、`Mesh` 中对 Renderer 的依赖

这些类目前依赖 `Renderer` 的 `beginSingleTimeCommands()` / `endSingleTimeCommands()` / `copyBuffer()`。改为依赖 `FrameSync`：

| 文件 | 修改 |
|------|------|
| `Texture` 构造函数 | `Texture(Device&, FrameSync&, const std::string&)` — 替代之前的 `(Device&, Renderer&, ...)` |
| `Mesh` 构造函数 | `Mesh(Device&, FrameSync&, ...)` — 替代之前的 `(Device&, Renderer&, ...)` |
| `Mesh::fromOBJ()` | `fromOBJ(Device&, FrameSync&, const std::string&)` |
| `Material` 构造函数 | 仍需 `Renderer` 的 `renderPass()` 和 UBO 访问器，但不再需要命令辅助——可传 `FrameSync&` 获取 `commandPool` 等 |

### F. 修改 `Renderer.cpp` 中的 `recreateSwapChain()`

```cpp
void Renderer::recreateSwapChain() {
    vkDeviceWaitIdle(device_->logicalDevice());
    cleanupSwapChainResources();
    swapChain_->recreate();
    createColorResources();
    createDepthResources();
    createFramebuffers();
    // 不再管 renderFinished 信号量——FrameSync 负责
}
```

## 验证

1. **编译通过**：`cmake --build . --config Debug`
2. **运行**：渲染效果与之前完全一致
3. **Validation layer**：零 error、零 warning（除已知的 C4819 编码警告）
4. **窗口 resize**：拖动窗口大小正常重建交换链
5. **最小化**：最小化后恢复正常

## 文件变更总结

```
新增：src/core/FrameSync.h
新增：src/core/FrameSync.cpp
修改：src/render/Renderer.h        (大幅瘦身)
修改：src/render/Renderer.cpp      (移除帧同步、命令辅助代码)
修改：src/app/Application.h        (新增 frameSync_ 成员)
修改：src/app/Application.cpp      (帧循环改用 FrameSync)
修改：src/render/Texture.h/cpp     (构造函数依赖 FrameSync 替代 Renderer)
修改：src/render/Mesh.h/cpp        (同上)
修改：src/render/Material.h/cpp    (构造函数参数调整)
```
