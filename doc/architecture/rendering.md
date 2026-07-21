# 渲染流程

> Status: Current
> Last verified: 2026-07-21
> Verified against: current working tree based on `a154f52`

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

`FrameRenderTargets` 按 `MAX_FRAMES_IN_FLIGHT` 分配资源。每个 frame slot 有一张单采样 HDR resolve image、一张 depth image、一张 2048x2048 shadow depth image；设备支持 MSAA 时还会有一张多采样 HDR color image。HDR 优先使用 `R16G16B16A16_SFLOAT`，不满足 color attachment 与 sampled 要求时回退到 `R32G32B32A32_SFLOAT`。HDR sample count 取 color/depth format 和设备能力的交集，Shadow 与 ToneMap 固定为 1x。

`RenderPipeline` 按 Shadow、Forward、ToneMap 顺序记录到同一个 frame command buffer。Pass 间通过 render-pass final/initial layout 和明确 dependency 完成 shadow-write 到 fragment-read、HDR-write 到 fragment-read 的同步。

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

Application 从实际上传的第一盏 Directional light 生成 `DirectionalShadowFrameData`。场景没有显式灯光时使用默认 Sun；场景有灯光但没有 Directional light 时不生成阴影。无有效 bounds、无有效光方向或关闭 Shadows 时，ShadowPass 仍清除目标，但 Forward shader 不采样阴影贡献。

阴影相机使用场景 AABB 的 8 个角点拟合：light view 看向 bounds center，XY 增加 5% padding、Z 增加 10% padding，并将 XY center 对齐到 shadow texel。投影使用 Vulkan `[0,1]` 深度的正交 ZO 矩阵。

DirectionalShadowPass 的 caster 规则为：

- Opaque 使用 vertex-only depth pipeline。
- MASK 使用 fragment shader，按 BaseColor texture/factor、vertex color 和 alpha cutoff 执行 discard。
- BLEND 与 transmission 不投射阴影。
- `doubleSided` 继续控制 back cull 或 no cull。

PBR-lite Forward 与 PBR-lite NormalMapped 使用 comparison sampler 和 3x3 PCF。阴影只乘到第一盏 Directional light 的 direct contribution；ambient、emissive、Point 和 Spot lighting 不受影响。透明材质可以接收阴影。Raster constant/slope bias 与 shader receiver bias 均可通过 Lighting 面板或 Runtime Control 调节。

`Debug Shadow` 输出最终 visibility 灰度，用于检查投影范围、bias 和 PCF；它和其他 Debug variant 一样不经过 tone mapping。

## HDR 与 Tone Mapping

MainForwardPass 输出线性 HDR；MSAA 开启时 resolve 到单采样 HDR image，并转换为 `SHADER_READ_ONLY_OPTIMAL`。ToneMapPass 使用无 vertex buffer 的 fullscreen triangle 采样当前 frame slot 的 HDR image。

- PBR-lite 两个 variant 先应用 `color *= exp2(exposureEv)`，再按设置执行 ACES fitted、Reinhard 或 PassThrough。
- Legacy 和所有 Debug variant 强制 PassThrough，以维持基线和材质通道语义。
- sRGB swapchain 由硬件进行线性到 sRGB 编码；非 sRGB UNORM swapchain 由 ToneMap shader 显式 gamma encode。
- ImGui 在 fullscreen draw 之后写入同一个 ToneMap render pass，因此 UI 不受曝光和 tone mapping 影响。

ToneMapPass 最终 layout 为 `PRESENT_SRC_KHR`。异步截图继续复制最终 swapchain image，因此捕获结果包含 tone mapping，并可按请求包含或排除 ImGui。

## Pipeline、材质与 Descriptor

MaterialTemplate 保存基础 PipelineConfig 和材质 descriptor layout。MaterialInstance 保存材质参数及 BaseColor、Normal、MetallicRoughness、Occlusion、Emissive 五个纹理槽。缺失槽由 fallback texture 填充，因此 Materials 面板中的 Bound 只表示 descriptor 已绑定。

PipelineConfig 支持零或多个 color blend attachment、零 vertex binding、可选 fragment shader、topology、subpass 和 depth bias。PipelineCache key 包含 pass、ShaderVariant、queue、cull mode、alpha-masked 状态、render pass、subpass 和 sample count，避免 Shadow、Forward 与 ToneMap pipeline 错误复用。

Forward descriptor 约定为：

- `set=0, binding=0`：每帧 GlobalUBO，包含相机、光源和 directional shadow 数据。
- `set=1, binding=0..4`：五个材质纹理槽。
- `set=2, binding=0`：当前 frame slot 的 comparison shadow map。
- 128 字节 push constant：model matrix 和材质因子。

ToneMap 使用独立的 pass-local source texture descriptor layout，不复用材质 layout。

## Shader Variant

`shader/CMakeLists.txt` 是 shader source list 的唯一所有者。GLSL 编译到 `generated/<Config>/shader/` 后 stage 到 runtime `shader/`；开发运行和 Cook package 使用相同 SPIR-V。Cook 除 variant 路径外，还显式包含 Shadow 与 ToneMap 的内部 shader。

当前 variant 包含 Legacy、两个 PBR-lite、BaseColor/Normal/Roughness/Metallic/Occlusion/Emissive/Alpha/Transmission 调试视图，以及 `Debug Shadow`。PBR-lite 使用 baseColor、metallicRoughness、AO 和 emissive；NormalMapped 额外使用 tangent/TBN 与 normal scale。Transmission 当前仍是 alpha 与 Fresnel 轮廓近似，不采样场景颜色。

顶点布局固定为 position、normal、UV0、tangent、UV1 和 vertex color，location 为 0 到 5。AO 可选择 UV0/UV1；其他纹理当前使用 UV0。

## 光源

SceneLight 支持 Directional、Point 和 Spot。GlobalUBO 最多上传 1 个 directional light 和 8 个 punctual lights；Point 与 Spot 共用 punctual 配额。当前 glTF loader 不解析 `KHR_lights_punctual`。环境项由 ambient color/intensity 提供，AO 只影响 ambient。

当前只支持一张方向光 shadow map；没有 CSM、Point/Spot shadow、IBL、skybox、deferred rendering、bloom 或 auto exposure。

## Swapchain 截图

开发运行时提供异步 PNG 截图，入口为 Capture 面板、F12 或 Runtime Control v3。ToneMapPass 结束后，截图在同一个 frame command buffer 中执行 `PRESENT -> TRANSFER_SRC`、image-to-buffer copy 和 `TRANSFER_SRC -> PRESENT`。

FrameSync 使用单调 submission serial 和正常 frame fence 管理 readback 生命周期。CPU worker 只处理 RGBA bytes、PNG 和 SHA-256，不访问 Vulkan、GLFW、ImGui 或 Scene。截图路径不调用 `vkQueueWaitIdle()` 或 `vkDeviceWaitIdle()`。

## Resize 生命周期

resize 时 Renderer 等待 device idle，然后按以下顺序处理：

1. 逆序调用 pass 的 `releaseSwapChainResources()`，先释放持有 swapchain/HDR image view 的 framebuffer 和 descriptor 引用。
2. 重建 SwapChain。
3. 重建 extent-dependent HDR color、MSAA color 和 depth targets；固定 2048 的 shadow map 不重建。
4. 调用 pass `onResize()` 重建 framebuffer 和 HDR source descriptor。
5. 清空 PipelineCache，更新 GuiSystem、FrameSync 与相机 aspect ratio。

窗口最小化导致 framebuffer extent 为 0 时会延迟重建，并以短暂 sleep 保持主循环和 Runtime Control 可响应。
