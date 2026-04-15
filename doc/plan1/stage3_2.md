# 阶段 3.2：Texture 类

## 一、目标

将纹理加载、Mipmap 生成、Image 创建、ImageView 创建、Sampler 创建等分散在 `vulkan_texture.cpp` 和 `vulkan_mipmap.cpp` 中的逻辑，封装为独立的 `vkr::Texture` RAII 类。

完成后从 App 中移除以下成员和方法：

| 移除的成员变量 | 说明 |
|---|---|
| `unique_ptr<Image> textureImage_` | 合并进 Texture 内部 |
| `VkSampler textureSampler` | 合并进 Texture 内部 |
| `uint32_t mipLevels` | 合并进 Texture 内部 |

| 移除的方法 | 来源文件 |
|---|---|
| `createTextureImage()` | vulkan_texture.cpp |
| `createTextureImageView()` | vulkan_texture.cpp |
| `createTextureSampler()` | vulkan_texture.cpp |
| `transitionImageLayout()` | vulkan_texture.cpp |
| `copyBufferToImage()` | vulkan_texture.cpp |
| `generateMipmaps()` | vulkan_mipmap.cpp |

完成后删除文件：`vulkan_texture.cpp`、`vulkan_mipmap.cpp`。

---

## 二、类设计

```cpp
// src/render/Texture.h
#pragma once
#include "core/Device.h"
#include "core/Image.h"
#include <string>
#include <memory>
#include <vulkan/vulkan.h>

namespace vkr {

class Renderer;

class Texture {
public:
    /// 从图片文件加载（自动创建 Image + Mipmap + Sampler）
    Texture(Device& device, Renderer& renderer, const std::string& path);
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    VkImageView imageView() const { return image_->imageView(); }
    VkSampler   sampler()   const { return sampler_; }

private:
    void loadFromFile(Renderer& renderer, const std::string& path);
    void createSampler();

    // ---- 图像操作辅助（从 App 搬入） ----
    static void transitionImageLayout(VkCommandBuffer cmd, VkImage image,
                                      VkFormat format,
                                      VkImageLayout oldLayout,
                                      VkImageLayout newLayout,
                                      uint32_t mipLevels);

    static void copyBufferToImage(VkCommandBuffer cmd, VkBuffer buffer,
                                  VkImage image,
                                  uint32_t width, uint32_t height);

    void generateMipmaps(Renderer& renderer, VkImage image,
                         VkFormat format, int32_t width, int32_t height,
                         uint32_t mipLevels);

    Device*                 device_ = nullptr;
    std::unique_ptr<Image>  image_;
    VkSampler               sampler_ = VK_NULL_HANDLE;
    uint32_t                mipLevels_ = 1;
};

} // namespace vkr
```

### 设计要点

1. **构造函数完成全部工作** — 一次调用完成：stb 加载 → staging buffer 上传 → Image 创建 → layout transition → buffer→image 拷贝 → mipmap 生成 → ImageView 创建 → Sampler 创建。
2. **析构函数** — 销毁 `VkSampler`，`Image` 由 `unique_ptr` 自动销毁。
3. **辅助方法改为接收 `VkCommandBuffer` 参数** — `transitionImageLayout` 和 `copyBufferToImage` 原来各自开启/提交单次命令，搬入 Texture 后合并为单次命令提交（减少 GPU 同步次数），仅 `generateMipmaps` 单独提交（因其内部有复杂的 barrier 链）。

---

## 三、实施步骤

### 步骤 1：创建 Texture.h / Texture.cpp

- 文件位置：`src/render/Texture.h`、`src/render/Texture.cpp`
- 命名空间：`vkr`
- 将以下逻辑搬入 `Texture` 构造函数 / 私有方法：
  - `createTextureImage()` → `loadFromFile()`
  - `createTextureImageView()` → 在 `loadFromFile()` 末尾直接调用 `image_->createView()`
  - `createTextureSampler()` → `createSampler()`
  - `transitionImageLayout()` → 改为 static，接收 `VkCommandBuffer` 参数
  - `copyBufferToImage()` → 改为 static，接收 `VkCommandBuffer` 参数
  - `generateMipmaps()` → 保留为成员函数，内部开启自己的单次命令

### 步骤 2：优化命令提交

原 App 实现中：`transitionImageLayout` → 提交 → `copyBufferToImage` → 提交 → `generateMipmaps` → 提交，共 3 次单次命令。

优化为：
```
beginSingleTimeCommands()
  ├── transitionImageLayout (UNDEFINED → TRANSFER_DST)
  └── copyBufferToImage
endSingleTimeCommands()        // 第 1 次提交

generateMipmaps()              // 第 2 次提交（内部自带 begin/end）
```

这样把 transition + copy 合并到同一个命令缓冲中，从 3 次提交减少到 2 次。

### 步骤 3：修改 app.h

- 移除成员：`textureImage_`、`textureSampler`、`mipLevels`
- 移除方法声明：`createTextureImage`、`createTextureImageView`、`createTextureSampler`、`transitionImageLayout`、`copyBufferToImage`、`generateMipmaps`
- 新增成员：`std::unique_ptr<vkr::Texture> texture_`
- 新增 include：`#include "render/Texture.h"`

### 步骤 4：修改 app.cpp (initVulkan)

替换：
```cpp
createTextureImage();
createTextureImageView();
createTextureSampler();
```
为：
```cpp
texture_ = std::make_unique<vkr::Texture>(*device, *renderer_, TEXTURE_PATH);
```

### 步骤 5：修改 vulkan_uniform.cpp (createDescriptorSets)

将：
```cpp
imageInfo.imageView = textureImage_->imageView();
imageInfo.sampler = textureSampler;
```
改为：
```cpp
imageInfo.imageView = texture_->imageView();
imageInfo.sampler = texture_->sampler();
```

### 步骤 6：修改 app.cpp (cleanup)

将：
```cpp
vkDestroySampler(d, textureSampler, nullptr);
textureImage_.reset();
```
替换为：
```cpp
texture_.reset();
```

### 步骤 7：删除旧文件

- 删除 `src/vulkan_texture.cpp`
- 删除 `src/vulkan_mipmap.cpp`

### 步骤 8：构建验证

- 两个 build 目录均需 `cmake ..` 重新配置
- Release / Debug 编译通过
- 运行程序，纹理渲染结果与重构前一致

---

## 四、依赖关系

```
Texture 依赖:
  ├── Device     — 创建 Image / Sampler
  ├── Renderer   — beginSingleTimeCommands / endSingleTimeCommands
  ├── Image      — VkImage + VkImageView RAII
  ├── Buffer     — staging buffer 上传像素数据
  └── stb_image  — 图片文件解码
```

Texture 不引入任何新的外部依赖，仅使用已有的 `stb_image`、`Buffer`、`Image`。

---

## 五、对外部接口的影响

| 调用方 | 变更 |
|---|---|
| `createDescriptorSets()` (vulkan_uniform.cpp) | `textureImage_->imageView()` → `texture_->imageView()`，`textureSampler` → `texture_->sampler()` |
| `initVulkan()` (app.cpp) | 3 行调用合并为 1 行 Texture 构造 |
| `cleanup()` (app.cpp) | 2 行销毁合并为 `texture_.reset()` |

除以上 3 处外，无其他文件受影响。
