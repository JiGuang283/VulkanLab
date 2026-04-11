# 阶段 3.3：Material 类

## 一、目标

将 Descriptor 相关逻辑（DescriptorSetLayout、DescriptorPool、DescriptorSets）与 Pipeline 绑定，封装为独立的 `vkr::Material` RAII 类。Material 代表 **"用什么 shader + 用什么纹理来画"** 的完整描述。

完成后从 App 中移除以下成员和方法：

| 移除的成员变量 | 说明 |
|---|---|
| `unique_ptr<Pipeline> pipeline_` | 合并进 Material 内部 |
| `VkDescriptorSetLayout descriptorSetLayout` | 合并进 Material 内部 |
| `VkDescriptorPool descriptorPool` | 合并进 Material 内部 |
| `vector<VkDescriptorSet> descriptorSets` | 合并进 Material 内部 |

| 移除的方法 | 来源文件 |
|---|---|
| `createDescriptorSetLayout()` | vulkan_uniform.cpp |
| `createDescriptionPool()` | vulkan_uniform.cpp |
| `createDescriptorSets()` | vulkan_uniform.cpp |

`updateUniformBuffer()` 暂留在 App 中（后续阶段 3.5 Camera 时迁移）。

完成后删除文件：`vulkan_uniform.cpp`。

---

## 二、类设计

```cpp
// src/render/Material.h
#pragma once

#include "core/Device.h"
#include "core/Pipeline.h"
#include "Renderer.h"
#include "Texture.h"

#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Material {
public:
    Material(Device& device, Renderer& renderer, const Texture& texture,
             const std::string& vertShader, const std::string& fragShader);
    ~Material();

    Material(const Material&) = delete;
    Material& operator=(const Material&) = delete;

    /// 绑定 Pipeline + DescriptorSet 到命令缓冲
    void bind(VkCommandBuffer cmd, uint32_t frameIndex) const;

    /// 访问器（供外部需要时使用）
    VkPipelineLayout pipelineLayout() const;

private:
    void createDescriptorSetLayout();
    void createDescriptorPool();
    void createDescriptorSets(const Texture& texture);

    Device*   device_ = nullptr;
    Renderer* renderer_ = nullptr;

    std::unique_ptr<Pipeline>    pipeline_;
    VkDescriptorSetLayout        descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool             descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_;
};

} // namespace vkr
```

### 设计要点

1. **构造函数完成全部工作** — 创建 DescriptorSetLayout → Pipeline → DescriptorPool → DescriptorSets，一步到位。
2. **析构函数** — 销毁 DescriptorPool（DescriptorSets 随之自动释放）、DescriptorSetLayout。Pipeline 由 `unique_ptr` 自动销毁。
3. **`bind(cmd, frameIndex)`** — 封装 `vkCmdBindPipeline` + `vkCmdBindDescriptorSets`，外部只需一行调用。
4. **构造函数接收 `const Texture&`** — 用于在 `createDescriptorSets` 中写入纹理 ImageView/Sampler。Texture 的生命周期由外部管理，Material 不持有所有权。
5. **接收 `Renderer&`** — 用于获取 `renderPass`（创建 Pipeline）和 per-frame `uniformBufferHandle`（写入 DescriptorSets）。
6. **Pipeline 内置到 Material** — 当前只有一种 shader 组合，Pipeline 与 Material 一一对应。未来支持多材质时，每个 Material 持有自己的 Pipeline。

---

## 三、实施步骤

### 步骤 1：创建 Material.h / Material.cpp

- 文件位置：`src/render/Material.h`、`src/render/Material.cpp`
- 命名空间：`vkr`
- 搬入逻辑映射：
  - `createDescriptorSetLayout()` → `Material::createDescriptorSetLayout()`（逻辑不变）
  - `createDescriptionPool()` → `Material::createDescriptorPool()`（逻辑不变）
  - `createDescriptorSets()` → `Material::createDescriptorSets(texture)`（改为接收 Texture 引用）
  - Pipeline 创建 → 在构造函数中，Layout 创建后直接 `pipeline_ = make_unique<Pipeline>(...)`

### 步骤 2：实现 `bind()` 方法

将 `mainLoop()` 中以下代码封装进 `Material::bind(cmd, frameIndex)`：

```cpp
// 当前散落在 mainLoop 中的代码：
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->handle());
// ...（viewport / scissor 仍留在外部）
vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
    pipeline_->layout(), 0, 1,
    &descriptorSets[renderer_->frameIndex()], 0, nullptr);
```

封装后：
```cpp
void Material::bind(VkCommandBuffer cmd, uint32_t frameIndex) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_->layout(), 0, 1,
        &descriptorSets_[frameIndex], 0, nullptr);
}
```

### 步骤 3：修改 app.h

- 移除成员：`pipeline_`、`descriptorSetLayout`、`descriptorPool`、`descriptorSets`
- 移除方法声明：`createDescriptorSetLayout`、`createDescriptionPool`、`createDescriptorSets`
- 新增成员：`std::unique_ptr<vkr::Material> material_`
- 新增 include：`#include "render/Material.h"`
- 可移除的 include：`#include "core/Pipeline.h"`（Pipeline 已被 Material 内部持有）

### 步骤 4：修改 app.cpp — initVulkan()

替换：
```cpp
createDescriptorSetLayout();
pipeline_ = std::make_unique<vkr::Pipeline>(
    *device, renderer_->renderPass(), descriptorSetLayout,
    "shader/vert.spv", "shader/frag.spv");
texture_ = std::make_unique<vkr::Texture>(*device, *renderer_, TEXTURE_PATH);
mesh_ = vkr::Mesh::fromOBJ(*device, *renderer_, MODEL_PATH);
createDescriptionPool();
createDescriptorSets();
```

为：
```cpp
texture_ = std::make_unique<vkr::Texture>(*device, *renderer_, TEXTURE_PATH);
material_ = std::make_unique<vkr::Material>(
    *device, *renderer_, *texture_,
    "shader/vert.spv", "shader/frag.spv");
mesh_ = vkr::Mesh::fromOBJ(*device, *renderer_, MODEL_PATH);
```

注意：Texture 必须在 Material 之前创建（Material 构造时需要读取 Texture 的 ImageView/Sampler）。

### 步骤 5：修改 app.cpp — mainLoop()

替换：
```cpp
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                  pipeline_->handle());

VkViewport viewport{};
// ... viewport / scissor 设置 ...
vkCmdSetScissor(cmd, 0, 1, &scissor);

mesh_->bind(cmd);

vkCmdBindDescriptorSets(
    cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->layout(), 0, 1,
    &descriptorSets[renderer_->frameIndex()], 0, nullptr);

mesh_->draw(cmd);
```

为：
```cpp
material_->bind(cmd, renderer_->frameIndex());

VkViewport viewport{};
// ... viewport / scissor 设置 ...
vkCmdSetScissor(cmd, 0, 1, &scissor);

mesh_->bind(cmd);
mesh_->draw(cmd);
```

Viewport / Scissor 设置保留在 App 层（它们与材质无关，属于渲染器全局状态）。

### 步骤 6：修改 app.cpp — cleanup()

替换：
```cpp
vkDestroyDescriptorPool(d, descriptorPool, nullptr);
texture_.reset();
vkDestroyDescriptorSetLayout(d, descriptorSetLayout, nullptr);
mesh_.reset();
pipeline_.reset();
```

为：
```cpp
material_.reset();
texture_.reset();
mesh_.reset();
```

Material 析构时自动销毁 DescriptorPool → DescriptorSetLayout → Pipeline。

### 步骤 7：删除旧文件

- 删除 `src/vulkan_uniform.cpp`

### 步骤 8：构建验证

- 两个 build 目录均需 `cmake ..` 重新配置
- Release / Debug 编译通过
- 运行程序，渲染结果与重构前一致

---

## 四、依赖关系

```
Material 依赖:
  ├── Device       — 创建 DescriptorSetLayout / Pool
  ├── Renderer     — renderPass（创建 Pipeline）、uniformBufferHandle（写入 DescriptorSets）
  ├── Pipeline     — 内部持有，RAII 管理
  └── Texture      — imageView + sampler（写入 DescriptorSets，不持有所有权）
```

---

## 五、对外部接口的影响

| 调用方 | 变更 |
|---|---|
| `initVulkan()` (app.cpp) | 6 行调用合并为 3 行（Texture + Material + Mesh） |
| `mainLoop()` (app.cpp) | `vkCmdBindPipeline` + `vkCmdBindDescriptorSets` 合并为 `material_->bind()` |
| `cleanup()` (app.cpp) | 3 行手动销毁合并为 `material_.reset()` |

除以上 3 处外，无其他文件受影响。

---

## 六、阶段完成后 App 状态

完成 3.3 后，App 将只剩以下成员：

```cpp
GLFWwindow* window;
unique_ptr<VulkanContext> context;
unique_ptr<Device>        device;
unique_ptr<SwapChain>     swapChain_;
unique_ptr<Renderer>      renderer_;
unique_ptr<Texture>       texture_;
unique_ptr<Material>      material_;
unique_ptr<Mesh>          mesh_;
```

方法仅剩：
```
initWindow, initVulkan, mainLoop, cleanup,
framebufferResizeCallback, updateUniformBuffer
```

`vulkan_uniform.cpp` 被完全删除，App 不再持有任何裸 Vulkan Descriptor 句柄。`updateUniformBuffer` 是唯一残留的渲染逻辑方法，将在阶段 3.5 Camera 中迁出。
