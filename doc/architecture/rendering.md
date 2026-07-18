# 渲染流程

> Status: Current
> Last verified: 2026-07-18
> Verified against: `0516951`

## RenderQueue

`Scene::collectRenderCommands()` 为每个 SceneObject 生成 Mesh、MaterialInstance、world transform 和 queue 类型。材质满足 `alphaMode == Blend` 或 `transmissionFactor > 0` 时进入 Transparent，其余 Opaque 与 Mask 材质进入 Opaque。

- Opaque 使用 MaterialTemplate、MaterialInstance、Mesh 地址排序，减少 pipeline、descriptor 和 vertex/index buffer 切换。
- Transparent 使用对象 world transform 的 translation 到相机的距离，从远到近排序。这是对象级近似，没有使用 mesh bounds，也不是 order-independent transparency。

## RenderPipeline 与 MainForwardPass

Renderer 当前创建一个 RenderPipeline，其中只有 MainForwardPass。该 pass 管理 MSAA color、depth、framebuffer 和 Vulkan render pass，按以下顺序记录命令：

1. 清空 color 与 depth。
2. 绘制 Opaque/Mask 队列。
3. 绘制 Transparent 队列。
4. 绘制 ImGui。

队列决定 pipeline 状态：

| 队列 | Blending | Depth test | Depth write |
|---|---:|---:|---:|
| Opaque/Mask | 关闭 | 开启 | 开启 |
| Transparent | 开启 | 开启 | 关闭 |

`doubleSided=false` 使用 back-face culling；`doubleSided=true` 使用 `VK_CULL_MODE_NONE`。相关 fragment shader 通过 `gl_FrontFacing` 修正背面法线。

## Pipeline 与材质

MaterialTemplate 保存基础 PipelineConfig 和材质 descriptor layout。MaterialInstance 保存材质参数及五个纹理槽：BaseColor、Normal、MetallicRoughness、Occlusion、Emissive。缺失槽由 fallback texture 填充，所以 Materials 面板中的 Bound 只表示 descriptor 已绑定，不代表模型原本提供了贴图。

MainForwardPass 在绘制时用当前 ShaderVariant 覆盖模板中的 Shader 路径，并根据队列和 `doubleSided` 覆盖 blend、depth write 与 cull mode。PipelineCache 的 key 包含 MaterialTemplate、pass、ShaderVariant、queue、cull mode、render pass、subpass 和 sample count，防止状态不兼容的 pipeline 被错误复用。

Descriptor 约定为：

- `set=0, binding=0`：每帧 GlobalUBO，vertex 和 fragment shader 可见。
- `set=1, binding=0..4`：五个材质纹理槽。
- 128 字节 push constant：model matrix、base color、emissive/metallic、roughness/alpha/AO 和 alpha/transmission/normal scale 参数。

## Shader Variant

当前可运行时切换：

- `Legacy Forward`
- `PBR-lite Forward`
- `PBR-lite NormalMapped`
- `Debug BaseColor`
- `Debug Normal`
- `Debug Roughness`
- `Debug Metallic`
- `Debug Occlusion`
- `Debug Emissive`
- `Debug Alpha`
- `Debug Transmission`

PBR-lite 使用 baseColor、metallicRoughness、AO 和 emissive；NormalMapped 额外使用 tangent/TBN 与 normal scale。传输材质当前只做 alpha 与 Fresnel 轮廓近似，不采样场景颜色，也不实现真实折射、volume absorption 或 rough transmission。

顶点布局固定为 position、normal、UV0、tangent、UV1 和 vertex color，location 分别为 0 到 5。AO 可按材质选择 UV0/UV1；其他纹理当前使用 UV0。

## 光源

SceneLight 支持 Directional、Point 和 Spot。GlobalUBO 最多上传 1 个 directional light 和 8 个 punctual lights，Point 与 Spot 共用 punctual 配额；超出的灯会被忽略并记录 warning。当前 glTF loader 不解析 `KHR_lights_punctual`，因此场景没有显式灯光时，Application 使用 UI 可调的默认 directional sun。环境项由 ambient color/intensity 提供，AO 只影响 ambient。

当前没有 shadow pass、IBL、skybox、deferred rendering 或 postprocess。

## Resize

窗口 resize 先由 FrameSync 标记。在 acquire/present 报告需要重建后，Renderer 等待 device idle，重建 SwapChain，并通知 RenderPipeline 重建 color、depth 和 framebuffer；随后清空 PipelineCache、重建帧同步所需状态、通知 GuiSystem 并更新相机 aspect ratio。
