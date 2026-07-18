# 第三步：Pipeline 参数化

## 目标

将 `Pipeline` 构造函数从硬编码所有状态改为接受 `PipelineConfig` 结构体，使不同材质可以使用不同的管线配置（vertex layout、cull mode、blend 等）。

## 前置状态

当前 `Pipeline` 构造函数：
```cpp
Pipeline(Device &device, VkRenderPass renderPass,
         VkDescriptorSetLayout descriptorSetLayout,
         const std::string &vertPath, const std::string &fragPath);
```
内部硬编码：`Vertex::getBindingDescription()`、`VK_CULL_MODE_BACK_BIT`、`VK_POLYGON_MODE_FILL`、`device->msaaSamples()`、1 个 push constant range（mat4）。

## 改动清单

### A. 新建 `src/core/PipelineConfig.h`

```cpp
#pragma once

#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

/// 顶点布局描述（替代硬编码 Vertex::get*）
struct VertexLayout {
    VkVertexInputBindingDescription                binding;
    std::vector<VkVertexInputAttributeDescription>  attributes;
};

/// Pipeline 可配置参数
struct PipelineConfig {
    // ---- Shader ----
    std::string vertShaderPath;
    std::string fragShaderPath;

    // ---- 顶点输入 ----
    VertexLayout vertexLayout;

    // ---- 光栅化 ----
    VkPolygonMode   polygonMode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags cullMode    = VK_CULL_MODE_BACK_BIT;
    VkFrontFace     frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    // ---- 深度 ----
    bool        depthTest    = true;
    bool        depthWrite   = true;
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS;

    // ---- 混合 ----
    bool blendEnable = false;  // true → 标准 alpha blend

    // ---- 多重采样 ----
    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;

    // ---- Descriptor / PushConstant（由 Material 设置）----
    std::vector<VkDescriptorSetLayout> descriptorLayouts;
    std::vector<VkPushConstantRange>   pushConstants;
};

/// 辅助：从当前 Vertex 结构体生成默认 VertexLayout
/// 供 OBJ / 简单 glTF 使用
VertexLayout defaultVertexLayout();

} // namespace vkr
```

`defaultVertexLayout()` 实现放在 `PipelineConfig.cpp` 或直接 inline：
```cpp
inline VertexLayout defaultVertexLayout() {
    VertexLayout layout;
    layout.binding = Vertex::getBindingDescription();
    auto attrs = Vertex::getAttributeDescriptions();
    layout.attributes.assign(attrs.begin(), attrs.end());
    return layout;
}
```

### B. 修改 `Pipeline` 构造函数

```cpp
// Pipeline.h
class Pipeline {
  public:
    Pipeline(Device &device, VkRenderPass renderPass,
             const PipelineConfig &config);
    ~Pipeline();
    // ... handle(), layout() 不变
};
```

```cpp
// Pipeline.cpp — 用 config 替代硬编码
Pipeline::Pipeline(Device &device, VkRenderPass renderPass,
                   const PipelineConfig &config)
    : device_(&device)
{
    auto vertCode = readFile(config.vertShaderPath);
    auto fragCode = readFile(config.fragShaderPath);
    VkShaderModule vertModule = createShaderModule(vertCode);
    VkShaderModule fragModule = createShaderModule(fragCode);

    // ... shader stages 不变 ...

    // 顶点输入 —— 从 config 取
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions    = &config.vertexLayout.binding;
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(config.vertexLayout.attributes.size());
    vertexInputInfo.pVertexAttributeDescriptions = config.vertexLayout.attributes.data();

    // 光栅化 —— 从 config 取
    rasterizer.polygonMode = config.polygonMode;
    rasterizer.cullMode    = config.cullMode;
    rasterizer.frontFace   = config.frontFace;

    // 多重采样 —— 从 config 取
    multisampling.rasterizationSamples = config.msaaSamples;

    // 深度 —— 从 config 取
    depthStencil.depthTestEnable  = config.depthTest  ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = config.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp   = config.depthCompare;

    // 混合 —— 从 config 取
    colorBlendAttachment.blendEnable = config.blendEnable ? VK_TRUE : VK_FALSE;
    if (config.blendEnable) {
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        colorBlendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;
    }

    // PipelineLayout —— 从 config 取
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(config.descriptorLayouts.size());
    layoutInfo.pSetLayouts    = config.descriptorLayouts.data();
    layoutInfo.pushConstantRangeCount = static_cast<uint32_t>(config.pushConstants.size());
    layoutInfo.pPushConstantRanges    = config.pushConstants.data();

    // ... 其余创建逻辑不变 ...
}
```

### C. 修改 `Material.cpp` —— 构造 PipelineConfig 传给 Pipeline

```cpp
Material::Material(Device &device, Renderer &renderer, const Texture &texture,
                   const std::string &vertShader, const std::string &fragShader)
    : device_(&device), renderer_(&renderer)
{
    createDescriptorSetLayout();

    // 构造 PipelineConfig
    PipelineConfig config;
    config.vertShaderPath   = vertShader;
    config.fragShaderPath   = fragShader;
    config.vertexLayout     = defaultVertexLayout();
    config.msaaSamples      = device.msaaSamples();
    config.descriptorLayouts = {descriptorSetLayout_};
    config.pushConstants     = {{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 16}};

    pipeline_ = std::make_unique<Pipeline>(device, renderer.renderPass(), config);

    createDescriptorPool();
    createDescriptorSets(texture);
}
```

Material 的外部接口完全不变，只是内部从原来传 4 个参数给 Pipeline 改为传 `PipelineConfig`。

### D. 移除 `Pipeline.cpp` 对 `Vertex.h` 的 include

Pipeline 不再直接 `#include "render/Vertex.h"`——vertex layout 信息通过 `PipelineConfig` 传入。

## 验证

1. **编译通过**
2. **运行效果不变**
3. **手动测试**：在 `Material.cpp` 中改 `config.cullMode = VK_CULL_MODE_NONE`，确认模型双面可见
4. **手动测试**：改 `config.polygonMode = VK_POLYGON_MODE_LINE`（需开启 `fillModeNonSolid` feature），确认线框模式

## 文件变更总结

```
新增：src/core/PipelineConfig.h
修改：src/core/Pipeline.h          (构造函数签名)
修改：src/core/Pipeline.cpp        (用 PipelineConfig 替代硬编码)
修改：src/render/Material.cpp      (构造 PipelineConfig)
```
