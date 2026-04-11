## 阶段 3.1 Mesh 类 — 具体操作步骤

### 目标

将 App 中分散的顶点/索引数据加载（`loadModel()`）、Buffer 创建（`createVertexBuffer()`/`createIndexBuffer()`）、以及渲染时的 bind/draw 调用，收拢到独立的 `Mesh` 类中。完成后：

- App 不再持有 `vertices`、`indices`、`vertexBuffer_`、`indexBuffer_`
- App 不再持有 `copyBuffer()` 辅助方法
- `loadModel()` 从 App 方法变为 `Mesh::fromOBJ()` 静态工厂
- `vulkan_vertex.cpp` 和 `vulkan_model.cpp` 删除
- mainLoop 中的 bind/draw 调用简化为 `mesh_->bind(cmd); mesh_->draw(cmd);`

---

### 前置分析

#### 当前相关资源分布

| 资源/方法 | 当前位置 | 目标位置 |
|-----------|---------|---------|
| `std::vector<Vertex> vertices` | App 成员 | Mesh 构造期间临时变量 |
| `std::vector<uint32_t> indices` | App 成员 | Mesh 构造期间临时变量 |
| `unique_ptr<Buffer> vertexBuffer_` | App 成员 | Mesh 私有成员 |
| `unique_ptr<Buffer> indexBuffer_` | App 成员 | Mesh 私有成员 |
| `loadModel()` | `vulkan_model.cpp` | `Mesh::fromOBJ()` |
| `createVertexBuffer()` | `vulkan_vertex.cpp` | Mesh 构造器内部 |
| `createIndexBuffer()` | `vulkan_vertex.cpp` | Mesh 构造器内部 |
| `copyBuffer()` | `vulkan_vertex.cpp` | Mesh 私有静态方法或 Renderer 公开方法 |

#### copyBuffer 归属讨论

`copyBuffer()` 使用 `renderer_->beginSingleTimeCommands()` 做 staging→device 拷贝。这是通用的 GPU 缓冲拷贝操作，不仅 Mesh 需要，Texture 也需要（`copyBufferToImage` 同样依赖单次命令）。

**方案**：将 `copyBuffer()` 提升为 `Renderer` 的公共方法。这样 Mesh 和后续 Texture 都可以通过 `Renderer&` 调用。

---

### 步骤 1：Renderer 新增 copyBuffer 公共方法

#### 1a. 修改 Renderer.h

在公共接口区新增：

```cpp
// ---- GPU 传输辅助 ----
void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size);
```

#### 1b. 修改 Renderer.cpp

```cpp
void Renderer::copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size) {
    VkCommandBuffer cmd = beginSingleTimeCommands();

    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(cmd, src, dst, 1, &copyRegion);

    endSingleTimeCommands(cmd);
}
```

#### 1c. 验证

编译通过即可（尚无调用方变更）。

---

### 步骤 2：创建 Mesh 类

#### 2a. 创建 src/core/Mesh.h

```cpp
#pragma once
#include "Buffer.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class Renderer;

class Mesh {
  public:
    // 从 OBJ 文件加载（内含 staging + GPU upload）
    static std::unique_ptr<Mesh> fromOBJ(Device &device, Renderer &renderer,
                                         const std::string &path);

    // 手动构造（顶点 + 索引数据）
    Mesh(Device &device, Renderer &renderer,
         const void *vertexData, VkDeviceSize vertexSize,
         const uint32_t *indexData, uint32_t indexCount);
    ~Mesh() = default;

    Mesh(const Mesh &) = delete;
    Mesh &operator=(const Mesh &) = delete;

    void bind(VkCommandBuffer cmd) const;
    void draw(VkCommandBuffer cmd) const;

    uint32_t indexCount() const { return indexCount_; }

  private:
    std::unique_ptr<Buffer> vertexBuffer_;
    std::unique_ptr<Buffer> indexBuffer_;
    uint32_t                indexCount_ = 0;
};

} // namespace vkr
```

**设计说明**：
- `fromOBJ()` 返回 `unique_ptr<Mesh>` 而非值类型，因为 Mesh 持有不可拷贝的 Buffer
- 额外提供原始数据构造器，方便未来非 OBJ 数据源（如程序生成网格）
- `bind()` 绑定顶点和索引缓冲，`draw()` 调用 `vkCmdDrawIndexed`
- Vertex 结构体定义仍留在 `vulkan_utils.h`（共享类型，Mesh 和 Pipeline 都依赖）

#### 2b. 创建 src/core/Mesh.cpp

```cpp
#include "Mesh.h"
#include "Device.h"
#include "Renderer.h"
#include "vulkan_utils.h"

#include <tiny_obj_loader.h>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

namespace vkr {

Mesh::Mesh(Device &device, Renderer &renderer,
           const void *vertexData, VkDeviceSize vertexSize,
           const uint32_t *indexData, uint32_t indexCount)
    : indexCount_(indexCount) {

    // ---- 顶点缓冲 ----
    {
        Buffer staging(device, vertexSize,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        void *mapped = staging.map();
        memcpy(mapped, vertexData, vertexSize);
        staging.unmap();

        vertexBuffer_ = std::make_unique<Buffer>(
            device, vertexSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        renderer.copyBuffer(staging.handle(),
                            vertexBuffer_->handle(), vertexSize);
    }

    // ---- 索引缓冲 ----
    {
        VkDeviceSize indexSize =
            static_cast<VkDeviceSize>(indexCount) * sizeof(uint32_t);

        Buffer staging(device, indexSize,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        void *mapped = staging.map();
        memcpy(mapped, indexData, indexSize);
        staging.unmap();

        indexBuffer_ = std::make_unique<Buffer>(
            device, indexSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        renderer.copyBuffer(staging.handle(),
                            indexBuffer_->handle(), indexSize);
    }
}

std::unique_ptr<Mesh> Mesh::fromOBJ(Device &device, Renderer &renderer,
                                     const std::string &path) {
    tinyobj::attrib_t                attrib;
    std::vector<tinyobj::shape_t>    shapes;
    std::vector<tinyobj::material_t> materials;
    std::string                      err;

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &err, path.c_str())) {
        throw std::runtime_error(err);
    }

    std::vector<Vertex>               vertices;
    std::vector<uint32_t>             indices;
    std::unordered_map<Vertex, uint32_t> uniqueVertices;

    for (const auto &shape : shapes) {
        for (const auto &index : shape.mesh.indices) {
            Vertex vertex{};
            vertex.pos = {attrib.vertices[3 * index.vertex_index + 0],
                          attrib.vertices[3 * index.vertex_index + 1],
                          attrib.vertices[3 * index.vertex_index + 2]};
            vertex.texCoord = {
                attrib.texcoords[2 * index.texcoord_index + 0],
                1.0f - attrib.texcoords[2 * index.texcoord_index + 1]};
            vertex.color = {1.0f, 1.0f, 1.0f};

            if (uniqueVertices.count(vertex) == 0) {
                uniqueVertices[vertex] =
                    static_cast<uint32_t>(vertices.size());
                vertices.push_back(vertex);
            }
            indices.push_back(uniqueVertices[vertex]);
        }
    }

    std::cout << "Vertices: " << vertices.size()
              << ", Indices: " << indices.size() << std::endl;

    return std::make_unique<Mesh>(
        device, renderer,
        vertices.data(),
        static_cast<VkDeviceSize>(sizeof(Vertex) * vertices.size()),
        indices.data(),
        static_cast<uint32_t>(indices.size()));
}

void Mesh::bind(VkCommandBuffer cmd) const {
    VkBuffer     buffers[] = {vertexBuffer_->handle()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, indexBuffer_->handle(), 0,
                         VK_INDEX_TYPE_UINT32);
}

void Mesh::draw(VkCommandBuffer cmd) const {
    vkCmdDrawIndexed(cmd, indexCount_, 1, 0, 0, 0);
}

} // namespace vkr
```

#### 2c. 验证

编译通过即可。

---

### 步骤 3：App 切换到 Mesh

#### 3a. 修改 app.h

```diff
+ #include "core/Mesh.h"

  // 删除以下成员：
- std::vector<Vertex>   vertices;
- std::vector<uint32_t> indices;
- std::unique_ptr<vkr::Buffer> vertexBuffer_;
- std::unique_ptr<vkr::Buffer> indexBuffer_;

  // 新增：
+ std::unique_ptr<vkr::Mesh> mesh_;

  // 删除以下方法声明：
- void createVertexBuffer();
- void createIndexBuffer();
- void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
- void loadModel();
```

#### 3b. 修改 app.cpp — initVulkan

```diff
  // 替换：
- loadModel();
- createVertexBuffer();
- createIndexBuffer();
  // 为：
+ mesh_ = vkr::Mesh::fromOBJ(*device, *renderer_, MODEL_PATH);
```

#### 3c. 修改 app.cpp — mainLoop

```diff
  // 替换 bind/draw 调用：
- VkBuffer     vertexBuffers[] = {vertexBuffer_->handle()};
- VkDeviceSize offsets[] = {0};
- vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
- vkCmdBindIndexBuffer(cmd, indexBuffer_->handle(), 0,
-                      VK_INDEX_TYPE_UINT32);
- ...
- vkCmdDrawIndexed(cmd, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
  // 为：
+ mesh_->bind(cmd);
+ mesh_->draw(cmd);
```

注意：`vkCmdBindDescriptorSets` 调用保持不变，它在 bind/draw 之间。

完整 mainLoop 渲染段应为：

```cpp
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                  pipeline_->handle());

// viewport + scissor 设置 ...

mesh_->bind(cmd);

vkCmdBindDescriptorSets(
    cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->layout(), 0, 1,
    &descriptorSets[renderer_->frameIndex()], 0, nullptr);

mesh_->draw(cmd);
```

#### 3d. 修改 app.cpp — cleanup

```diff
  // 删除：
- indexBuffer_.reset();
- vertexBuffer_.reset();
  // 替换为：
+ mesh_.reset();
```

#### 3e. 删除文件

| 文件 | 原因 |
|------|------|
| `vulkan_vertex.cpp` | `createVertexBuffer()`/`createIndexBuffer()`/`copyBuffer()` 已迁入 Mesh + Renderer |
| `vulkan_model.cpp` | `loadModel()` 已迁入 `Mesh::fromOBJ()` |

注意：`vulkan_vertex.h` 仅包含空白/注释，一并删除。

#### 3f. 验证

```
cmake .. (在 build/ 和 build-debug/ 各执行一次)
cmake --build build --config Release
cmake --build build-debug --config Debug
```

运行确认功能不变。

---

### 变更后 App 状态一览

#### app.h 成员（步骤全部完成后）

```cpp
class HelloTriangleApplication {
private:
    GLFWwindow *window;

    std::unique_ptr<vkr::VulkanContext> context;
    std::unique_ptr<vkr::Device>       device;
    std::unique_ptr<vkr::SwapChain>    swapChain_;
    std::unique_ptr<vkr::Renderer>     renderer_;
    std::unique_ptr<vkr::Pipeline>     pipeline_;
    std::unique_ptr<vkr::Mesh>         mesh_;

    // 描述符（阶段 3.3 → Material / DescriptorManager）
    VkDescriptorSetLayout              descriptorSetLayout;
    VkDescriptorPool                   descriptorPool;
    std::vector<VkDescriptorSet>       descriptorSets;

    // 纹理（阶段 3.2 → Texture）
    std::unique_ptr<vkr::Image>        textureImage_;
    VkSampler                          textureSampler;
    uint32_t                           mipLevels;
};
```

#### cleanup()（步骤全部完成后）

```cpp
void HelloTriangleApplication::cleanup() {
    VkDevice d = device->logicalDevice();

    renderer_.reset();
    pipeline_.reset();
    mesh_.reset();

    vkDestroyDescriptorPool(d, descriptorPool, nullptr);
    vkDestroySampler(d, textureSampler, nullptr);
    textureImage_.reset();
    vkDestroyDescriptorSetLayout(d, descriptorSetLayout, nullptr);

    swapChain_.reset();
    device.reset();
    context.reset();

    glfwDestroyWindow(window);
    glfwTerminate();
}
```

#### 删除的文件

| 文件 | 原因 |
|------|------|
| `vulkan_vertex.cpp` | 内容迁入 Mesh 构造器 + Renderer::copyBuffer |
| `vulkan_vertex.h` | 已空，不再需要 |
| `vulkan_model.cpp` | 内容迁入 Mesh::fromOBJ |

#### 新增的文件

| 文件 | 内容 |
|------|------|
| `src/core/Mesh.h` | Mesh 类声明 |
| `src/core/Mesh.cpp` | Mesh 实现（fromOBJ + bind/draw + 缓冲创建） |

---

### 阶段 3.1 验收标准

| 标准 | 说明 |
|------|------|
| App 不持有 vertices / indices 数据 | 模型数据仅在加载期间存在 |
| App 不持有 vertexBuffer_ / indexBuffer_ | 由 Mesh 管理 |
| App 无 loadModel / createVertexBuffer / createIndexBuffer / copyBuffer 方法 | 全部迁入 Mesh / Renderer |
| mainLoop 使用 mesh_->bind() + mesh_->draw() | 渲染调用简化 |
| Renderer 提供 copyBuffer() 公共方法 | 供 Mesh 和后续 Texture 使用 |
| Release + Debug 编译通过，运行结果不变 | 无功能回归 |
