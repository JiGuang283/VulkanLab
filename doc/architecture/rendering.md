# 渲染流程

> Status: Current
> Last verified: 2026-07-20
> Verified against: `b925b2d`

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

`shader/CMakeLists.txt` 是 Shader source list 的唯一所有者。每个 GLSL 文件通过独立的 `add_custom_command(OUTPUT ...)` 生成到 `generated/<Config>/shader/`，再由同一依赖图 stage 到 runtime `shader/`；`VulkanLab` 和 `VulkanLabAssetTool` 都依赖 `VulkanLabShaders`。`ShaderVariant` 继续只保存 runtime 相对路径，因此 UI、Runtime Control 和 Cook 使用同一套 SPIR-V，不读取源码树生成物。

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

## Swapchain 截图

开发运行时提供异步 PNG 截图，入口为 `Capture` 面板或 F12。Cooked package 当前不创建 CaptureService。截图根目录由 `ProjectContext::captureRoot` 提供；输出路径必须是该目录下的相对 `.png` 路径。

- SwapChain 只有同时支持 `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` 和 8-bit RGBA/BGRA UNORM/SRGB format 时才启用截图。
- MainForwardPass 结束后，截图在同一个 frame command buffer 中执行 `PRESENT -> TRANSFER_SRC`、image-to-buffer copy 和 `TRANSFER_SRC -> PRESENT`。
- FrameSync 为每次 graphics submit 分配单调 serial。只有对应 frame fence 已完成、completed serial 推进后，主线程才 invalidate readback buffer 并复制 CPU bytes。
- BGRA/RGBA 转换、PNG 编码和 SHA-256 在惰性启动的 worker 中执行；worker 不访问 Vulkan、GLFW、ImGui 或 Scene。
- 同时最多有一个 Recording/WaitingForGpu task，非终态 task 上限为 8，终态历史上限为 32。取消已提交 task 时仍先等待其正常 GPU 完成。
- `includeGui=false` 时只丢弃截图目标帧的 ImGui draw，不破坏下一帧 backend 状态。

截图提交和读回路径不调用 `vkQueueWaitIdle()` 或 `vkDeviceWaitIdle()`；正常 frame fence、resize 已有的 device-idle 和应用关闭同步不属于截图等待路径。

## Resize

窗口 resize 先由 FrameSync 标记。在 acquire/present 报告需要重建后，Renderer 等待 device idle，重建 SwapChain，并通知 RenderPipeline 重建 color、depth 和 framebuffer；随后推进已完成 submission serial、处理旧 SwapChain 上的截图读回、清空 PipelineCache、重建帧同步所需状态、通知 GuiSystem 并更新相机 aspect ratio。窗口最小化导致 framebuffer extent 为 0 时会延迟重建，并以短暂 sleep 保持主循环和 Runtime Control 可响应。
