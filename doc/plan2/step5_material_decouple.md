# 第五步：Material 解耦

## 目标

Material 不再持有 Pipeline 对象。Material 只负责：
1. 持有 `PipelineConfig`（声明自己需要什么管线配置）
2. 管理 descriptor set（UBO / Texture 绑定）

Pipeline 由 Application 显式创建和持有（当前只有 1 条 opaque pipeline），等管线种类超过手动管理能力时再引入缓存。

## 前置条件

- step3 完成（Pipeline 接受 PipelineConfig）
- step4 完成（GltfLoader 可加载几何）

## 改动清单

### A. Material 去掉 Pipeline 持有

**Material.h 修改后：**

```cpp
class Material {
  public:
    Material(Device &device, Renderer &renderer, const Texture &texture,
             const PipelineConfig &config);
    ~Material();

    // 绑定 descriptor set（不绑定 pipeline）
    void bindDescriptors(VkCommandBuffer cmd, VkPipelineLayout layout,
                         uint32_t frameIndex) const;

    // 获取此材质的管线配置（供外部比较）
    const PipelineConfig &pipelineConfig() const { return config_; }

    // descriptor set layout（pipeline 创建时需要）
    VkDescriptorSetLayout descriptorSetLayout() const { return descriptorSetLayout_; }

  private:
    void createDescriptorSetLayout();
    void createDescriptorPool();
    void createDescriptorSets(const Texture &texture);

    Device    *device_;
    Renderer  *renderer_;

    PipelineConfig              config_;
    VkDescriptorSetLayout       descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool            descriptorPool_       = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_;
};
```

**关键变化：**
- 构造函数接受 `PipelineConfig` 而非 shader 路径
- 移除 `unique_ptr<Pipeline> pipeline_`
- 原来的 `bind()` 拆成 `bindDescriptors()`（不再绑 pipeline）
- `pipelineLayout()` 删除 → layout 由外部管理

### B. Application 显式持有 Pipeline

不引入 PipelineCache / hash 缓存机制。当前只有 1 条 opaque pipeline，直接在 Application 中显式创建：

```cpp
// Application.h
class Application {
    // ...
    std::unique_ptr<Pipeline> opaquePipeline_;
};

// Application.cpp — init()
PipelineConfig config;
config.vertShaderPath    = config_.vertShaderPath;
config.fragShaderPath    = config_.fragShaderPath;
config.vertexLayout      = defaultVertexLayout();
config.msaaSamples       = device_->msaaSamples();
config.descriptorLayouts = {material_->descriptorSetLayout()};
config.pushConstants     = {{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)}};

opaquePipeline_ = std::make_unique<Pipeline>(*device_, renderer_->renderPass(), config);
```

如果未来需要第二条管线（如 alpha blend），只需再加一个成员：
```cpp
std::unique_ptr<Pipeline> transparentPipeline_;  // 需要时加
```

等管线种类多到手动管理不便（>5 条）时，再提取 PipelineCache。

### C. Scene::render() 接受外部 Pipeline

```cpp
// Scene.h — render 签名增加 Pipeline 参数
void render(VkCommandBuffer cmd, uint32_t frameIndex,
            Pipeline &pipeline) const;

// Scene.cpp
void Scene::render(VkCommandBuffer cmd, uint32_t frameIndex,
                   Pipeline &pipeline) const
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());

    for (const auto &obj : objects_) {
        obj.material->bindDescriptors(cmd, pipeline.layout(), frameIndex);

        vkCmdPushConstants(cmd, pipeline.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(glm::mat4), &obj.transform);

        obj.mesh->bind(cmd);
        obj.mesh->draw(cmd);
    }
}
```

> 当前所有对象共用同一条 pipeline，所以 `vkCmdBindPipeline` 只调一次。
> 未来若需多管线，可改为按 material 分组渲染，或传入 `vector<Pipeline*>`。

### D. Application mainLoop 适配

```cpp
// Application.cpp — mainLoop 内
renderer_->beginRenderPass(ctx->cmd, ctx->imageIndex);
scene_.render(ctx->cmd, ctx->frameIndex, *opaquePipeline_);
renderer_->endRenderPass(ctx->cmd);
```

### E. Material 构造调用适配

```cpp
// 之前
auto material = std::make_shared<Material>(
    *device_, *renderer_, *texture_, Config::vertShaderPath, Config::fragShaderPath);

// 之后
PipelineConfig config;
config.vertShaderPath   = Config::vertShaderPath;
config.fragShaderPath   = Config::fragShaderPath;
config.vertexLayout     = defaultVertexLayout();
config.msaaSamples      = device_->msaaSamples();
config.pushConstants    = {{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)}};
// descriptorLayouts 在 Material 内部设置

auto material = std::make_shared<Material>(*device_, *renderer_, *texture_, config);
```

Material 内部仍创建 descriptorSetLayout，并在构造时把它塞到 `config_.descriptorLayouts = {descriptorSetLayout_}`。

### F. SwapChain 重建时重建 Pipeline

```cpp
// Application.cpp — recreateSwapChain 之后
// renderPass 不变（只是 framebuffer 重建），所以 pipeline 不需要重建
// 但如果 renderPass 也重建了，则需要：
opaquePipeline_.reset();
opaquePipeline_ = std::make_unique<Pipeline>(*device_, renderer_->renderPass(), config);
```

> 注：当前 `recreateSwapChain()` 不重建 renderPass（format 不变），所以 Pipeline 通常不需要重建。
> 保留重建逻辑作为防御性代码即可。

## 验证

1. **编译通过**
2. **渲染结果不变** —— 同一纹理、同一模型
3. **Pipeline 不在 Material 中**：确认 Material 析构不 destroy pipeline
4. **SwapChain 重建**：拉伸窗口正常，无崩溃

## 文件变更总结

```
修改：src/render/Material.h         (去掉 Pipeline 持有，改接口)
修改：src/render/Material.cpp       (构造函数改参数，bind 改为 bindDescriptors)
修改：src/scene/Scene.h             (render 签名增加 Pipeline&)
修改：src/scene/Scene.cpp           (render 接受外部 pipeline)
修改：src/app/Application.h         (持有 opaquePipeline_)
修改：src/app/Application.cpp       (显式创建 Pipeline，传入 render)
```
