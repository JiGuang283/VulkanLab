# Step 3：VK_CHECK 宏替换

> 前置条件：Step 1（VulkanCheck.h 已创建）、Step 2 已完成
>
> 目标：将所有 `if (vkXxx(...) != VK_SUCCESS) throw std::runtime_error(...)` 模式统一替换为 `VK_CHECK(expr)`，消除大量重复的错误检查样板代码。

---

## 一、VK_CHECK 宏回顾

已在 Step 1 创建于 `src/core/VulkanCheck.h`：

```cpp
#define VK_CHECK(expr)                                                         \
    do {                                                                       \
        VkResult _vk_result = (expr);                                          \
        if (_vk_result != VK_SUCCESS) {                                        \
            throw std::runtime_error(                                          \
                std::string("Vulkan error ") +                                 \
                std::to_string(static_cast<int>(_vk_result)) + " at " +        \
                __FILE__ + ":" + std::to_string(__LINE__));                     \
        }                                                                      \
    } while (0)
```

优点：自动包含错误码数值和出错位置（文件名 + 行号），信息量优于手写字符串。

---

## 二、变更清单

### 2.1 总览

| 文件 | 简单替换数 | 复杂/跳过 | 说明 |
|------|-----------|----------|------|
| `core/VulkanContext.cpp` | 3 | 0 | createInstance + debugMessenger + createSurface |
| `core/Device.cpp` | 2 | 0 | createDevice + createAllocator |
| `core/Buffer.cpp` | 1 | 0 | vmaCreateBuffer |
| `core/Image.cpp` | 2 | 0 | vmaCreateImage + createImageView |
| `core/Pipeline.cpp` | 3 | 0 | shaderModule + pipelineLayout + pipeline |
| `core/SwapChain.cpp` | 2 | 0 | createSwapchain + createImageView |
| `render/Renderer.cpp` | 6 | 3 | 详见下方复杂模式分析 |
| `render/Material.cpp` | 3 | 0 | descriptorSetLayout + pool + sets |
| `render/Texture.cpp` | 1 | 0 | createSampler |
| **合计** | **23** | **3** | |

### 2.2 涉及文件的 include 变更

每个涉及替换的 `.cpp` 文件需新增：

```cpp
#include "core/VulkanCheck.h"
```

> 在 `core/` 目录下的文件可直接 `#include "VulkanCheck.h"`。

---

## 三、简单模式替换（23 处）

### 模式

**改前：**
```cpp
if (vkSomeCall(args...) != VK_SUCCESS) {
    throw std::runtime_error("failed to do something!");
}
```

**改后：**
```cpp
VK_CHECK(vkSomeCall(args...));
```

> 无花括号的单行 `if (...) throw ...;` 同样适用。

### 3.1 `core/VulkanContext.cpp`（3 处）

添加 `#include "VulkanCheck.h"`。

| 行号 | 原调用 | 替换 |
|------|--------|------|
| ~99 | `vkCreateInstance(...)` | `VK_CHECK(vkCreateInstance(&createInfo, nullptr, &instance_));` |
| ~111 | `CreateDebugUtilsMessengerEXT(...)` | `VK_CHECK(CreateDebugUtilsMessengerEXT(instance_, &createInfo, nullptr, &debugMessenger_));` |
| ~118 | `glfwCreateWindowSurface(...)` | `VK_CHECK(glfwCreateWindowSurface(instance_, window_, nullptr, &surface_));` |

### 3.2 `core/Device.cpp`（2 处）

添加 `#include "VulkanCheck.h"`。

| 行号 | 原调用 | 替换 |
|------|--------|------|
| ~171 | `vkCreateDevice(...)` | `VK_CHECK(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_));` |
| ~296 | `vmaCreateAllocator(...)` | `VK_CHECK(vmaCreateAllocator(&info, &allocator_));` |

### 3.3 `core/Buffer.cpp`（1 处）

添加 `#include "VulkanCheck.h"`。

| 行号 | 原调用 | 替换 |
|------|--------|------|
| ~18 | `vmaCreateBuffer(...)` | `VK_CHECK(vmaCreateBuffer(device.allocator(), &bufferInfo, &allocCI, &buffer_, &allocation_, nullptr));` |

### 3.4 `core/Image.cpp`（2 处）

添加 `#include "VulkanCheck.h"`。

| 行号 | 原调用 | 替换 |
|------|--------|------|
| ~27 | `vmaCreateImage(...)` | `VK_CHECK(vmaCreateImage(device.allocator(), &imageInfo, &allocCI, &image_, &allocation_, nullptr));` |
| ~78 | `vkCreateImageView(...)` | `VK_CHECK(vkCreateImageView(device_->logicalDevice(), &viewInfo, nullptr, &view_));` |

### 3.5 `core/Pipeline.cpp`（3 处）

添加 `#include "VulkanCheck.h"`。

| 行号 | 原调用 | 替换 |
|------|--------|------|
| ~34 | `vkCreateShaderModule(...)` | `VK_CHECK(vkCreateShaderModule(device_->logicalDevice(), &createInfo, nullptr, &shaderModule));` |
| ~172 | `vkCreatePipelineLayout(...)` | `VK_CHECK(vkCreatePipelineLayout(device_->logicalDevice(), &pipelineLayoutInfo, nullptr, &pipelineLayout_));` |
| ~196 | `vkCreateGraphicsPipelines(...)` | `VK_CHECK(vkCreateGraphicsPipelines(device_->logicalDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_));` |

### 3.6 `core/SwapChain.cpp`（2 处）

添加 `#include "VulkanCheck.h"`。

| 行号 | 原调用 | 替换 |
|------|--------|------|
| ~119 | `vkCreateSwapchainKHR(...)` | `VK_CHECK(vkCreateSwapchainKHR(device_->logicalDevice(), &createInfo, nullptr, &swapChain_));` |
| ~161 | `vkCreateImageView(...)` | `VK_CHECK(vkCreateImageView(device_->logicalDevice(), &viewInfo, nullptr, &imageView));` |

### 3.7 `render/Renderer.cpp`（6 处简单 + 3 处复杂另算）

添加 `#include "core/VulkanCheck.h"`。

**简单模式（6 处）：**

| 行号 | 原调用 | 替换 |
|------|--------|------|
| ~73 | `vkBeginCommandBuffer(...)` | `VK_CHECK(vkBeginCommandBuffer(frames_[currentFrame_].commandBuffer, &beginInfo));` |
| ~85 | `vkEndCommandBuffer(...)` | `VK_CHECK(vkEndCommandBuffer(frames_[currentFrame_].commandBuffer));` |
| ~107 | `vkQueueSubmit(...)` | `VK_CHECK(vkQueueSubmit(device_->graphicsQueue(), 1, &submitInfo, frames_[currentFrame_].inFlight));` |
| ~309 | `vkCreateRenderPass(...)` | `VK_CHECK(vkCreateRenderPass(device_->logicalDevice(), &renderPassInfo, nullptr, &renderPass_));` |
| ~325 | `vkCreateCommandPool(...)` | `VK_CHECK(vkCreateCommandPool(device_->logicalDevice(), &poolInfo, nullptr, &commandPool_));` |
| ~340 | `vkAllocateCommandBuffers(...)` | `VK_CHECK(vkAllocateCommandBuffers(device_->logicalDevice(), &allocInfo, buffers.data()));` |
| ~369 | `vkCreateFramebuffer(...)` | `VK_CHECK(vkCreateFramebuffer(device_->logicalDevice(), &framebufferInfo, nullptr, &framebuffers_[i]));` |

> 注：实际 7 处简单替换（含 createFramebuffer），连同 3 处复杂，Renderer.cpp 共有 10 处 `!= VK_SUCCESS`。

### 3.8 `render/Material.cpp`（3 处）

添加 `#include "core/VulkanCheck.h"`。

| 行号 | 原调用 | 替换 |
|------|--------|------|
| ~61 | `vkCreateDescriptorSetLayout(...)` | `VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &layoutInfo, nullptr, &descriptorSetLayout_));` |
| ~80 | `vkCreateDescriptorPool(...)` | `VK_CHECK(vkCreateDescriptorPool(device_->logicalDevice(), &poolInfo, nullptr, &descriptorPool_));` |
| ~96 | `vkAllocateDescriptorSets(...)` | `VK_CHECK(vkAllocateDescriptorSets(device_->logicalDevice(), &allocInfo, descriptorSets_.data()));` |

### 3.9 `render/Texture.cpp`（1 处）

添加 `#include "core/VulkanCheck.h"`。

| 行号 | 原调用 | 替换 |
|------|--------|------|
| ~97 | `vkCreateSampler(...)` | `VK_CHECK(vkCreateSampler(device_->logicalDevice(), &samplerInfo, nullptr, &sampler_));` |

---

## 四、复杂模式处理（3 处）

这 3 处位于 `render/Renderer.cpp`，因为需要对不同的 `VkResult` 值做多路分支判断，不适合直接用 `VK_CHECK` 替换。

### 4.1 `vkAcquireNextImageKHR`（Renderer.cpp ~55-65）

**保持不变**，因为需要检查 `VK_ERROR_OUT_OF_DATE_KHR` 和 `VK_SUBOPTIMAL_KHR`：

```cpp
VkResult result = vkAcquireNextImageKHR(d, swapChain_->handle(), UINT64_MAX,
                                        frames_[currentFrame_].imageAvailable,
                                        VK_NULL_HANDLE, &currentImageIndex_);
if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    recreateSwapChain();
    return VK_NULL_HANDLE;
} else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    throw std::runtime_error("failed to acquire swap chain image!");
}
```

> 理由：此处 `VK_SUBOPTIMAL_KHR` 和 `VK_ERROR_OUT_OF_DATE_KHR` 都不是错误，而是需要特殊处理的正常流程。

### 4.2 `vkQueuePresentKHR`（Renderer.cpp ~121-132）

**保持不变**，同理需要多路分支：

```cpp
VkResult result = vkQueuePresentKHR(device_->presentQueue(), &presentInfo);
if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
    framebufferResized_) {
    framebufferResized_ = false;
    recreateSwapChain();
} else if (result != VK_SUCCESS) {
    throw std::runtime_error("failed to present swap chain image!");
}
```

### 4.3 同步对象创建三联 `||`（Renderer.cpp ~388-394）

**改前：**
```cpp
if (vkCreateSemaphore(d, &semaphoreInfo, nullptr, &f.imageAvailable) !=
        VK_SUCCESS ||
    vkCreateSemaphore(d, &semaphoreInfo, nullptr, &f.renderFinished) !=
        VK_SUCCESS ||
    vkCreateFence(d, &fenceInfo, nullptr, &f.inFlight) != VK_SUCCESS) {
    throw std::runtime_error(
        "failed to create synchronization objects for a frame!");
}
```

**改后** — 拆为 3 个独立 `VK_CHECK`：
```cpp
VK_CHECK(vkCreateSemaphore(d, &semaphoreInfo, nullptr, &f.imageAvailable));
VK_CHECK(vkCreateSemaphore(d, &semaphoreInfo, nullptr, &f.renderFinished));
VK_CHECK(vkCreateFence(d, &fenceInfo, nullptr, &f.inFlight));
```

> 拆分后每个调用独立报错，更易定位哪个创建失败。

---

## 五、执行顺序

按依赖关系从底层到上层，逐批操作后编译验证。

### 批次 A — core 模块（6 文件，13 处）

| 序号 | 文件 | 操作 |
|------|------|------|
| A1 | `VulkanContext.cpp` | 添加 `#include "VulkanCheck.h"`，替换 3 处 |
| A2 | `Device.cpp` | 添加 `#include "VulkanCheck.h"`，替换 2 处 |
| A3 | `Buffer.cpp` | 添加 `#include "VulkanCheck.h"`，替换 1 处 |
| A4 | `Image.cpp` | 添加 `#include "VulkanCheck.h"`，替换 2 处 |
| A5 | `Pipeline.cpp` | 添加 `#include "VulkanCheck.h"`，替换 3 处 |
| A6 | `SwapChain.cpp` | 添加 `#include "VulkanCheck.h"`，替换 2 处 |

**编译验证 ✓**

### 批次 B — render 模块（3 文件，11 处）

| 序号 | 文件 | 操作 |
|------|------|------|
| B1 | `Renderer.cpp` | 添加 `#include "core/VulkanCheck.h"`，简单替换 7 处，拆分同步对象 1 处，保留 2 处复杂模式 |
| B2 | `Material.cpp` | 添加 `#include "core/VulkanCheck.h"`，替换 3 处 |
| B3 | `Texture.cpp` | 添加 `#include "core/VulkanCheck.h"`，替换 1 处 |

**编译验证 ✓**

---

## 六、验收标准

1. `grep -rn "!= VK_SUCCESS" src/` **仅命中以下 2 处**（Renderer.cpp 的 acquire/present 多路分支）和 `VulkanCheck.h` 宏定义自身：
   - `Renderer.cpp` — `vkAcquireNextImageKHR` 结果分支
   - `Renderer.cpp` — `vkQueuePresentKHR` 结果分支
   - `VulkanCheck.h` — 宏定义内部
2. 全量 clean build 通过（零 `error C` / `error LNK`）
3. 运行行为不变（渲染正常、resize 正常）

---

## 七、风险与注意事项

| 风险 | 应对 |
|------|------|
| `VK_CHECK` 吞掉了原有的自定义错误信息 | 宏会输出错误码 + 文件:行号，定位能力更强；如需保留原信息可扩展宏 |
| `glfwCreateWindowSurface` 返回 `VkResult` 但并非 Vulkan 核心函数 | GLFW 保证返回 `VK_SUCCESS` 或负值错误码，`VK_CHECK` 兼容 |
| `vmaCreateAllocator` / `vmaCreateBuffer` / `vmaCreateImage` 为 VMA 函数 | 返回 `VkResult`，与 `VK_CHECK` 完全兼容 |
| 同步对象 `||` 链拆分后，若第二个失败第一个已创建但未清理 | 与原代码行为一致（原代码也不清理已成功的），实践中构造函数失败直接 terminate 可接受 |
