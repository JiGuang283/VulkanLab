# P0：VMA 真正启用 — 详细计划

## 背景

`Device` 类已通过 `vmaCreateAllocator()` 创建了 `VmaAllocator`，并在析构中调用 `vmaDestroyAllocator()`。但 `Buffer` 和 `Image` 仍使用原始 Vulkan API 分配内存：

```
Buffer.cpp:  vkAllocateMemory → vkBindBufferMemory → vkFreeMemory
Image.cpp:   vkAllocateMemory → vkBindImageMemory  → vkFreeMemory
```

VMA 需要接管这两处，统一通过 `vmaCreateBuffer` / `vmaCreateImage` 完成资源创建 + 内存分配。

---

## 涉及文件清单

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `src/core/Buffer.h` | **修改** | `VkDeviceMemory` → `VmaAllocation`，移除 `memory()` 访问器，新增 `#include "vk_mem_alloc.h"` |
| `src/core/Buffer.cpp` | **修改** | 构造/析构/map/unmap/move 全部改为 VMA 调用 |
| `src/core/Image.h` | **修改** | `VkDeviceMemory` → `VmaAllocation`，移除 `memory()` 访问器，新增 `#include "vk_mem_alloc.h"` |
| `src/core/Image.cpp` | **修改** | 构造/析构/move 全部改为 VMA 调用 |
| `src/core/Device.h` | **不变** | `allocator()` 访问器已存在 |
| `src/core/Device.cpp` | **可选** | `findMemoryType()` 迁移完成后可标 deprecated 或移除 |

**不需要改动的文件**：经全局搜索确认，无任何外部代码调用 `Buffer::memory()` 或 `Image::memory()`，迁移不影响外部接口。

---

## 步骤 1：修改 Buffer

### Buffer.h 改动

```cpp
// ---- 替换前 ----
#pragma once
#include <vulkan/vulkan.h>

namespace vkr {
class Device;

class Buffer {
  public:
    Buffer(Device &device, VkDeviceSize size, VkBufferUsageFlags usage,
           VkMemoryPropertyFlags memProps);
    // ...
    VkBuffer       handle() const { return buffer_; }
    VkDeviceMemory memory() const { return memory_; }     // 删除
    VkDeviceSize   size() const { return size_; }
    // ...
  private:
    Device        *device_ = nullptr;
    VkBuffer       buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;              // 替换
    VkDeviceSize   size_ = 0;
    void          *mapped_ = nullptr;
};
```

```cpp
// ---- 替换后 ----
#pragma once
#include <vulkan/vulkan.h>
#include "vk_mem_alloc.h"

namespace vkr {
class Device;

class Buffer {
  public:
    Buffer(Device &device, VkDeviceSize size, VkBufferUsageFlags usage,
           VkMemoryPropertyFlags memProps);   // 签名不变，内部转为 VMA usage
    // ...
    VkBuffer     handle() const { return buffer_; }
    // memory() 移除 —— 外部无调用
    VkDeviceSize size() const { return size_; }
    // ...
  private:
    Device        *device_ = nullptr;
    VkBuffer       buffer_ = VK_NULL_HANDLE;
    VmaAllocation  allocation_ = VK_NULL_HANDLE;   // 替代 VkDeviceMemory
    VkDeviceSize   size_ = 0;
    void          *mapped_ = nullptr;
};
```

### Buffer.cpp 改动

#### 构造函数

```cpp
// ---- 替换前 ----
Buffer::Buffer(Device &device, VkDeviceSize size, VkBufferUsageFlags usage,
               VkMemoryPropertyFlags memProps)
    : device_(&device), size_(size) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device.logicalDevice(), &bufferInfo, nullptr,
                       &buffer_) != VK_SUCCESS)
        throw std::runtime_error("failed to create buffer!");

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device.logicalDevice(), buffer_, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex =
        device.findMemoryType(memReq.memoryTypeBits, memProps);

    if (vkAllocateMemory(device.logicalDevice(), &allocInfo, nullptr,
                         &memory_) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate buffer memory!");

    vkBindBufferMemory(device.logicalDevice(), buffer_, memory_, 0);
}
```

```cpp
// ---- 替换后 ----
Buffer::Buffer(Device &device, VkDeviceSize size, VkBufferUsageFlags usage,
               VkMemoryPropertyFlags memProps)
    : device_(&device), size_(size) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocCI{};
    // 将 VkMemoryPropertyFlags 转为 VMA 的 requiredFlags
    allocCI.requiredFlags = memProps;

    if (vmaCreateBuffer(device.allocator(), &bufferInfo, &allocCI,
                        &buffer_, &allocation_, nullptr) != VK_SUCCESS)
        throw std::runtime_error("failed to create buffer with VMA!");
}
```

**要点**：
- `vmaCreateBuffer` 一次调用完成 `vkCreateBuffer` + `vkAllocateMemory` + `vkBindBufferMemory`
- `VmaAllocationCreateInfo::requiredFlags` 接受 `VkMemoryPropertyFlags`（如 `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT`），VMA 内部自动选择合适的 memory type
- 不再需要手动 `vkGetBufferMemoryRequirements` 和 `findMemoryType`

#### 析构 / cleanup

```cpp
// ---- 替换前 ----
void Buffer::cleanup() {
    if (device_ && buffer_ != VK_NULL_HANDLE) {
        if (mapped_) unmap();
        vkDestroyBuffer(device_->logicalDevice(), buffer_, nullptr);
        vkFreeMemory(device_->logicalDevice(), memory_, nullptr);
        buffer_ = VK_NULL_HANDLE;
        memory_ = VK_NULL_HANDLE;
    }
}
```

```cpp
// ---- 替换后 ----
void Buffer::cleanup() {
    if (device_ && buffer_ != VK_NULL_HANDLE) {
        if (mapped_) unmap();
        vmaDestroyBuffer(device_->allocator(), buffer_, allocation_);
        buffer_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
    }
}
```

**要点**：`vmaDestroyBuffer` 一次调用同时销毁 buffer 和释放内存。

#### map / unmap

```cpp
// ---- 替换前 ----
void *Buffer::map() {
    if (!mapped_) {
        vkMapMemory(device_->logicalDevice(), memory_, 0, size_, 0, &mapped_);
    }
    return mapped_;
}
void Buffer::unmap() {
    if (mapped_) {
        vkUnmapMemory(device_->logicalDevice(), memory_);
        mapped_ = nullptr;
    }
}
```

```cpp
// ---- 替换后 ----
void *Buffer::map() {
    if (!mapped_) {
        vmaMapMemory(device_->allocator(), allocation_, &mapped_);
    }
    return mapped_;
}
void Buffer::unmap() {
    if (mapped_) {
        vmaUnmapMemory(device_->allocator(), allocation_);
        mapped_ = nullptr;
    }
}
```

#### move 构造 / move 赋值

将所有 `memory_` 替换为 `allocation_`，逻辑不变：

```cpp
Buffer::Buffer(Buffer &&other) noexcept
    : device_(other.device_), buffer_(other.buffer_),
      allocation_(other.allocation_),       // 替换 memory_
      size_(other.size_), mapped_(other.mapped_) {
    other.device_ = nullptr;
    other.buffer_ = VK_NULL_HANDLE;
    other.allocation_ = VK_NULL_HANDLE;     // 替换 memory_
    other.size_ = 0;
    other.mapped_ = nullptr;
}

Buffer &Buffer::operator=(Buffer &&other) noexcept {
    if (this != &other) {
        cleanup();
        device_ = other.device_;
        buffer_ = other.buffer_;
        allocation_ = other.allocation_;    // 替换 memory_
        size_ = other.size_;
        mapped_ = other.mapped_;
        other.device_ = nullptr;
        other.buffer_ = VK_NULL_HANDLE;
        other.allocation_ = VK_NULL_HANDLE; // 替换 memory_
        other.size_ = 0;
        other.mapped_ = nullptr;
    }
    return *this;
}
```

---

## 步骤 2：修改 Image

### Image.h 改动

```cpp
// ---- 替换前 ----
    VkImage        handle() const { return image_; }
    VkDeviceMemory memory() const { return memory_; }     // 删除
    VkImageView    imageView() const { return view_; }
  private:
    Device        *device_ = nullptr;
    VkImage        image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;              // 替换
    VkImageView    view_ = VK_NULL_HANDLE;
```

```cpp
// ---- 替换后 ----
    VkImage     handle() const { return image_; }
    // memory() 移除 —— 外部无调用
    VkImageView imageView() const { return view_; }
  private:
    Device       *device_ = nullptr;
    VkImage       image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;           // 替代
    VkImageView   view_ = VK_NULL_HANDLE;
```

### Image.cpp 改动

#### 构造函数

```cpp
// ---- 替换前 ----
Image::Image(Device &device, uint32_t width, uint32_t height,
             uint32_t mipLevels, VkSampleCountFlagBits samples, VkFormat format,
             VkImageTiling tiling, VkImageUsageFlags usage,
             VkMemoryPropertyFlags memProps)
    : device_(&device) {
    VkImageCreateInfo imageInfo{};
    // ... imageInfo 填充 ...

    if (vkCreateImage(device.logicalDevice(), &imageInfo, nullptr, &image_) !=
        VK_SUCCESS)
        throw std::runtime_error("failed to create image!");

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device.logicalDevice(), image_, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex =
        device.findMemoryType(memReq.memoryTypeBits, memProps);

    if (vkAllocateMemory(device.logicalDevice(), &allocInfo, nullptr,
                         &memory_) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate image memory!");

    vkBindImageMemory(device.logicalDevice(), image_, memory_, 0);
}
```

```cpp
// ---- 替换后 ----
Image::Image(Device &device, uint32_t width, uint32_t height,
             uint32_t mipLevels, VkSampleCountFlagBits samples, VkFormat format,
             VkImageTiling tiling, VkImageUsageFlags usage,
             VkMemoryPropertyFlags memProps)
    : device_(&device) {
    VkImageCreateInfo imageInfo{};
    // ... imageInfo 填充保持不变 ...

    VmaAllocationCreateInfo allocCI{};
    allocCI.requiredFlags = memProps;

    if (vmaCreateImage(device.allocator(), &imageInfo, &allocCI,
                       &image_, &allocation_, nullptr) != VK_SUCCESS)
        throw std::runtime_error("failed to create image with VMA!");
}
```

#### 析构 / cleanup

```cpp
// ---- 替换前 ----
void Image::cleanup() {
    if (device_) {
        VkDevice d = device_->logicalDevice();
        if (view_ != VK_NULL_HANDLE)
            vkDestroyImageView(d, view_, nullptr);
        if (image_ != VK_NULL_HANDLE)
            vkDestroyImage(d, image_, nullptr);
        if (memory_ != VK_NULL_HANDLE)
            vkFreeMemory(d, memory_, nullptr);
        view_ = VK_NULL_HANDLE;
        image_ = VK_NULL_HANDLE;
        memory_ = VK_NULL_HANDLE;
    }
}
```

```cpp
// ---- 替换后 ----
void Image::cleanup() {
    if (device_) {
        VkDevice d = device_->logicalDevice();
        if (view_ != VK_NULL_HANDLE)
            vkDestroyImageView(d, view_, nullptr);
        if (image_ != VK_NULL_HANDLE)
            vmaDestroyImage(device_->allocator(), image_, allocation_);
        view_ = VK_NULL_HANDLE;
        image_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
    }
}
```

**注意**：`VkImageView` 仍用 `vkDestroyImageView` 销毁，VMA 只管 `VkImage` + 内存。

#### move 构造 / move 赋值

同 Buffer，`memory_` 全部替换为 `allocation_`。

---

## 步骤 3：清理 Device::findMemoryType

迁移完成后，`findMemoryType` 不再被任何代码调用。

**方案**：直接移除。

```diff
- // Device.h
- uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props) const;

- // Device.cpp
- uint32_t Device::findMemoryType(...) const { ... }
```

如果后续某些场景仍需手动查询 memory type（极少见），可再加回。

---

## 步骤 4：编译验证

```powershell
# Release
cd c:\Project\vulkan_learn\build
cmake ..
cmake --build . --config Release

# Debug
cd c:\Project\vulkan_learn\build-debug
cmake ..
cmake --build . --config Debug
```

---

## 步骤 5：运行时验证

```powershell
cd c:\Project\vulkan_learn\build\Release
.\VulkanLab.exe
```

确认：
1. 程序正常启动，渲染画面与之前一致
2. WASD + 鼠标 FPS 控制正常
3. 窗口 resize 不崩溃
4. ESC 正常退出，无验证层报错

---

## 步骤 6：全局搜索确认

```powershell
Select-String -Path "src\**\*.cpp","src\**\*.h" -Pattern "vkAllocateMemory|vkFreeMemory|vkBindBufferMemory|vkBindImageMemory" -Recurse
```

**期望结果**：零匹配。

```powershell
Select-String -Path "src\**\*.cpp","src\**\*.h" -Pattern "findMemoryType" -Recurse
```

**期望结果**：零匹配（如果步骤 3 已移除）。

---

## API 对照表

| 操作 | 替换前（原始 Vulkan） | 替换后（VMA） |
|------|---------------------|--------------|
| 创建 Buffer + 分配内存 | `vkCreateBuffer` + `vkGetBufferMemoryRequirements` + `vkAllocateMemory` + `vkBindBufferMemory` (4 调用) | `vmaCreateBuffer` (1 调用) |
| 销毁 Buffer + 释放内存 | `vkDestroyBuffer` + `vkFreeMemory` (2 调用) | `vmaDestroyBuffer` (1 调用) |
| 创建 Image + 分配内存 | `vkCreateImage` + `vkGetImageMemoryRequirements` + `vkAllocateMemory` + `vkBindImageMemory` (4 调用) | `vmaCreateImage` (1 调用) |
| 销毁 Image + 释放内存 | `vkDestroyImage` + `vkFreeMemory` (2 调用) | `vmaDestroyImage` (1 调用) |
| 映射内存 | `vkMapMemory` | `vmaMapMemory` |
| 取消映射 | `vkUnmapMemory` | `vmaUnmapMemory` |
| 选择 memory type | `findMemoryType()` 手动查询 | VMA 内部自动处理 |

---

## 风险与注意事项

1. **构造函数签名不变**：`Buffer` 和 `Image` 的构造参数保持 `VkMemoryPropertyFlags memProps`，内部转为 `VmaAllocationCreateInfo::requiredFlags`，外部调用者（Renderer、Texture、Mesh）无需任何修改

2. **VMA 内部调用 `vkAllocateMemory`**：搜索验证时，不要搜到 `external/vma/vk_mem_alloc.h` 里的代码，只检查 `src/` 下的文件

3. **VMA 的 `VK_API_VERSION`**：当前 `createAllocator` 中设为 `VK_API_VERSION_1_0`，这是正确的（项目使用 Vulkan 1.0）

4. **持久映射**：VMA 支持 `VMA_ALLOCATION_CREATE_MAPPED_BIT` 在创建时直接映射，当前不使用此特性，保持手动 map/unmap 行为不变

5. **调试**：VMA 支持统计输出（`vmaBuildStatsString`），可在后续调试时启用，本次不引入
