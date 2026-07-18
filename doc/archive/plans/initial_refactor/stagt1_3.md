## 阶段 1.3 Buffer / Image RAII — 具体操作步骤

### 前置分析

**需要用 RAII 包装的成员变量：**

| 资源类型 | 当前成员变量 | 所在文件 |
|---------|------------|---------|
| 顶点缓冲 | `vertexBuffer` + `vertexBufferMemory` | vulkan_vertex.cpp |
| 索引缓冲 | `indexBuffer` + `indexBufferMemory` | vulkan_vertex.cpp |
| Uniform 缓冲 | `uniformBuffers[]` + `uniformBuffersMemory[]` + `uniformBuffersMapped[]` | vulkan_uniform.cpp |
| Staging 缓冲 | 局部变量，函数作用域内创建销毁 | vulkan_vertex.cpp, vulkan_texture.cpp |
| 纹理图像 | `textureImage` + `textureImageMemory` + `textureImageView` | vulkan_texture.cpp |
| 深度图像 | `depthImage` + `depthImageMemory` + `depthImageView` | vulkan_depth.cpp |
| MSAA 颜色图像 | `colorImage` + `colorImageMemory` + `colorImageView` | vulkan_msaa.cpp |

**暂不搬移的方法**（依赖 `commandPool`，等阶段 2 CommandManager 再搬）：
- `beginSingleTimeCommands()` / `endSingleTimeCommands()`
- `transitionImageLayout()` / `copyBufferToImage()` / `copyBuffer()`
- `generateMipmaps()`

**设计原则**：1.3 只做 RAII 所有权封装，不改变分配策略（暂用 raw Vulkan，VMA 替换留到后续）。

---

### 步骤 1：创建 `src/core/Buffer.h`

```cpp
#pragma once
#include <vulkan/vulkan.h>

namespace vkr {

class Device;

class Buffer {
public:
    Buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage,
           VkMemoryPropertyFlags memProps);
    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    // 移动语义（staging buffer 需要作为局部变量返回或赋值）
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    VkBuffer       handle() const { return buffer_; }
    VkDeviceMemory memory() const { return memory_; }
    VkDeviceSize   size()   const { return size_; }

    // 映射/取消映射（用于 host-visible 缓冲如 Uniform、Staging）
    void* map();
    void  unmap();
    void* mappedData() const { return mapped_; }

private:
    void cleanup();

    Device*        device_ = nullptr;  // 非拥有指针
    VkBuffer       buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize   size_   = 0;
    void*          mapped_ = nullptr;
};

} // namespace vkr
```

**要点**：
- **必须支持移动语义**：staging buffer 作为局部 RAII 对象，作用域结束自动销毁；如果不支持移动，将来无法放入 `std::vector` 或从函数返回
- `map()` / `unmap()`：Uniform buffer 需要 persistent mapping，staging buffer 需要临时映射写入数据
- `device_` 用裸指针而非引用，是为了支持移动语义（移动后源对象的 `device_` 置空）

---

### 步骤 2：创建 `src/core/Buffer.cpp`

将 App 中 `createBuffer()` 的逻辑搬入构造函数：

```cpp
#include "Buffer.h"
#include "Device.h"
#include <stdexcept>

namespace vkr {

Buffer::Buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage,
               VkMemoryPropertyFlags memProps)
    : device_(&device), size_(size)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device.logicalDevice(), &bufferInfo, nullptr, &buffer_) != VK_SUCCESS)
        throw std::runtime_error("failed to create buffer!");

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device.logicalDevice(), buffer_, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = device.findMemoryType(memReq.memoryTypeBits, memProps);

    if (vkAllocateMemory(device.logicalDevice(), &allocInfo, nullptr, &memory_) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate buffer memory!");

    vkBindBufferMemory(device.logicalDevice(), buffer_, memory_, 0);
}

Buffer::~Buffer() { cleanup(); }

Buffer::Buffer(Buffer&& other) noexcept
    : device_(other.device_), buffer_(other.buffer_), memory_(other.memory_),
      size_(other.size_), mapped_(other.mapped_)
{
    other.device_ = nullptr;
    other.buffer_ = VK_NULL_HANDLE;
    other.memory_ = VK_NULL_HANDLE;
    other.mapped_ = nullptr;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        cleanup();
        device_ = other.device_;
        buffer_ = other.buffer_;
        memory_ = other.memory_;
        size_   = other.size_;
        mapped_ = other.mapped_;
        other.device_ = nullptr;
        other.buffer_ = VK_NULL_HANDLE;
        other.memory_ = VK_NULL_HANDLE;
        other.mapped_ = nullptr;
    }
    return *this;
}

void* Buffer::map() {
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

void Buffer::cleanup() {
    if (device_ && buffer_ != VK_NULL_HANDLE) {
        if (mapped_) unmap();
        vkDestroyBuffer(device_->logicalDevice(), buffer_, nullptr);
        vkFreeMemory(device_->logicalDevice(), memory_, nullptr);
        buffer_ = VK_NULL_HANDLE;
        memory_ = VK_NULL_HANDLE;
    }
}

} // namespace vkr
```

---

### 步骤 3：创建 `src/core/Image.h`

```cpp
#pragma once
#include <vulkan/vulkan.h>

namespace vkr {

class Device;

class Image {
public:
    Image(Device& device, uint32_t width, uint32_t height, uint32_t mipLevels,
          VkSampleCountFlagBits samples, VkFormat format, VkImageTiling tiling,
          VkImageUsageFlags usage, VkMemoryPropertyFlags memProps);
    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;

    VkImage        handle()    const { return image_; }
    VkDeviceMemory memory()    const { return memory_; }
    VkImageView    imageView() const { return view_; }

    // 为 Image 创建 ImageView（可多次调用不同参数，但只保留最后一个）
    void createView(VkFormat format, VkImageAspectFlags aspectFlags, uint32_t mipLevels);

private:
    void cleanup();

    Device*        device_ = nullptr;
    VkImage        image_  = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView    view_   = VK_NULL_HANDLE;
};

} // namespace vkr
```

**要点**：
- `createView()` 独立于构造函数，因为有些 Image（如纹理）先创建 Image → 上传数据 → 生成 Mipmap → 最后才创建 View
- 交换链的 Image 是 Vulkan 创建的，**不要**用这个类包装，交换链的 ImageView 保持原有方式创建

---

### 步骤 4：创建 `src/core/Image.cpp`

将 App 中 `createImage()` + `createImageView()` 的逻辑搬入：

```cpp
#include "Image.h"
#include "Device.h"
#include <stdexcept>

namespace vkr {

Image::Image(Device& device, uint32_t width, uint32_t height, uint32_t mipLevels,
             VkSampleCountFlagBits samples, VkFormat format, VkImageTiling tiling,
             VkImageUsageFlags usage, VkMemoryPropertyFlags memProps)
    : device_(&device)
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = samples;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device.logicalDevice(), &imageInfo, nullptr, &image_) != VK_SUCCESS)
        throw std::runtime_error("failed to create image!");

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device.logicalDevice(), image_, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = device.findMemoryType(memReq.memoryTypeBits, memProps);

    if (vkAllocateMemory(device.logicalDevice(), &allocInfo, nullptr, &memory_) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate image memory!");

    vkBindImageMemory(device.logicalDevice(), image_, memory_, 0);
}

Image::~Image() { cleanup(); }

// 移动构造/赋值同 Buffer 模式 ...

void Image::createView(VkFormat format, VkImageAspectFlags aspectFlags, uint32_t mipLevels) {
    if (view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_->logicalDevice(), view_, nullptr);
        view_ = VK_NULL_HANDLE;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_->logicalDevice(), &viewInfo, nullptr, &view_) != VK_SUCCESS)
        throw std::runtime_error("failed to create image view!");
}

void Image::cleanup() {
    if (device_) {
        VkDevice d = device_->logicalDevice();
        if (view_ != VK_NULL_HANDLE) vkDestroyImageView(d, view_, nullptr);
        if (image_ != VK_NULL_HANDLE) vkDestroyImage(d, image_, nullptr);
        if (memory_ != VK_NULL_HANDLE) vkFreeMemory(d, memory_, nullptr);
        view_   = VK_NULL_HANDLE;
        image_  = VK_NULL_HANDLE;
        memory_ = VK_NULL_HANDLE;
    }
}

} // namespace vkr
```

---

### 步骤 5：修改 app.h — 替换成员变量

```cpp
#include "core/Buffer.h"
#include "core/Image.h"
```

**替换 Buffer 成员：**

| 删除 | 替换为 |
|------|--------|
| `VkBuffer vertexBuffer;` + `VkDeviceMemory vertexBufferMemory;` | `std::unique_ptr<vkr::Buffer> vertexBuffer_;` |
| `VkBuffer indexBuffer;` + `VkDeviceMemory indexBufferMemory;` | `std::unique_ptr<vkr::Buffer> indexBuffer_;` |
| `std::vector<VkBuffer> uniformBuffers;` + `std::vector<VkDeviceMemory> uniformBuffersMemory;` + `std::vector<void*> uniformBuffersMapped;` | `std::vector<std::unique_ptr<vkr::Buffer>> uniformBuffers_;` |

**替换 Image 成员：**

| 删除 | 替换为 |
|------|--------|
| `VkImage textureImage;` + `VkDeviceMemory textureImageMemory;` + `VkImageView textureImageView;` | `std::unique_ptr<vkr::Image> textureImage_;` |
| `VkImage depthImage;` + `VkDeviceMemory depthImageMemory;` + `VkImageView depthImageView;` | `std::unique_ptr<vkr::Image> depthImage_;` |
| `VkImage colorImage;` + `VkDeviceMemory colorImageMemory;` + `VkImageView colorImageView;` | `std::unique_ptr<vkr::Image> colorImage_;` |

> 用 `unique_ptr` 是因为这些资源延迟初始化（不在构造时创建），且 Image/Buffer 支持移动但 `unique_ptr` 更明确表达"可选、延迟创建"的语义。

**删除已搬走的方法声明：**
```cpp
// 删除
void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags properties, VkBuffer &buffer,
                  VkDeviceMemory &bufferMemory);
void createImage(uint32_t width, uint32_t height, uint32_t mipLevels,
                 VkSampleCountFlagBits numSamples, VkFormat format,
                 VkImageTiling tiling, VkImageUsageFlags usage,
                 VkMemoryPropertyFlags properties, VkImage &image,
                 VkDeviceMemory &imageMemory);
VkImageView createImageView(VkImage image, VkFormat format,
                            VkImageAspectFlags aspectFlags, uint32_t mipLevels);
```

> **注意**：`createImageView` 在 `createImageViews()`（交换链）中仍被使用。交换链的 Image 是 Vulkan 拥有的，不能用 `vkr::Image` 包装。有两个选择：
> - **方案 A**：保留 `createImageView` 作为 App 的方法，仅供交换链使用
> - **方案 B**：把它改为自由函数或 Device 的方法
>
> **建议方案 A**（改动最小），等阶段 1.4 SwapChain RAII 时再处理。

---

### 步骤 6：逐文件更新使用方 — Buffer 相关

#### 6.1 vulkan_vertex.cpp

**`createVertexBuffer()`**：
```cpp
void HelloTriangleApplication::createVertexBuffer() {
    VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();

    // staging buffer 作为局部 RAII 对象
    vkr::Buffer staging(*device, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    void* data = staging.map();
    memcpy(data, vertices.data(), bufferSize);
    staging.unmap();

    vertexBuffer_ = std::make_unique<vkr::Buffer>(
        *device, bufferSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    copyBuffer(staging.handle(), vertexBuffer_->handle(), bufferSize);
    // staging 自动析构销毁
}
```

**`createIndexBuffer()`**：同上模式。

**删除**：`createBuffer()` 方法实现（已搬入 `Buffer` 构造函数）。

**保留**：`beginSingleTimeCommands()`、`endSingleTimeCommands()`、`copyBuffer()` — 它们依赖 `commandPool`，等阶段 2 搬到 CommandManager。

#### 6.2 vulkan_uniform.cpp

**`createUniformBuffers()`**：
```cpp
void HelloTriangleApplication::createUniformBuffers() {
    VkDeviceSize bufferSize = sizeof(UniformBufferObject);
    uniformBuffers_.resize(MAX_FRAMES_IN_FLIGHT);

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        uniformBuffers_[i] = std::make_unique<vkr::Buffer>(
            *device, bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        uniformBuffers_[i]->map();  // persistent mapping
    }
}
```

**`updateUniformBuffer()`**：
```cpp
memcpy(uniformBuffers_[currentImage]->mappedData(), &ubo, sizeof(ubo));
```

**`createDescriptorSets()`**：描述符集中 `bufferInfo.buffer` 改为 `uniformBuffers_[i]->handle()`。

---

### 步骤 7：逐文件更新使用方 — Image 相关

#### 7.1 vulkan_texture.cpp

**`createTextureImage()`**：
```cpp
void HelloTriangleApplication::createTextureImage() {
    // ... 加载像素 ...

    vkr::Buffer staging(*device, imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    void* data = staging.map();
    memcpy(data, pixels, imageSize);
    staging.unmap();
    stbi_image_free(pixels);

    textureImage_ = std::make_unique<vkr::Image>(
        *device, texWidth, texHeight, mipLevels, VK_SAMPLE_COUNT_1_BIT,
        VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    transitionImageLayout(textureImage_->handle(), ...);
    copyBufferToImage(staging.handle(), textureImage_->handle(), texWidth, texHeight);
    generateMipmaps(textureImage_->handle(), ...);
}
```

**`createTextureImageView()`**：
```cpp
void HelloTriangleApplication::createTextureImageView() {
    textureImage_->createView(VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);
}
```

> 删除 `createImage()` 方法实现（已搬入 `Image` 构造函数）。
> 保留 `createImageView()` 仅用于交换链 ImageView。

#### 7.2 vulkan_depth.cpp

```cpp
void HelloTriangleApplication::createDepthResources() {
    VkFormat depthFormat = findDepthFormat();

    depthImage_ = std::make_unique<vkr::Image>(
        *device, swapChainExtent.width, swapChainExtent.height, 1,
        device->msaaSamples(), depthFormat, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    depthImage_->createView(depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1);

    transitionImageLayout(depthImage_->handle(), depthFormat,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 1);
}
```

#### 7.3 vulkan_msaa.cpp

```cpp
void HelloTriangleApplication::createColorResources() {
    VkFormat colorFormat = swapChainImageFormat;

    colorImage_ = std::make_unique<vkr::Image>(
        *device, swapChainExtent.width, swapChainExtent.height, 1,
        device->msaaSamples(), colorFormat, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    colorImage_->createView(colorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1);
}
```

---

### 步骤 8：更新所有引用旧成员变量的地方

以下地方使用了旧的裸变量名，需要改为 RAII 访问器：

| 旧写法 | 新写法 | 涉及文件 |
|--------|--------|---------|
| `vertexBuffer` | `vertexBuffer_->handle()` | vulkan_drawframe.cpp (`recordCommandBuffer`) |
| `indexBuffer` | `indexBuffer_->handle()` | vulkan_drawframe.cpp |
| `textureImageView` | `textureImage_->imageView()` | vulkan_uniform.cpp (`createDescriptorSets`) |
| `depthImageView` | `depthImage_->imageView()` | vulkan_framebuffers.cpp |
| `colorImageView` | `colorImage_->imageView()` | vulkan_framebuffers.cpp |
| `uniformBuffers[i]` | `uniformBuffers_[i]->handle()` | vulkan_uniform.cpp |

---

### 步骤 9：更新 `cleanup()` 和 `cleanupSwapChain()`

**`cleanup()` 中删除：**
```cpp
// 删除这些，Buffer/Image 析构函数自动处理
vkDestroyBuffer(d, uniformBuffers[i], nullptr);
vkFreeMemory(d, uniformBuffersMemory[i], nullptr);
vkDestroyImage(d, textureImage, nullptr);
vkFreeMemory(d, textureImageMemory, nullptr);
vkDestroyImageView(d, textureImageView, nullptr);
vkDestroyBuffer(d, indexBuffer, nullptr);
vkFreeMemory(d, indexBufferMemory, nullptr);
vkDestroyBuffer(d, vertexBuffer, nullptr);
vkFreeMemory(d, vertexBufferMemory, nullptr);
```

替换为：
```cpp
uniformBuffers_.clear();
textureImage_.reset();
vertexBuffer_.reset();
indexBuffer_.reset();
```

**`cleanupSwapChain()` 中删除：**
```cpp
// 删除这些
vkDestroyImageView(d, colorImageView, nullptr);
vkDestroyImage(d, colorImage, nullptr);
vkFreeMemory(d, colorImageMemory, nullptr);
vkDestroyImageView(d, depthImageView, nullptr);
vkDestroyImage(d, depthImage, nullptr);
vkFreeMemory(d, depthImageMemory, nullptr);
```

替换为：
```cpp
colorImage_.reset();
depthImage_.reset();
```

> **注意销毁顺序**：`colorImage_` 和 `depthImage_` 必须在 framebuffers 销毁之后再销毁（当前代码中 framebuffers 先销毁，顺序没问题）。

---

### 步骤 10：更新 CMakeLists.txt

确保 `src/core/Buffer.cpp` 和 `src/core/Image.cpp` 加入编译。

---

### 步骤 11：编译验证

1. Debug / Release 编译通过
2. 运行渲染结果不变
3. `cleanup()` 中不再有 `vkDestroyBuffer` / `vkFreeMemory` / `vkDestroyImage` / `vkDestroyImageView`（纹理/深度/颜色/顶点/索引/uniform 相关的）
4. 验证层无报错

---

### 需要注意的坑

1. **交换链 ImageView 不要用 `vkr::Image` 包装** — 交换链的 `VkImage` 由 Vulkan 拥有和销毁，我们只创建了 `VkImageView`。在 `createImageViews()` 中保留旧的 `createImageView()` 调用方式。等阶段 1.4 SwapChain RAII 再统一处理。

2. **移动语义不可省略** — staging buffer 是局部 RAII 变量；`std::vector<std::unique_ptr<Buffer>>` 需要 unique_ptr 可以移动。如果不用 unique_ptr 而直接用 `std::vector<Buffer>`，则 Buffer 必须有移动构造。

3. **Uniform buffer 的 persistent mapping** — 当前代码在 `createUniformBuffers` 里 `vkMapMemory` 后存入 `uniformBuffersMapped[i]`，直到程序结束都不 unmap。RAII 化后通过 `Buffer::map()` 实现同样效果，析构时自动 unmap。

4. **`textureSampler` 暂不处理** — Sampler 是独立资源，按计划归入 `core/Sampler.h`（可以算 1.3 的附加内容或单独做）。当前保留 `cleanup()` 中的 `vkDestroySampler` 手动销毁。

5. **先修 Device.h 的 bug** — `graphicsQueue()` 返回了 `presentQueue_`，务必修正为 `graphicsQueue_`，否则后续 `beginSingleTimeCommands` 提交到错误队列。
