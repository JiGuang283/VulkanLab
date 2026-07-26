# 渲染流程

> Status: Current
> Last verified: 2026-07-26
> Verified against: `bfb50ef`

## 帧图与 Pass 顺序

Renderer 当前采用显式的三段 Forward 帧图，不依赖 RHI 或 RenderGraph：

```text
DirectionalShadowPass
        -> shadow depth
MainForwardPass
        -> linear HDR color
ToneMapPass + ImGui
        -> swapchain / capture
```

`RenderResourceRegistry` 使用稳定的类型化 handle 管理内部 render target 和 sampler。资源描述明确指定 fixed/swapchain-relative extent、single/per-frame multiplicity、format、sample count、usage 与 aspect。当前注册 HDR resolve、可选 HDR MSAA、main depth、2048x2048 directional shadow depth，以及 HDR/shadow sampler。每个 per-frame image 按 `MAX_FRAMES_IN_FLIGHT` 分配；HDR 优先使用 `R16G16B16A16_SFLOAT`，不满足 color attachment 与 sampled 要求时回退到 `R32G32B32A32_SFLOAT`。HDR sample count 取 color/depth format 和设备能力的交集，Shadow 与 ToneMap 固定为 1x。

每个 Pass 通过 `resourceUsages()` 声明 attachment write、sampled read、required/final layout。`RenderPipeline` 在初始化和 resize 后验证 handle、usage flag、sample/aspect、read-before-write 与相邻 layout 契约。Registry 不插入 barrier、不推导 lifetime、不重排 Pass；`RenderPipeline` 仍按 Shadow、Forward、ToneMap 顺序记录到同一个 frame command buffer，实际同步由 render-pass final/initial layout 和明确 dependency 完成。

Application 每帧只组装 `RenderViewInput`。纯函数 `buildRenderView()` 负责默认 Sun 规则、灯光截断与 GPU 打包、阴影矩阵计算，输出不可变 `RenderView`。Renderer 接收该对象并上传其中的 `GlobalFrameUbo`；Pass 通过 `RenderFrameContext::view` 读取同一份 settings 和 shadow 数据。

## RenderQueue 与 Forward

`Scene::collectRenderCommands()` 为每个 SceneObject 生成 Mesh、MaterialInstance、world transform 和 queue 类型。材质满足 `alphaMode == Blend` 或 `transmissionFactor > 0` 时进入 Transparent，其余 Opaque 与 Mask 材质进入 Opaque。

- Opaque 使用 MaterialTemplate、MaterialInstance、Mesh 地址排序，减少 pipeline、descriptor 和 vertex/index buffer 切换。
- Transparent 使用对象 world translation 到相机的距离从远到近排序。这是对象级近似，没有使用 mesh bounds，也不是 order-independent transparency。

MainForwardPass 清空 HDR color/depth，先画 Opaque/Mask，再画 Transparent。它不再写 swapchain，也不再绘制 ImGui。

| 队列 | Blending | Depth test | Depth write |
|---|---:|---:|---:|
| Opaque/Mask | 关闭 | 开启 | 开启 |
| Transparent | 开启 | 开启 | 关闭 |

`doubleSided=false` 使用 back-face culling；`doubleSided=true` 使用 `VK_CULL_MODE_NONE`。相关 fragment shader 通过 `gl_FrontFacing` 修正背面法线。

## 方向光阴影

`buildRenderView()` 从实际上传的第一盏 Directional light 生成 `DirectionalShadowFrameData`。场景没有显式灯光时使用默认 Sun；场景有灯光但没有 Directional light 时不生成阴影。无有效 bounds、无有效光方向或关闭 Shadows 时，ShadowPass 仍清除目标，但 Forward shader 不采样阴影贡献。

阴影相机使用场景 AABB 的 8 个角点拟合：light view 看向 bounds center，XY 增加 5% padding、Z 增加 10% padding，并将 XY center 对齐到 shadow texel。投影使用 Vulkan `[0,1]` 深度的正交 ZO 矩阵。

DirectionalShadowPass 的 caster 规则为：

- Opaque 使用 vertex-only depth pipeline。
- MASK 使用 fragment shader，按 BaseColor texture/factor、vertex color 和 alpha cutoff 执行 discard。
- BLEND 与 transmission 不投射阴影。
- `doubleSided` 继续控制 back cull 或 no cull。

PBR-lite Forward 与 PBR-lite NormalMapped 使用 comparison sampler 和 3x3 PCF。阴影只乘到第一盏 Directional light 的 direct contribution；ambient、emissive、Point 和 Spot lighting 不受影响。透明材质可以接收阴影。Raster constant/slope bias 与 shader receiver bias 均可通过 `VulkanLab -> Render -> Lighting` 或 Runtime Control 调节。

`Debug Shadow` 输出最终 visibility 灰度，用于检查投影范围、bias 和 PCF；它和其他 Debug variant 一样不经过 tone mapping。

## HDR 与 Tone Mapping

MainForwardPass 输出线性 HDR；MSAA 开启时 resolve 到单采样 HDR image，并转换为 `SHADER_READ_ONLY_OPTIMAL`。ToneMapPass 使用无 vertex buffer 的 fullscreen triangle 采样当前 frame slot 的 HDR image。

- PBR-lite 两个 variant 先应用 `color *= exp2(exposureEv)`，再按设置执行 ACES fitted、Reinhard 或 PassThrough。
- Legacy 和所有 Debug variant 强制 PassThrough，以维持基线和材质通道语义。
- sRGB swapchain 由硬件进行线性到 sRGB 编码；非 sRGB UNORM swapchain 由 ToneMap shader 显式 gamma encode。
- ImGui 在 fullscreen draw 之后写入同一个 ToneMap render pass，因此 UI 不受曝光和 tone mapping 影响。

ToneMapPass 最终 layout 为 `PRESENT_SRC_KHR`。异步截图继续复制最终 swapchain image，因此捕获结果包含 tone mapping，并可按请求包含或排除 ImGui。

## Pipeline、材质与 Descriptor

MaterialTemplate 保存基础 PipelineConfig 和材质 descriptor layout。MaterialInstance 保存材质参数及 BaseColor、Normal、MetallicRoughness、Occlusion、Emissive 五个纹理槽。缺失槽由 fallback texture 填充，因此 `VulkanLab -> Materials` 中的 Bound 只表示 descriptor 已绑定。

PipelineConfig 支持零或多个 color blend attachment、零 vertex binding、可选 fragment shader、topology、subpass 和 depth bias。`PipelineCache::getOrCreate()` 只接收 render pass 与完整 `PipelineConfig`，由 cache 内部规范化并生成 key。Key 覆盖 shader 路径、vertex layout、topology、raster/depth/blend/MSAA 状态、descriptor layouts、push ranges、render pass 和 subpass；不再包含 pass、材质指针、ShaderVariant、queue 或 alpha-masked 等语义标签。Pipeline 创建直接使用 key 内保存的 config，因此不存在手工 key 与实际 Vulkan 状态分叉。

Forward descriptor 约定为：

- `set=0, binding=0`：每帧 GlobalUBO，包含相机、光源和 directional shadow 数据。
- `set=1, binding=0..4`：五个材质纹理槽。
- `set=2, binding=0`：当前 frame slot 的 comparison shadow map。
- 128 字节 push constant：model matrix 和材质因子。

ToneMap 使用独立的 pass-local source texture descriptor layout，不复用材质 layout。

## Shader Variant

`shader/manifest.json` 是 Shader program 和 selectable variant 的唯一权威清单。CMake 在配置阶段读取 Manifest、去重所有 stage 源文件，并为 `glslc` 配置 include 路径和依赖；每个产物必须先通过 `spirv-val`，再从 `generated/<Config>/shader/` stage 到 runtime `shader/`。Manifest 本身也进入开发 runtime 和 Cook package。

Application 在创建 Window/Vulkan 前加载 `ShaderRegistry`。当前选择使用稳定 variant ID，UI 使用 display name；ToneMap 是否可配置由 variant metadata 决定。Shadow 与 ToneMap 通过稳定 program ID 查询，不再维护 C++ 路径常量。MaterialTemplate 的基础 PipelineConfig 不携带默认 Shader，MainForwardPass 在创建 pipeline 前必须写入当前 variant 路径。

测试目标静态链接固定版本的 SPIRV-Reflect，按 Manifest program contract 遍历全部 Forward、Shadow 与 ToneMap program，校验 stage、descriptor、UBO/push size 和 member offset、vertex location/format、跨阶段 varying及 fragment output。反射不进入 VulkanLab 运行时，也不自动生成 DescriptorSetLayout；生产布局仍由显式 C++ 代码创建。

当前 variant 包含 Legacy、两个 PBR-lite、BaseColor/Normal/Roughness/Metallic/Occlusion/Emissive/Alpha/Transmission 调试视图，以及 `Debug Shadow`。启动默认使用 `PBR-lite NormalMapped`；Legacy 保留为显式基线和兼容性检查。PBR-lite 使用 baseColor、metallicRoughness、AO 和 emissive；NormalMapped 额外使用 tangent/TBN 与 normal scale。Transmission 当前仍是 alpha 与 Fresnel 轮廓近似，不采样场景颜色。

新增兼容 Main Forward ABI 的 variant 只需增加 GLSL 和 Manifest 条目；构建、运行时 UI、Cook 和 contract tests 会自动包含它。当前不支持目录扫描、热重载或第三方 Shader 插件，具体流程见 [Shader Registry](../guides/shader_registry.md)。

顶点布局固定为 position、normal、UV0、tangent、UV1 和 vertex color，location 为 0 到 5。AO 可选择 UV0/UV1；其他纹理当前使用 UV0。

## 光源

SceneLight 支持 Directional、Point 和 Spot。GlobalUBO 最多上传 1 个 directional light 和 8 个 punctual lights；Point 与 Spot 共用 punctual 配额。当前 glTF loader 不解析 `KHR_lights_punctual`。环境项由 ambient color/intensity 提供，AO 只影响 ambient。

当前只支持一张方向光 shadow map；没有 CSM、Point/Spot shadow、IBL、skybox、deferred rendering、bloom 或 auto exposure。

## GPU Pass 计时

Renderer 持有一个 `GpuPassProfiler` 和 timestamp query pool。每个 frame slot 为 `DirectionalShadow`、`MainForward`、`ToneMap + UI` 分配 begin/end query；ToneMap 区间包含同一 render pass 内的 ImGui draw。总时间从第一个 Pass begin 到最后一个 Pass end 计算。

`FrameSync::beginFrame()` 已等待对应 slot 的 fence 后，Profiler 才使用不带 `WAIT_BIT` 的 `vkGetQueryPoolResults()` 读取旧结果，然后在新 command buffer 中 reset 该 slot。计时不会增加 queue/device idle 或额外 fence wait。换算使用设备 `timestampPeriod`，并按 graphics queue 的 `timestampValidBits` 处理计数器回绕；不支持 timestamp 的设备返回 `available=false`，渲染继续运行。结果显示在 `VulkanLab -> Diagnostics -> Performance`，并由 `render.status.gpuTimings` 返回。

## Swapchain 截图

开发运行时提供异步 PNG 截图，入口为 `VulkanLab -> Diagnostics -> Capture`、F12 或 Runtime Control v3。ToneMapPass 结束后，截图在同一个 frame command buffer 中执行 `PRESENT -> TRANSFER_SRC`、image-to-buffer copy 和 `TRANSFER_SRC -> PRESENT`。

FrameSync 使用单调 submission serial 和正常 frame fence 管理 readback 生命周期。CPU worker 只处理 RGBA bytes、PNG 和 SHA-256，不访问 Vulkan、GLFW、ImGui 或 Scene。截图路径不调用 `vkQueueWaitIdle()` 或 `vkDeviceWaitIdle()`。

## Resize 生命周期

resize 时 Renderer 等待 device idle，然后按以下顺序处理：

1. 逆序调用 pass 的 `releaseSwapChainResources()`，先释放持有 swapchain/HDR image view 的 framebuffer 和 descriptor 引用。
2. 重建 SwapChain。
3. 由 Registry 重建 extent-dependent HDR color、MSAA color 和 depth targets；fixed 2048 的 shadow map 不重建。
4. 调用 pass `onResize()` 重建 framebuffer 和 HDR source descriptor。
5. 清空 PipelineCache，更新 GuiSystem、FrameSync 与相机 aspect ratio。

窗口最小化导致 framebuffer extent 为 0 时会延迟重建，并以短暂 sleep 保持主循环和 Runtime Control 可响应。
