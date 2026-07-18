# 可拓展的前向渲染器架构设计

本文面向当前这个已经完成基础封装的 Vulkan 示例程序，目标不是“从零设计一个新引擎”，而是给出现有代码可持续演进的架构方案，使其逐步具备以下能力：

1. 支持多物体
2. 支持多材质与多套着色器
3. 支持多 Pass 渲染流程
4. 为 Deferred Rendering、PBR、阴影、后处理等特性预留稳定接口

当前代码已经具备一部分正确方向上的拆分：

- core 层已经有 Device、Buffer、Image、SwapChain、VulkanContext
- render 层已经有 Renderer、Mesh、Material、Texture
- scene 层已经有 Scene、SceneObject、Camera
- window 层已经有 Window、InputManager

但这些模块目前仍然偏“教程式封装”，还没有形成完整的渲染架构。核心问题是：

- Renderer 同时承担帧同步、命令缓冲、RenderPass、Framebuffer、临时拷贝和每帧 UBO 管理
- Material 仍然直接依赖某个具体 Renderer 和某个具体 RenderPass
- Scene 只是对象数组加一个顺序 render，没有“可见性收集、排序、Pass 提交”能力
- 应用层仍然显式驱动相机更新、场景更新和单通道绘制
- 资源生命周期、着色器组织、描述符布局、Pass 依赖关系都还没有抽象出来

因此，新架构应当解决的不是“继续拆类”，而是建立稳定的职责边界和运行时数据流。

## 一、设计目标

新的 Forward Renderer 建议围绕以下五个目标展开：

### 1.1 运行时分层清晰

应用层负责“组装世界”，渲染层负责“提交 GPU”，资源层负责“加载和缓存”，场景层负责“表达世界状态”。

### 1.2 数据流单向

推荐的数据流为：

Application -> Scene -> RenderWorld/RenderQueue -> RenderGraph/Passes -> RHI/Core

也就是说：

- Scene 不直接操作 Vulkan
- Material 不直接管理帧循环
- Pass 不负责加载资源
- Renderer 不直接关心业务对象如何创建

### 1.3 面向“批次”而不是面向“物体循环”

当前 Scene::render 的模式是“遍历对象，然后立即 draw”。这对单模型示例是足够的，但对多材质、多 Pass 不够。新的架构应该先收集 DrawItem，再按 Pass 和排序规则批处理。

### 1.4 RenderPass 与 Vulkan RenderPass 解耦

文档中用到的“Pass”有两个层次：

- Vulkan RenderPass / Framebuffer：底层 GPU 附件与子通道对象
- Engine Pass：例如 ShadowPass、ForwardOpaquePass、TransparentPass、PostProcessPass

这两者不能混为一谈。未来的引擎 Pass 需要组合底层附件资源，但不应被某个固定的 VkRenderPass 结构锁死。

### 1.5 预留扩展点

如果现在就把材质、描述符布局、管线创建写死为“单贴图 + 单 UBO + 单 RenderPass”，未来接 Deferred 或 PBR 时会整体返工。新架构需要把“可变项”从一开始抽成显式对象。

## 二、推荐的总体分层

建议将程序分为六层，自底向上如下：

### 2.1 Core / RHI 层

职责：只处理 Vulkan 对象生命周期、同步、上传、命令提交等低层能力，不表达渲染语义。

建议包含：

- VulkanContext：Instance、Surface、DebugMessenger
- Device：PhysicalDevice、LogicalDevice、Queue、Allocator
- SwapChain：交换链和与窗口相关的 backbuffer
- CommandContext / CommandManager：命令池、一次性命令、每帧命令缓冲分配
- Buffer、Image、Sampler、DescriptorAllocator、PipelineStateCache
- FrameResources：每帧临时资源、ring buffer、动态 UBO 分配

这一层应尽量满足两个约束：

- 不依赖 Scene、Material、Mesh 等上层概念
- 允许被 Forward 和 Deferred 共用

### 2.2 Resource 层

职责：从磁盘或缓存中加载资源，并生成运行时 GPU 资源对象。

建议包含：

- ShaderModule / ShaderProgram
- ShaderLibrary：按路径或 key 缓存着色器
- TextureAsset / TextureResource
- MeshAsset / MeshResource
- MaterialTemplate：材质模板或 shader 变体描述
- ResourceManager：统一缓存入口

这一层的关键点是“资源唯一化”：

- 同一个贴图路径只加载一次
- 同一个 mesh 文件只解析一次
- 同一套 shader permutation 只编译一次

### 2.3 Scene 层

职责：描述世界，不关心 GPU 提交细节。

建议包含：

- Scene：场景容器
- SceneNode 或 Entity：层级关系、Transform
- MeshRendererComponent：引用 Mesh + Material
- Camera
- Light：Directional、Point、Spot
- Environment：天空盒、IBL 贴图、雾参数

如果暂时不做 ECS，也没问题。当前代码使用 Scene + SceneObject 已经足够作为第一阶段形态，只需要把数据结构扩充到可收集、可排序、可按 Pass 分类即可。

### 2.4 Render Scene / Render Data 层

这是最值得新增的一层，用于把“世界对象”转换成“渲染可消费数据”。

建议包含：

- RenderObject：已经解析好 mesh、material、transform、bounds 的对象
- DrawItem：单次 draw call 所需的最小提交单元
- RenderQueue：按 Pass 分类后的绘制列表
- LightData：上传给 GPU 的紧凑光照数据
- CameraData / GlobalFrameData：当前帧全局常量

这一层的意义是把 Scene 与 Renderer 解耦。Scene 可以继续是高层对象集合，而 Renderer 只消费结构稳定、面向 GPU 提交的 DrawItem。

### 2.5 Render Pipeline 层

职责：定义“这一帧有哪些 Pass，以什么顺序执行，它们读写哪些资源”。

建议包含：

- Renderer：帧调度器，负责一帧入口和出口
- RenderPipeline：Pass 列表与执行顺序
- IRenderPass：统一 Pass 接口
- ForwardOpaquePass
- ForwardTransparentPass
- ShadowPass
- SkyboxPass
- PostProcessPass
- UIPass

这个层次不一定一开始就上完整 Frame Graph，但至少要先从“硬编码单一 RenderPass”升级到“Pass 对象列表 + 明确输入输出”。

### 2.6 Application 层

职责：初始化系统、构建场景、驱动运行，不直接发 Vulkan draw call。

Application 应当只做这些事：

- 创建 Window、Input、EngineContext
- 创建 Scene、Camera、资源实例
- 每帧更新游戏逻辑与相机
- 调用 renderer.render(scene)

## 三、推荐的目录结构

为了兼容你现在的目录，建议在现有基础上增量演进，而不是一次性大改名。推荐结构如下：

```text
src/
├── app/
│   ├── Application.h
│   ├── Application.cpp
│   └── AppConfig.h
├── core/
│   ├── VulkanContext.h/.cpp
│   ├── Device.h/.cpp
│   ├── SwapChain.h/.cpp
│   ├── CommandManager.h/.cpp
│   ├── Buffer.h/.cpp
│   ├── Image.h/.cpp
│   ├── Sampler.h/.cpp
│   ├── DescriptorAllocator.h/.cpp
│   ├── DescriptorLayoutCache.h/.cpp
│   ├── Pipeline.h/.cpp
│   ├── FrameResources.h/.cpp
│   └── VulkanTypes.h
├── resource/
│   ├── ResourceManager.h/.cpp
│   ├── ShaderLibrary.h/.cpp
│   ├── ModelLoader.h/.cpp
│   ├── TextureLoader.h/.cpp
│   └── MaterialTemplate.h/.cpp
├── render/
│   ├── Renderer.h/.cpp
│   ├── RenderPipeline.h/.cpp
│   ├── RenderGraph.h/.cpp          // 可先做轻量版
│   ├── RenderQueue.h/.cpp
│   ├── RenderFrame.h/.cpp
│   ├── RenderScene.h/.cpp
│   ├── Material.h/.cpp
│   ├── MaterialInstance.h/.cpp
│   ├── Mesh.h/.cpp
│   ├── Texture.h/.cpp
│   └── pass/
│       ├── IRenderPass.h
│       ├── ForwardOpaquePass.h/.cpp
│       ├── ForwardTransparentPass.h/.cpp
│       ├── ShadowPass.h/.cpp
│       ├── SkyboxPass.h/.cpp
│       └── PostProcessPass.h/.cpp
├── scene/
│   ├── Scene.h/.cpp
│   ├── SceneObject.h/.cpp
│   ├── Transform.h
│   ├── Camera.h/.cpp
│   ├── Light.h/.cpp
│   └── Environment.h/.cpp
├── window/
│   ├── Window.h/.cpp
│   └── InputManager.h/.cpp
└── main.cpp
```

如果你不想引入 app 和 resource 目录，也可以先不改目录，只要把职责边界按上面的方式建立起来即可。架构的重点在对象关系，不在于目录名本身。

## 四、关键运行时对象设计

下面给出建议保留的核心对象关系。

### 4.1 EngineContext

建议增加一个高层聚合对象，把当前 app.cpp 里分散持有的核心系统统一收拢。

```cpp
struct EngineContext {
    Window* window = nullptr;
    InputManager* input = nullptr;

    VulkanContext* vkContext = nullptr;
    Device* device = nullptr;
    SwapChain* swapChain = nullptr;
    CommandManager* commandManager = nullptr;
    ResourceManager* resourceManager = nullptr;
    Renderer* renderer = nullptr;
};
```

它不是为了替代依赖注入，而是为了降低早期重构成本，让 Application 能先“瘦下来”。后续再逐步把直接依赖替换成更精确的构造参数。

### 4.2 RenderFrame

当前 Renderer 里的 FrameData 已经是 RenderFrame 的雏形，但还缺少“帧级动态分配能力”。建议扩展为：

```cpp
struct RenderFrame {
    VkCommandBuffer commandBuffer;
    VkSemaphore imageAvailable;
    VkSemaphore renderFinished;
    VkFence inFlight;

    Buffer globalUniformBuffer;
    Buffer objectUploadBuffer;
    DescriptorAllocator frameDescriptorAllocator;

    uint32_t frameIndex;
};
```

这样做的原因：

- 全局数据、对象数据、材质实例更新都可以按帧写入，避免跨帧冲突
- 每帧 descriptor 临时分配可以在帧开始时整体 reset
- 后续接入动态 UBO、SSBO、GPU driven 数据上传时不需要重构 Renderer 主流程

### 4.3 RenderQueue

RenderQueue 不应该只是一个对象数组，它应该是“按渲染阶段分类后的 draw 列表”。建议形式如下：

```cpp
struct DrawItem {
    const Mesh* mesh = nullptr;
    const MaterialInstance* material = nullptr;
    glm::mat4 modelMatrix{1.0f};
    uint32_t objectId = 0;
    float sortKey = 0.0f;
};

struct RenderQueue {
    std::vector<DrawItem> shadowCasters;
    std::vector<DrawItem> opaque;
    std::vector<DrawItem> transparent;
    std::vector<DrawItem> overlay;
};
```

每帧流程不再是 Scene::render(cmd)，而是：

1. Scene 提供对象列表
2. RenderSceneBuilder 进行可见性收集和分类
3. 生成 RenderQueue
4. 各 Pass 消费各自关心的 DrawItem 列表

这一步是从“示例程序”进入“渲染器架构”的关键分水岭。

## 五、Mesh / Material / Texture 的职责划分

### 5.1 Mesh

Mesh 只负责几何数据和绑定，不负责“这个物体该怎么画”。

建议职责：

- 持有 vertex/index buffer
- 持有 submesh 信息
- 暴露 bounds，用于剔除
- 提供 bind(cmd) 和 draw(cmd, submeshIndex)

未来如果一个模型有多个 submesh，对应多个材质槽，那么 DrawItem 应当引用 submesh，而不是整个 mesh 对象。

### 5.2 Texture

Texture 应只表达 GPU 纹理资源本身：

- VkImage / VkImageView / VkSampler
- 格式、尺寸、mipLevels
- 资源状态和销毁逻辑

贴图路径、磁盘缓存、异步加载状态等不属于 Texture 本体，属于 ResourceManager 或 TextureAsset。

### 5.3 Material 与 MaterialInstance

当前 Material 已经同时承担：

- 管线创建
- 描述符布局
- 描述符池
- 纹理绑定
- 每帧 descriptor set 管理

这会在多物体、多材质时迅速膨胀。建议拆成两层：

#### MaterialTemplate

描述“如何渲染”：

- 使用哪些 shader
- 使用什么 blend/depth/cull/raster 状态
- 需要哪些资源槽位
- 属于哪个 Pass 域

#### MaterialInstance

描述“这份材质的参数”：

- 绑定哪些纹理
- baseColor / metallic / roughness 等参数
- 这份实例对应的 GPU descriptor 数据

两者关系如下：

- MaterialTemplate 类似“材质类型”
- MaterialInstance 类似“某个对象实际使用的材质实例”

这样可以避免每创建一个物体就复制一整套 pipeline 和 layout。

## 六、推荐的描述符与数据绑定策略

前向渲染器建议从一开始就按“全局 / 材质 / 物体”三层数据组织，否则后面很难扩展。

### 6.1 Set 划分建议

推荐约定：

- Set 0：Frame Global
- Set 1：Material
- Set 2：Object

建议数据如下：

#### Set 0：Frame Global

包含：

- View / Projection / ViewProjection
- CameraPosition
- 时间、屏幕尺寸
- 主方向光、环境参数
- 阴影图采样器入口

特点：

- 每帧只更新一次
- 每个 Pass 可共享一套结构，也可按 Pass 扩展字段

#### Set 1：Material

包含：

- Albedo / Normal / MetallicRoughness / Emissive 纹理
- 材质常量参数
- alphaMode、doubleSided 等开关数据

特点：

- 切换材质时更新绑定
- 可以由 MaterialInstance 持有长期 descriptor

#### Set 2：Object

包含：

- ModelMatrix
- NormalMatrix
- objectId / pickingId

对于前向渲染，Object 数据推荐优先使用两种方案之一：

1. Push Constants：适合少量对象矩阵数据
2. Dynamic UBO / SSBO：适合对象很多时批量上传

如果你的目标是后续扩展到上百上千对象，建议直接为 Object 数据预留动态缓冲方案，而不是把 model matrix 塞进每个 descriptor set。

## 七、Pass 系统设计

多 Pass 架构的关键不是“多建几个 VkRenderPass”，而是把每个渲染阶段的职责固定下来。

### 7.1 IRenderPass 接口

建议统一接口如下：

```cpp
class IRenderPass {
public:
    virtual ~IRenderPass() = default;

    virtual const char* name() const = 0;
    virtual void setup(RenderGraphBuilder& builder) = 0;
    virtual void execute(const RenderPassContext& context,
                         const RenderQueue& queue) = 0;
};
```

如果前期不做 RenderGraphBuilder，也可以先把 setup 简化成 createResources / resize，但接口形式最好先统一。

### 7.2 第一阶段推荐支持的 Pass

建议按以下顺序演进：

#### Pass 1：ForwardOpaquePass

职责：

- 渲染所有不透明物体
- 按 pipeline/material 排序
- 负责主颜色和深度输出

这是当前单通道 Renderer 最容易抽取出来的部分，应优先完成。

#### Pass 2：ForwardTransparentPass

职责：

- 渲染透明物体
- 按相机距离逆序排序
- 使用不同 blend state

这一步能验证“同一 Scene，多种排序策略，多套 pipeline”的架构是否成立。

#### Pass 3：ShadowPass

职责：

- 从光源视角生成 shadow map
- 只消费可投射阴影对象队列
- 输出独立深度纹理

这一步会真正测试 Pass 间资源传递能力。

#### Pass 4：PostProcessPass

职责：

- 读取主颜色附件或中间纹理
- 执行 tone mapping、gamma、FXAA、Bloom 等全屏效果

这一步会迫使 Renderer 从“直接渲染到 swapchain”升级到“渲染到中间目标再合成”。

### 7.3 Pass 与资源依赖

建议至少在设计上明确这些关系：

- ShadowPass 输出 shadowMap
- ForwardOpaquePass 读取 shadowMap，输出 hdrColor + depth
- TransparentPass 读取 depth，输出 hdrColor
- PostProcessPass 读取 hdrColor，输出 swapchainColor

即便第一阶段先用手写顺序执行，也应该把这些输入输出作为显式字段保留下来，避免后续重构时再次回到硬编码。

## 八、Renderer 的职责重组

新的 Renderer 不应直接等于“一个大杂烩 Vulkan 管理器”，而应缩到以下职责：

### 8.1 Renderer 负责的事

- 帧开始与帧结束
- 获取当前 RenderFrame
- 准备 RenderPassContext
- 驱动 RenderPipeline 执行各个 Pass
- 响应窗口 resize 并触发依赖资源重建

### 8.2 Renderer 不负责的事

- 不负责加载 OBJ 或纹理
- 不负责决定场景里有哪些对象
- 不负责保存某个固定材质的 descriptor set
- 不负责把 SceneObject 直接逐个 draw 出去

### 8.3 推荐接口

```cpp
class Renderer {
public:
    void render(const Scene& scene, const Camera& camera);
    void notifyResize();

private:
    RenderFrame& beginFrame();
    void endFrame(RenderFrame& frame);

    void buildRenderScene(const Scene& scene,
                          const Camera& camera,
                          RenderScene& renderScene);

    void buildRenderQueue(const RenderScene& renderScene,
                          RenderQueue& queue);

    void executePipeline(const RenderFrame& frame,
                         const RenderScene& renderScene,
                         const RenderQueue& queue);
};
```

注意这里多出两步：

- buildRenderScene
- buildRenderQueue

这两步能把“业务世界”和“GPU 提交”拆开，是架构稳定的关键。

## 九、Scene 层的建议演进

当前 SceneObject 已经有 mesh、material、transform，这个形态可以保留，但建议补上以下内容：

- 可见性标志
- 渲染层级或 renderLayer
- 是否投射阴影
- 包围盒 bounds
- 透明类型或 alphaMode
- 可选的 per-object 参数块

推荐结构：

```cpp
struct SceneObject {
    std::shared_ptr<Mesh> mesh;
    std::shared_ptr<MaterialInstance> material;
    Transform transform;

    Bounds bounds;
    bool castShadow = true;
    bool visible = true;
    uint32_t renderLayer = 0;
};
```

Scene 本身建议增加这些能力：

- 添加和删除对象
- 维护主相机或活动相机
- 提供 lights() / objects() / environment() 访问器
- 后续可挂接空间索引结构用于剔除

## 十、为 Deferred 与 PBR 留接口的具体策略

这部分不能只停留在“以后可扩展”的口号上，应该明确哪些设计点必须现在就做好。

### 10.1 不把 Pipeline 写死在单一 RenderPass 上

如果 Pipeline 创建接口把 renderPass、descriptor layout、shader 文件路径全写死在 Material 里，那么以后切换到 G-Buffer 或 Shadow Pass 时会非常痛苦。

建议改为：

- Pipeline 由 PipelineKey 创建
- PipelineKey 包含 passType、shaderVariant、vertexLayout、blend/depth 状态
- MaterialTemplate 只描述需求，不直接生成唯一固定 Pipeline

### 10.2 材质参数使用标准化槽位

为 PBR 预留时，建议直接约定材质参数结构：

- baseColorFactor
- emissiveFactor
- metallicFactor
- roughnessFactor
- normalScale
- alphaCutoff

即便当前 shader 只用到 baseColor，也要把结构设计成可扩展，而不是后续不断重排 descriptor。

### 10.3 全局光照数据单独组织

不要把光源数据散落在各个 Material 中。建议有单独的 FrameLightingData：

- directionalLights
- pointLights
- spotLights
- shadow settings
- environment settings

这样 Forward 和 Deferred 都能共用同一份 CPU 侧输入。

### 10.4 中间渲染目标抽象成 RenderTarget

未来需要：

- HDR Color
- Depth
- Shadow Map
- G-Buffer A/B/C
- PostProcess Ping-Pong

因此建议尽早引入 RenderTarget 或 AttachmentHandle 抽象，而不是让每个 Pass 自己散落持有若干 Image。

## 十一、推荐的每帧执行流程

下面给出更贴近目标架构的每帧主循环：

```cpp
void Application::tick() {
    input_->update();
    updateCamera();
    updateSceneLogic();

    renderer_->render(scene_, camera_);
}
```

Renderer 内部流程建议为：

```cpp
void Renderer::render(const Scene& scene, const Camera& camera) {
    RenderFrame& frame = beginFrame();
    if (!frame.commandBuffer) {
        return;
    }

    RenderScene renderScene;
    buildRenderScene(scene, camera, renderScene);

    updateGlobalFrameData(frame, renderScene);

    RenderQueue queue;
    buildRenderQueue(renderScene, queue);

    for (IRenderPass* pass : renderPipeline_->passes()) {
        pass->execute(buildPassContext(frame), queue);
    }

    endFrame(frame);
}
```

这个流程的优势在于：

- 同一 Scene 可被不同 RenderPipeline 消费
- 同一个 Renderer 可切换 Forward / Deferred 管线实现
- Pass 插拔性更强，后处理与阴影更容易加进来

## 十二、从当前代码到目标架构的建议演进顺序

为了避免一次性重构过大，建议按下面顺序推进。

### 12.1 第一步：让 Application 只做组装

当前 app.cpp 里还有较多渲染过程细节，建议先把这些逻辑缩到 Renderer：

- beginRenderPass / endRenderPass 从 app.cpp 移出
- viewport / scissor 设置移入具体 Pass
- updateUniformBuffer 从 Application 移到 Renderer 或 FrameDataUpdater

目标是让 app.cpp 只剩：输入更新、相机更新、scene 更新、renderer.render(scene, camera)

### 12.2 第二步：从 Scene::render 切换到 RenderQueue

当前 Scene::render 仍然是“直接遍历对象并 bind/draw”。建议先新增：

- RenderScene
- DrawItem
- RenderQueue

然后改成：

- Scene 不再直接发 draw
- Renderer 负责收集 queue
- ForwardOpaquePass 负责消费 queue.opaque

### 12.3 第三步：拆分 Material

把当前 Material 拆成：

- MaterialTemplate
- MaterialInstance
- PipelineCache

这是支持多材质和 shader variant 的前提。

### 12.4 第四步：引入 Pass 列表

先不要急着做完整 Frame Graph。建议先让 Renderer 持有：

- std::vector<std::unique_ptr<IRenderPass>> passes_

按顺序执行即可。只要 Pass 的输入输出接口先立住，后面接 RenderGraph 就会容易很多。

### 12.5 第五步：抽象 RenderTarget

当需要阴影或后处理时，再把颜色、深度、中间附件抽象成 RenderTarget/Attachment，由各 Pass 声明读写关系。

## 十三、当前代码中的直接改造重点

结合现有实现，建议优先关注以下几个点：

### 13.1 Renderer

当前 Renderer 已经有 FrameData、command pool、render pass、framebuffer、swapchain recreate 等内容，是最合适的演进中心。但要避免继续往里塞 Mesh/Material 特定逻辑。

### 13.2 Material

当前 Material 直接依赖 Renderer 的 renderPass 和 uniformBuffer，这种依赖方向需要尽快扭转。更合理的方向是：

- Material 依赖 pipeline layout 约定
- Pass 决定用哪种 pipeline variant
- Renderer 只提供 frame context 和 descriptor 分配能力

### 13.3 Scene

当前 Scene 只是一组 SceneObject。短期内不用推翻，但建议去掉直接 render(cmd, frameIndex) 这种接口，把它变成纯数据容器。

### 13.4 Camera

Camera 当前实现已经比较独立，适合作为全局帧数据输入的一部分。建议后续把 View、Proj、CameraPosition 统一打包到 FrameGlobalData。

## 十四、结论

对于这个项目，新的程序架构不应理解成“把 HelloTriangleApplication 拆成更多类”，而应理解成建立一条稳定的渲染数据通路：

1. Scene 负责表达世界
2. RenderScene / RenderQueue 负责把世界转成可绘制数据
3. Renderer 负责一帧调度
4. Pass 负责执行具体渲染阶段
5. ResourceManager 负责共享资源
6. Core/RHI 负责 Vulkan 低层对象与提交

只要这六层边界建立起来，多物体、多材质、多 Pass 会自然成立，而 Deferred Rendering、PBR、Shadow Map、后处理这些能力也都能在现有架构上增量接入，而不是重新推翻一遍。

从现阶段实现成本和收益比来看，最值得优先落地的三个改造点是：

1. 让 Renderer 统一接管 render(scene, camera)
2. 用 RenderQueue 替代 Scene::render 的直接绘制
3. 把 Material 拆成模板与实例两层

只要这三步完成，这个项目就会从“教学示例封装”正式进入“可拓展渲染器骨架”的状态。