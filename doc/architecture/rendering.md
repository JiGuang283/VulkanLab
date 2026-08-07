# 渲染流程

> Status: Current
> Last verified: 2026-08-07
> Verified against: Screen-space foundations, AO backends, TAA, GTAO, SSR, SSGI and Local Reflection Probes v1

## 帧图与 Pass 顺序

Renderer 当前采用显式的 Forward + Compute 帧图，不依赖 RHI 或 RenderGraph：

```text
AtmosphereLutPass
        -> transmittance / multiple scattering / sky view / aerial perspective
DirectionalShadowPass
        -> shadow depth
SurfacePrepass
        -> sampled depth + normal/roughness + motion/history + albedo/metallic
HiZBuildPass
        -> max-depth mip pyramid
OcclusionCullPass
        -> per-draw indirect instanceCount
ScreenDepthPyramidPass
        -> nearest/min-depth mip chain when requested
SsaoPass
        -> half-resolution raw + bilateral-filtered AO
SkyBackgroundPass
        -> procedural atmosphere / skybox / clear
MainForwardPass (Opaque)
        -> opaque HDR + baseline indirect specular + baseline indirect diffuse
SceneColorPyramidPass
        -> opaque HDR color mip chain when requested
SsrPass
        -> half-resolution trace + temporal + bilateral filter
SsgiPass
        -> half-resolution diffuse trace + temporal moments + A-Trous filter
ScreenSpaceLightingComposite
        -> replace baseline specular/diffuse according to confidence
MainForwardPass (Transparent)
        -> blend over composited HDR with read-only Surface Depth
TaaPass
        -> optional temporal resolve of complete scene color
BloomPass
        -> half-resolution bloom pyramid
ToneMapPass
        -> per-frame Viewport Color
PresentPass + ImGui
        -> swapchain / Workspace capture
```

`RenderResourceRegistry` 使用稳定的类型化 handle 管理内部 render target 和 sampler。资源描述明确指定 fixed/viewport-relative extent、相对尺寸除数、single/per-frame multiplicity、format、sample count、usage、aspect、array layer、view type、history capability 和 fixed/full mip policy。当前注册 HDR resolve、可选 HDR MSAA、baseline indirect specular/diffuse 及其可选 MSAA resolve、composited HDR、main depth、2048x2048 directional shadow depth、Surface Depth、`RGBA16F` world normal/roughness/history validity、`RG16F` motion、`RGBA8_UNORM` linear albedo/metallic、用于遮挡剔除的 `R32_SFLOAT` max-depth Hi-Z、用于屏幕空间效果的 `R32_SFLOAT` nearest-depth pyramid、`RGBA16F` Scene Color pyramid、三张半分辨率 `R16_SFLOAT` SSAO 图像、五张半分辨率 GTAO 图像、五张半分辨率 `RGBA16F` SSR 图像、六张半分辨率 `RGBA16F` SSGI 图像、最多六级 Bloom 图像、LDR Viewport Color，以及四张 Atmosphere LUT 和相关 sampler。每个 per-frame image 按 `MAX_FRAMES_IN_FLIGHT` 分配；HDR 优先使用 `R16G16B16A16_SFLOAT`，不满足 color attachment 与 sampled 要求时回退到 `R32G32B32A32_SFLOAT`。Viewport Color 使用 Swapchain format，以延续既有 sRGB/gamma 行为。HDR sample count 取 color/depth format 和设备能力的交集，Surface Data、Atmosphere、Shadow、屏幕空间效果、Bloom、ToneMap 与 Present 固定为 1x。

每个 Pass 通过 `resourceUsages()` 声明 attachment write/read、sampled read、storage write/read-write、transfer read/write、required/final layout。`RenderPipeline` 在初始化和 resize 后验证 handle、usage flag、sample/aspect、read-before-write 与相邻 layout 契约。Registry 不插入 barrier、不推导 lifetime、不重排 Pass；`RenderPipeline` 仍按 Atmosphere LUT、Shadow、Surface Prepass、Visibility Hi-Z、Occlusion、Screen Depth Pyramid、SSAO/GTAO/CACAO、Sky Background、Opaque Forward、Scene Color Pyramid、SSR、SSGI、Screen-Space Lighting Composite、Transparent Forward、TAA、Bloom、ToneMap、Present 顺序记录到同一个 frame command buffer。Render pass 之间使用 final/initial layout 和 dependency；compute pass 通过集中式经典 Vulkan barrier helper明确记录逐 mip、temporal resolve 和逐滤波阶段的 image barrier，以及 Occlusion SSBO write 到 indirect-command read 的 buffer barrier。

Application 每帧只组装 `RenderViewInput`。纯函数 `buildRenderView()` 负责默认 Sun 规则、灯光过滤与分组、稳定阴影 caster、Atmosphere Sun 选择、阴影矩阵和 Atmosphere GPU 参数，输出不可变 `RenderView`。Renderer 将 `GlobalFrameUbo`、variable-length `sceneLights` 和 `AtmosphereGpuParams` 上传到当前 frame slot；Pass 通过 `RenderFrameContext::view` 读取同一份 settings、shadow 和 atmosphere 数据。

## RenderQueue 与 Forward

`Scene::collectRenderCommands()` 为每个 SceneObject 生成 Mesh、MaterialInstance、world transform 和 queue 类型。材质满足 `alphaMode == Blend` 或 `transmissionFactor > 0` 时进入 Transparent，其余 Opaque 与 Mask 材质进入 Opaque。

- Opaque 使用 MaterialTemplate、MaterialInstance、Mesh 地址排序，减少 pipeline、descriptor 和 vertex/index buffer 切换。
- Transparent 使用对象 world translation 到相机的距离从远到近排序。这是对象级近似，没有使用 mesh bounds，也不是 order-independent transparency。

SkyBackgroundPass 负责清空 HDR color，并按优先级绘制程序化 Atmosphere、环境 Skybox 或纯色背景。Opaque MainForward 使用 `LOAD` 保留背景、清空主 depth，并把 PBR baseline indirect specular 与 diffuse 写入附加 MRT。Screen-Space Lighting Composite 完成后，Transparent MainForward 使用 `LOAD` 保留 composited HDR，并以 Surface Depth 的 read-only layout 做 depth test；透明阶段不写 baseline MRT。Forward 不再写 swapchain，也不再绘制 ImGui。MSAA HDR 与 baseline indirect images 不使用 transient allocation，因为内容跨多个 render pass 保留。

| 队列 | Blending | Depth test | Depth write |
|---|---:|---:|---:|
| Opaque/Mask | 关闭 | 开启 | 开启 |
| Transparent | 开启 | 开启 | 关闭 |

`doubleSided=false` 使用 back-face culling；`doubleSided=true` 使用 `VK_CULL_MODE_NONE`。相关 fragment shader 通过 `gl_FrontFacing` 修正背面法线。

## 统一可见性与 Hi-Z 遮挡剔除

`IRenderWorld` 只收集全量 RenderCommand，不感知相机。Application 随后用 `VisibilitySystem` 将 Mesh local bounds 变换为 world AABB，并按固定顺序执行 Vulkan `[0,1]` frustum、相机到 AABB 最近点距离和投影像素尺寸检查，生成 camera、shadow caster 与 depth-prepass 三套队列。bounds 无效、穿过 near plane 或投影不稳定的对象保守可见；Transparent 只参与 CPU 剔除并继续 back-to-front 排序。

GPU 遮挡链路只处理 CPU 可见的 Opaque/MASK：

1. `SurfacePrepass` 使用独立的单采样 MRT 绘制遮挡物；MASK 继续执行 BaseColor alpha cutoff，BLEND/transmission 不写入。它同时输出可采样深度、oct 编码 world normal、roughness、motion vector、history-validity，以及供 SSGI 使用的 linear albedo/metallic。
2. `HiZBuildPass` 将 depth 复制到 `R32_SFLOAT` mip 0，并按普通 Z 对每个 2x2 区域取最大深度，构建完整 mip chain。
3. `OcclusionCullPass` 将每个 world AABB 投影到屏幕，矩形扩张 2 像素，选择可覆盖矩形的保守 mip，并比较对象 nearest depth 与覆盖 texel 的最大深度。
4. Compute 为每个候选写出完整 `VkDrawIndexedIndirectCommand`，只通过 `instanceCount=0/1` 控制 MainForward 是否执行该 draw。

MainForward 仍在 CPU 侧逐条绑定 Pipeline、Material、Mesh，并逐条调用一次 `vkCmdDrawIndexedIndirect()`；因此 v1 能跳过被遮挡对象的 vertex/fragment 工作，但不会减少 CPU 绑定与 draw 录制成本，也不是 bindless、draw compaction 或完整 GPU-driven rendering。每个 frame slot 持有独立且按 2 的幂增长的 input SSBO、indirect buffer 和 counter；旧 GPU 统计只在该 slot 的正常 frame fence 完成后读取，不增加等待。

Surface Data 要求存在可采样 depth、`R16G16B16A16_SFLOAT` normal/roughness attachment 和 `R16G16_SFLOAT` motion attachment。GPU Occlusion 额外要求 graphics queue 支持 compute，且 `R32_SFLOAT` 支持 sampled/storage image。Surface Data 不可用时关闭其调试视图和 GPU Occlusion；只缺少 Occlusion 能力时仍可保留 Surface Prepass。MainForward 自动回到直接 draw，CPU camera culling 与 shadow culling 始终可用。

`IRenderWorld` 现在输出稳定的 canonical `RenderItem`；VisibilityFrame 只用 index list 表示 camera opaque、camera transparent 和 shadow caster 队列，避免复制后身份漂移。每个 item 的 key 由 owner、Entity UUID、ModelAsset generation 和 primitive index 构成，legacy 路径使用确定性的 fallback ordinal。直接 draw 和 indirect draw 的 `firstInstance` 都是该 canonical item index。

`VisibilitySystem` 在 frame submit 成功后提交 current world/view-projection，下一帧按 RenderItem key 生成 previous transform。首次运行、scene generation 变化、Editor/Active Camera 切换、viewport resize、projection 改变、Shader variant 变化和 camera cut 会使 history generation 递增，并将本帧 motion 置零。`TemporalFrameHistoryData` 区分 stable projection 与 jittered view-projection：前者用于剔除、阴影拟合和失效判断，后者用于实际光栅化、motion 与时域重投影。Surface Data 调试视图可查看 Normal、Roughness、Motion 和 History Validity；TAA、GTAO、SSR 与 SSGI 复用 frame-level invalidation，但各自维护独立 history。

## 屏幕空间基础与环境遮蔽

屏幕空间效果使用独立资源，不复用遮挡剔除的 max-depth Hi-Z。`ScreenDepthPyramidPass` 从 Surface Depth 生成普通 Z 的最小深度 mip chain；`SceneColorPyramidPass` 在 Opaque Forward 后、Transparent 前从未包含本帧 SSR/SSGI 的 HDR 生成 2x2 box-average mip chain。两者默认不执行，只有对应 Debug View、SSR 或 SSGI 声明需求时才 dispatch。

`SsaoPass` 位于 Occlusion 与 Sky Background 之间。它从每个半分辨率像素覆盖的 2x2 full-resolution texel 中选择最近表面，使用 inverse view-projection 重建位置，并以 world normal 转换后的 view-space normal 建立稳定旋转的半球 kernel。Low、Medium 与 High 固定使用 8、16 与 32 个样本；结果约定为 `1 = fully visible`、`0 = fully occluded`。Raw AO 随后经过 horizontal 与 vertical 两次 5-tap depth/normal bilateral blur。

`windows-msvc-ao-compare` 额外启用固定版本的 FidelityFX CACAO v1.2。`CACAO Input Adapter` 将 Surface Depth 复制到 full-resolution `R32_SFLOAT`，并把 oct-encoded world normal 转成 `RGBA8_UNORM` view-space normal；这两个独立资源满足上游 Vulkan backend 固定的 shader-read layout，不改变共享 Surface Depth ABI。随后 `CACAO` Pass 使用每个 frame slot 独立的 context写出 full-resolution `R8_UNORM` AO。Native/Half 内部分辨率切换只在所有 frame fences 完成后事务重建 contexts。

`GtaoPass` 使用半分辨率 `R16_SFLOAT` Raw、History、Temp、Filtered 和 Debug 资源。Trace 通过 nearest-depth pyramid 搜索 horizon，Low/Medium/High 分别执行 2x2、3x4 和 4x6 的 slice/step 组合。Temporal 阶段使用 motion 重投影 previous history，并通过 depth/normal discontinuity、3x3 AO neighborhood clamp 和 frame-level history generation 拒绝失效样本；之后复用 SSAO 的 horizontal/vertical bilateral blur。GTAO history 与 TAA history 相互独立，参数变化、不连续执行和全局 history invalidation 只重置 GTAO 自身。

MainForward 的固定 `set=4` 包含 32B ScreenSpace UBO 和 filtered AO。只有 Manifest 声明 `screenSpace=true` 的两个 PBR variant 使用它；AO Off 或不兼容 variant 绑定 1x1 white fallback。Opaque/MASK 的间接项使用：

```glsl
indirectOcclusion = materialOcclusion * screenSpaceAo;
```

Direct Lighting、Shadow、Emissive 与 Atmosphere direct transmittance 不乘 screen-space AO；BLEND 与 transmission 强制使用 `screenSpaceAo=1`。SSAO、CACAO 与 GTAO 复用固定 `set=4` AO binding，同一帧只选择一个 active output；仅选择各自 Debug View 时也会执行对应 Pass，但不会因此开启 PBR AO shading。Surface Debug 与 Screen-Space Debug 互斥；Depth 使用 near/far 对数显示，Scene Color 使用当前 exposure/tone mapper，AO、history rejection 与 history weight 使用灰度 pass-through。

能力检查按功能独立降级：nearest depth 要求 graphics compute 与 `R32_SFLOAT` sampled/storage；Scene Color 要求 `RGBA16F` sampled/linear/storage；SSAO 额外要求 Surface Data 与 `R16_SFLOAT` sampled/linear/storage。GTAO 同时要求 SSAO 的 Surface/R16F 能力和 nearest-depth pyramid；CACAO 还要求编译期开关、`shaderImageGatherExtended` 以及上游内部/adapter 所需的 sampled/storage formats。功能不可用时 Renderer 继续启动并通过 UI、Runtime Control 和 `render.status.screenSpace` 报告原因。

## Screen-Space Reflections

SSR 默认关闭，只在两个 PBR variant、Surface Data、nearest-depth pyramid 和 Scene Color pyramid 均可用时激活。Opaque Forward 的第二个 MRT 保存已经计入最终 HDR 的 baseline indirect specular；Scene Color pyramid 在 Opaque 后、Transparent 前从不透明 HDR 生成，因此 trace 不会递归采样本帧 SSR，也不会把透明层当作稳定反射源。

`SsrPass` 使用五张半分辨率 `RGBA16F` per-frame image：Raw、History、Temp、Filtered 和 Debug。Trace 从每个 2x2 full-resolution block 选择最近表面，按 normal、view vector 和 roughness 发射 reflection ray，并使用 nearest-depth pyramid 的 mip 做保守步进；输出 RGB radiance 与 alpha confidence。Temporal 阶段使用 motion、previous depth/normal、3x3 neighborhood clamp 和独立 history generation 做重投影，之后执行两次 depth/normal bilateral blur。camera cut、resize、scene generation、camera mode、projection、Shader variant、参数签名或不连续执行只重置 SSR 自身 history。

`HdrCompositePass` 在 SSR active 时执行：

```glsl
result = opaqueHdr - baselineSpecular
       + mix(baselineSpecular, ssrRadiance, confidence);
```

miss、屏幕边缘、高 roughness 和 rejection 区域因 confidence 降低而自然回到当前 baseline IBL/constant ambient specular。SSR inactive 时该 Pass 只把 opaque HDR 复制到 composited HDR，不 dispatch SSR shader。Transparent 随后在 composited HDR 上执行原有 blending；BLEND/transmission 不写 Surface Data，也不参与 SSR trace。

SSR 要求 graphics compute、Surface Data、`R32_SFLOAT` sampled/storage 以及 `RGBA16F` sampled/linear/storage。能力不足时 requested mode 保留但 active=false，渲染继续使用 baseline specular，并通过 UI、Runtime Control 和 `render.status.screenSpace` 报告原因。Raw、Temporal、Filtered、Confidence 与 Rejection 可通过统一 ToneMap debug source 查看。

## Screen-Space Global Illumination

SSGI 默认关闭，只在两个 PBR variant、Surface Data、albedo/metallic MRT、nearest-depth pyramid 与 Scene Color pyramid 均可用时激活。Opaque PBR Forward 的第三个 MRT 保存已经计入 HDR 的 baseline indirect diffuse；其 alpha 保存 material occlusion，使 SSGI 命中和 ambient/IBL fallback 能使用不同的 AO 语义。

`SsgiPass` 使用六张半分辨率 `RGBA16F` per-frame image：Raw、History、Moments、Temp、Filtered 和 Debug。Trace 按 cosine-weighted hemisphere 发射 4/6/8 条 diffuse ray，并以 nearest-depth hierarchy 搜索命中，从 opaque Scene Color pyramid 读取 incident radiance。Temporal 阶段使用 motion、previous depth/normal、3x3 radiance clamp、luminance moments 和独立 history generation；随后执行两轮 depth/normal/variance-guided A-Trous 滤波。所有 previous-frame descriptor 在 history 无效时绑定当前已初始化资源，避免 resize 或首次启用读取 undefined image。

`ScreenSpaceLightingComposite` 同时处理 SSR 与 SSGI：

```glsl
result = opaque - baselineSpecular - baselineDiffuse
       + mix(baselineSpecular, ssrRadiance, ssrConfidence)
       + mix(baselineDiffuse.rgb,
             ssgiRadiance * baselineDiffuse.materialAo,
             ssgiConfidence);
```

因此 SSGI miss 回退 ambient/IBL baseline，hit 不再次乘完整 screen-space AO，也不会从已经合成 SSGI 的颜色递归采样。SSR 或 SSGI 单独关闭时，对应输入绑定到合法 fallback 且 replacement confidence 为零；两者都关闭时 Composite 只复制 opaque HDR。Transparent 在合成后绘制，不参与 SSGI trace/history。

SSGI history 与 TAA、GTAO、SSR 分离，并响应共享的 camera cut、resize、scene generation、camera mode、projection 和 Shader variant 失效事件；参数签名或执行序列中断只重置 SSGI。Raw、Temporal、Filtered、Confidence、Variance 和 Rejection 可通过 ToneMap 动态 debug source 查看。`render.status.screenSpace` 分别报告 requested GI mode、active fallback、support、extent、history generation 和 reset 原因。

## Temporal Anti-Aliasing

TAA 默认关闭，要求 Surface Data 和 `RGBA16F` sampled/linear/storage 支持。Application 使用 8-phase Halton 2/3 序列生成亚像素 jitter；`RenderView` 保留 stable projection，同时将 jittered projection 写入 Global UBO。SurfacePrepass 因此输出包含 camera/object movement 和 jitter 差异的 motion vector，而 camera/shadow culling 不随 jitter 抖动。

`TaaPass` 位于完整 MainForward 后，读取当前 HDR、当前/上一 frame slot 的 depth 与 normal、motion 和上一 slot 的 TAA History，并写入当前 slot 的全分辨率 `RGBA16F` History 与 Debug。Resolve 使用 nearest-depth motion、depth/normal/history-validity rejection、3x3 YCoCg variance clipping、自适应 history weight 和轻量 sharpening；天空通过 previous view-projection 重投影。BLEND/transmission 不进入 SurfacePrepass，v1 尚无 reactive mask，因此存在透明 draw 时保守限制 history weight。

History 通过 Registry 的 `Previous` sampled-read 契约表达，且只在 scene generation、camera identity、stable projection、Shader variant、viewport 和提交序列连续时复用。resize 会等待现有 frame fences，释放 descriptor 后重建 per-frame history，不调用 device idle。TAA 激活时 Scene Color Pyramid、Bloom 和 ToneMap 直接读取 resolve History；关闭时读取 MainForward HDR，不产生额外全屏复制。History、Rejection 和 History Weight 可通过 ToneMap 调试，UI 仍只在 Present 阶段绘制，因此不经过 TAA。

## 方向光阴影

`buildRenderView()` 只从 `castsShadow=true` 的有效 Directional 中选择 caster。显式 Light Entity 优先于 imported Directional，并使用 Entity UUID、Model Entity UUID 与 prototype index 组成的稳定 key 排序；被选中的 caster 保证进入 256 灯上限且位于 Directional 分段首位。零强度、零颜色或非有限参数的 Scene light 不上传到 GPU；场景没有实际贡献光照的灯时按兼容规则使用 fallback Sun。没有合格 Directional、无有效 bounds、无有效光方向或关闭 Shadows 时，ShadowPass 仍清除目标，但 Forward shader 不采样阴影贡献。

阴影相机优先拟合相机 near 到 `min(cameraFar, shadowDistance)` 的 receiver frustum slice：light-space XY 增加 5% padding并按 shadow texel snapping，Z 范围继续使用 Scene Bounds 且增加 10% padding，从而保留沿光线方向的离屏 caster。Shadow queue 以最终 light-space volume 对 Opaque/MASK AABB 做相交测试，不使用 camera frustum。无有效 receiver、bounds 或光源时回退原来的全 Scene Bounds 拟合。投影继续使用 Vulkan `[0,1]` 深度的正交 ZO 矩阵。

DirectionalShadowPass 的 caster 规则为：

- Opaque 使用 vertex-only depth pipeline。
- MASK 使用 fragment shader，按 BaseColor texture/factor、vertex color 和 alpha cutoff 执行 discard。
- BLEND 与 transmission 不投射阴影。
- `doubleSided` 继续控制 back cull 或 no cull。

PBR-lite Forward 与 PBR-lite NormalMapped 使用 comparison sampler 和 3x3 PCF。阴影只乘到被选中 Directional caster 的 direct contribution；ambient、emissive、Point 和 Spot lighting 不受影响。透明材质可以接收阴影。Raster constant/slope bias 与 shader receiver bias 均可通过 `VulkanLab -> Render -> Lighting` 或 Runtime Control 调节。

`Debug Shadow` 输出最终 visibility 灰度，用于检查投影范围、bias 和 PCF；它使用 PassThrough tone mapping。

## 程序化 Sky Atmosphere

Sky Atmosphere v1 依据 [Hillaire 2020](https://sebh.github.io/publications/egsr2020.pdf) 的 LUT 架构和 [MIT 参考实现](https://github.com/sebh/UnrealEngineSkyAtmosphere)，在项目内实现 Vulkan/GLSL 版本，不把参考工程作为运行时依赖。当前物理模型包含 Rayleigh、Mie 和臭氧吸收、多次散射近似、太阳圆盘，以及作用于 PBR 的 aerial perspective。当前背景采用无地面模式：Sky View 和 Aerial Perspective LUT 将地平线以下的方向镜像到大气上半球，不绘制虚拟行星地表；真实场景几何体仍可正常遮挡背景。Ground Albedo 只参与多次散射近似。

Registry 为 Atmosphere 注册四张 `RGBA16F` storage/sampled image：

- Transmittance：`256x64`，Single，物理参数变化后重算。
- Multiple Scattering：`32x32`，Single，依赖 Transmittance。
- Sky View：`192x108`，Per-frame，活动时按相机和 Sun 每帧更新。
- Aerial Perspective：`32x32x32`，Per-frame 2D array，RGB 保存 in-scattering，Alpha 保存平均 transmittance。

设备必须让 graphics queue 支持 compute，并支持 `R16G16B16A16_SFLOAT` sampled、linear filtering 和 storage image。能力不满足时 AtmospherePass 不创建，descriptor 仍绑定合法 fallback；普通方向光、Skybox 和 clear 背景继续工作。

`AtmosphereLutPass` 在同一 graphics command buffer 中运行四个 compute program。静态物理参数变化后等待 100 ms 稳定窗口再重建 Transmittance 和 Multiple Scattering，避免拖动 Inspector 时连续重算；Sky View 和 Aerial Perspective 在 Atmosphere active 时每帧更新。Pass 只记录 image barrier，不引入 queue/device idle。

Atmosphere Sun 由 SceneDocument 中标记 `atmosphereSunIndex=0` 的 Directional Light 显式选择，与 shadow caster 独立。`buildRenderView()` 会在 256 灯上限内优先保留两者，并把 Atmosphere Sun 的最终 SSBO index 写入 frame data。LUT 使用单位太阳输入，天空、太阳圆盘、直射光透射和 aerial in-scattering 最终乘该灯的 color/intensity。

SkyBackgroundPass 在 PBR variant、Atmosphere 和静态 LUT 都有效时绘制程序化天空；否则保留原 Skybox/clear 路径。两个 PBR fragment shader对 Atmosphere Sun 的 direct contribution 采样 Transmittance，并在材质光照完成后应用：

```glsl
color = color * aerialTransmittance + aerialInScattering;
```

最大 aerial 距离为 `min(cameraFar, 96 km)`，计算使用 camera-relative kilometer 坐标，避免在 shader 中直接对 6360 km 行星坐标做大数相减。Legacy 和材质 Debug variants 不声明 Atmosphere descriptor，也不改变既有基线。程序化天空控制可见背景，Environment IBL 仍可独立启用；v1 不从天空动态生成 diffuse/specular IBL。

## HDR、Viewport Color 与 Present

MainForwardPass 输出线性 HDR；MSAA 开启时 resolve 到单采样 HDR image，并转换为 `SHADER_READ_ONLY_OPTIMAL`。ToneMapPass 使用无 vertex buffer 的 fullscreen triangle 采样当前 frame slot 的 HDR image，并可同时采样 Bloom 第 0 级。其输出写入与当前 Scene Viewport 物理像素一致的 per-frame Viewport Color，而不是 Swapchain。

- PBR-lite 两个 variant 先合成 `hdr + bloom * intensity`，再应用 `color *= exp2(exposureEv)`，最后按设置执行 ACES fitted、Reinhard 或 PassThrough。
- Legacy 和材质通道/Shadow Debug variant 强制 PassThrough，以维持基线和材质通道语义；两个 Debug IBL variant 使用可配置 tone mapping，因为其输出是线性 HDR。
- Viewport Color 与 Swapchain 使用同一 format：sRGB format 由硬件编码，非 sRGB UNORM format 由 ToneMap shader 显式 gamma encode。
- ToneMapPass 不绘制 ImGui；UI 因此不受曝光或 tone mapping 影响。

ToneMapPass 将 Viewport Color 转为 `SHADER_READ_ONLY_OPTIMAL`。Editor 构建通过每个 frame slot 对应的 ImGui descriptor 在 `Viewport` 窗口显示该图像；PresentPass 随后只绘制 ImGui 到 Swapchain。无 GUI 或 Editor 未编译时，PresentPass 使用 fullscreen triangle 直接采样 Viewport Color。PresentPass 负责最终 `PRESENT_SRC_KHR` layout。

## Compute Bloom

Bloom 默认关闭，并且只有 Manifest 中声明 `bloom: true` 的两个 PBR-lite variant 可以激活。设置会在 Shader 切换时保留；Legacy 或 Debug variant 下 `enabled` 可以保持为 true，但 `active` 为 false。

设备能力检查要求选中的 graphics queue 同时支持 compute、`shaderStorageImageExtendedFormats` 可启用，并且 `VK_FORMAT_R16G16B16A16_SFLOAT` 支持 sampled、linear filtering 和 storage image。任一条件不满足时 Renderer 不创建 Bloom 资源或 BloomPass，其他渲染功能继续运行；UI 和 Runtime Control 会报告不可用原因。

BloomPass 使用当前 graphics command buffer 和 queue：

1. 将 HDR Resolve 作为第一级输入。
2. 使用 13-tap filter 依次生成 `1/2` 到 `1/64` 的六张 per-frame `RGBA16F` 图像；第一级按 `max(rgb)`、threshold 和 soft knee 提取亮部。
3. 从最小活动层反向使用 9-tap tent filter，将低分辨率结果累加到上一级。
4. 在 dispatch 之间插入 compute write 到 compute read/write barrier，结束时插入 compute write 到 fragment sampled-read barrier。
5. 当层尺寸已经到达 `2x2` 或更小时停止继续缩小，所有维度至少为 1。

`bloomThreshold`、`bloomSoftKnee` 和 `bloomIntensity` 的范围分别为 `[0,20]`、`[0,1]` 和 `[0,5]`。Bloom 不修改 Global UBO、材质 ABI 或 Lighting descriptor；ToneMap 使用独立的第二个 sampled-image binding 读取结果。

## IBL 与 Skybox

环境资源只从离线派生 KTX2 加载，Renderer 不在运行时执行 equirectangular 转 cube、卷积或 BRDF integration。一个已发布的 `EnvironmentGpuResources` 包含 Radiance、Irradiance、Prefiltered Specular 和 BRDF LUT。切换环境时创建新的 Lighting descriptor generation；新资源完整就绪前继续使用旧 generation，旧资源按 frame submission serial 延迟销毁。程序化 Atmosphere 不会自动替换或重建这组 IBL 资源。

`RenderSettings` 中的 `iblEnabled`、`skyboxEnabled`、`environmentIntensity` 和 `environmentRotationRadians` 默认分别为 false、false、1 和 0。选择 Environment 不自动打开 IBL/Skybox。环境旋转统一作用于 diffuse lookup、reflection vector 和 Skybox，因此光照方向与可见背景保持一致。

两个 PBR-lite variant 使用 metallic-roughness split-sum IBL：

```text
F0       = mix(0.04, albedo, metallic)
diffuse  = irradiance(N) * albedo * (1 - F) * (1 - metallic)
specular = prefiltered(R, roughnessLOD) * BRDF(NdotV, roughness, F0)
indirect = (diffuse + specular) * AO * environmentIntensity
```

Irradiance bake 已除以 π，因此 shader 不再次除 π。AO 只乘 IBL/constant ambient 间接项，不影响 Directional、Point、Spot 或 emissive。IBL 关闭、环境未就绪或设备不支持所需 float format 时，shader 精确保留原 constant ambient 路径。

SkyBackgroundPass 在 Atmosphere 未接管背景时，使用 fullscreen triangle、inverse view-projection 和 Radiance LOD 0 输出线性 HDR Skybox。它不写 depth；MainForward 的不透明几何覆盖背景，透明/transmission 材质在已有背景上执行现有 alpha blending。`Debug IBL Diffuse` 与 `Debug IBL Specular` 分别隔离两条间接光路径。

## Pipeline、材质与 Descriptor

MaterialTemplate 保存基础 PipelineConfig 和材质 descriptor layout。MaterialInstance 保存材质参数及 BaseColor、Normal、MetallicRoughness、Occlusion、Emissive 五个纹理槽。缺失槽由 fallback texture 填充，因此 `VulkanLab -> Materials` 中的 Bound 只表示 descriptor 已绑定。

PipelineConfig 支持零或多个 color blend attachment、零 vertex binding、可选 fragment shader、topology、subpass 和 depth bias。`PipelineCache::getOrCreate()` 只接收 render pass 与完整 `PipelineConfig`，由 cache 内部规范化并生成 key。Key 覆盖 shader 路径、vertex layout、topology、raster/depth/blend/MSAA 状态、descriptor layouts、push ranges、render pass 和 subpass；不再包含 pass、材质指针、ShaderVariant、queue 或 alpha-masked 等语义标签。Pipeline 创建直接使用 key 内保存的 config，因此不存在手工 key 与实际 Vulkan 状态分叉。

Compute 使用独立的 `ComputePipelineConfig`、`ComputePipelineKey` 和 `PipelineCache::getOrCreateCompute()`。Compute key 覆盖 compute shader、descriptor layouts 和 push ranges；与 graphics pipeline 一样，`debugName` 只用于对象命名，不参与缓存身份。`PipelineCache::clear()` 会同时销毁 graphics 和 compute pipelines。

Forward descriptor 约定为：

- `set=0, binding=0`：每帧 GlobalUBO，包含相机、灯光计数、directional shadow 和 Atmosphere frame 数据。
- `set=0, binding=1`：PBR fragment shader 使用的 Scene Light SSBO。
- `set=1, binding=0..4`：五个材质纹理槽。
- `set=2`：统一 Lighting descriptor。
  - binding 0：当前 frame slot 的 comparison shadow map。
  - binding 1：Irradiance cubemap。
  - binding 2：Prefiltered Specular cubemap。
  - binding 3：BRDF LUT。
  - binding 4：Radiance cubemap。
  - binding 5：最多 8 个局部 Reflection Probe prefiltered cubemap。
  - binding 6：每帧 Reflection Probe metadata SSBO。
- `set=3`：Atmosphere descriptor，仅 Atmosphere programs 和两个 PBR variants 声明。
  - binding 0：每帧 `AtmosphereGpuParams` UBO。
  - binding 1：Transmittance LUT。
  - binding 2：Multiple Scattering LUT。
  - binding 3：Sky View LUT。
  - binding 4：Aerial Perspective 2D-array LUT。
- 128 字节 push constant：model matrix 和材质因子。

Lighting 的七个 binding 始终绑定真实资源或合法 fallback，不依赖 partially-bound descriptor。局部 Reflection Probe 按 priority 和 Entity UUID 稳定选择最多 8 个；Box/Sphere influence 与 box parallax correction 在 PBR fragment shader 中完成。SSR 使用 confidence 替换 `Local Probe -> Global IBL` 的 specular baseline，global diffuse irradiance 不被局部探针替换。sampler array 通过常量 switch 访问，因此不要求 sampled-image array dynamic indexing。ToneMap 使用固定五 binding 的 pass-local descriptor：当前 HDR、Bloom、Surface Normal-Roughness、Motion 和按当前 debug mode 选择的单一 Debug Source。Depth/Color pyramid、AO、TAA、GTAO、SSR 与 SSGI 调试图像都在 CPU 更新 descriptor 时映射到该动态 source，避免每新增一个算法就扩大 ToneMap descriptor ABI。未在本帧生成的输入绑定已初始化 fallback，push constant 禁止采样。Present 使用另一个 pass-local descriptor，只包含当前 frame slot 的 Viewport Color。

## Shader Variant

`shader/manifest.json` 是 Shader program 和 selectable variant 的唯一权威清单。CMake 在配置阶段读取 Manifest、去重所有 stage 源文件，并为 `glslc` 配置 include 路径和依赖；每个产物必须先通过 `spirv-val`，再从 `generated/<Config>/shader/` stage 到 runtime `shader/`。Manifest 本身也进入开发 runtime 和 Cook package。

Application 在创建 Window/Vulkan 前加载 `ShaderRegistry`。当前选择使用稳定 variant ID，UI 使用 display name；ToneMap 和 Bloom 兼容性由 variant metadata 决定。Shadow、Bloom、ToneMap 与 Present 通过稳定 program ID 查询，不再维护 C++ 路径常量。MaterialTemplate 的基础 PipelineConfig 不携带默认 Shader，MainForwardPass 在创建 pipeline 前必须写入当前 variant 路径。

测试目标静态链接固定版本的 SPIRV-Reflect，按 Manifest program contract 遍历全部 Forward、Shadow、Atmosphere、Sky Background、Visibility、Screen-Space、Bloom、ToneMap 与 Present program，校验 stage、descriptor、UBO/push size 和 member offset、vertex location/format、跨阶段 varying及 fragment output。反射不进入 VulkanLab 运行时，也不自动生成 DescriptorSetLayout；生产布局仍由显式 C++ 代码创建。

当前 variant 包含 Legacy、两个 PBR-lite、BaseColor/Normal/Roughness/Metallic/Occlusion/Emissive/Alpha/Transmission 调试视图，以及 `Debug Shadow`、`Debug IBL Diffuse` 和 `Debug IBL Specular`。启动默认使用 `PBR-lite NormalMapped`；Legacy 保留为显式基线和兼容性检查。只有两个 PBR-lite variant 在 Manifest 中声明 `bloom: true`。PBR-lite 使用 baseColor、metallicRoughness、AO、emissive 和可选 IBL；NormalMapped 额外使用 tangent/TBN 与 normal scale。Transmission 当前仍是 alpha 与 Fresnel 轮廓近似，不采样场景颜色。

新增兼容 Main Forward ABI 的 variant 只需增加 GLSL 和 Manifest 条目；构建、运行时 UI、Cook 和 contract tests 会自动包含它。当前不支持目录扫描、热重载或第三方 Shader 插件，具体流程见 [Shader Registry](../guides/shader_registry.md)。

顶点布局固定为 position、normal、UV0、tangent、UV1 和 vertex color，location 为 0 到 5。AO 可选择 UV0/UV1；其他纹理当前使用 UV0。

## 光源

SceneLight 支持 Directional、Point 和 Spot。glTF loader 解析 `KHR_lights_punctual` 根定义和当前 scene 的 node 引用，应用完整 node world transform后生成静态世界空间灯光。同一 light definition 可以由多个 node 实例化。glTF Directional 的局部 `-Z` 发射方向会翻转为引擎使用的 surface-to-light 方向；Spot 保留 light-to-scene 发射方向。

`GlobalFrameUbo` 只保存 `directional/point/spot/total` 计数，不再内嵌灯数组。`set=0 binding=1` 是 Fragment stage 的只读 `std430` Scene Light SSBO；`GpuLight` stride 固定 64 字节。Renderer 为每个 frame slot 分配持续映射的独立 buffer，容量从 16 按 2 的幂增长，最多 256 且不自动收缩；替换只发生在该 slot 的 fence 已完成后，不调用 `vkDeviceWaitIdle()`。

PBR Forward 按 Directional、Point、Spot 三段遍历 SSBO，Point/Spot 在超出 range 时提前跳过。三类灯共享 256 灯上限，超出部分保留在 SceneDocument 并计入 ignored；Legacy 继续使用 baseline 光照且不读取 SSBO。存在任意有效场景灯光时不注入 fallback Sun；如果场景灯全部无效，则按兼容规则使用 fallback Sun。Point/Spot 当前不投射阴影。颜色和 intensity 保持 glTF 物理单位，不做自动缩放；高强度场景通过 Exposure EV 和 Tone Mapping 调整。没有可用 IBL 时，环境项由 ambient color/intensity 提供；AO 只影响间接项。

当前支持一张全局环境、最多 8 个局部 Reflection Probe 和一张方向光 shadow map；没有 CSM、Point/Spot shadow、动态 probe 自动更新、local diffuse probe、deferred rendering 或 auto exposure。Bloom 是同步 compute 后处理，不使用异步 compute、lens dirt、anamorphic filter 或 temporal stabilization。

## GPU Pass 计时

Renderer 持有一个 `GpuPassProfiler` 和 timestamp query pool。每个 frame slot 为 `Atmosphere LUTs`、`DirectionalShadow`、`SurfacePrepass`、`HiZBuild`、`OcclusionCull`、按需 `ScreenDepthPyramid`、`SSAO`、`GTAO`、可选 `CACAO Input Adapter/CACAO`、`SkyBackground`、`MainForward Opaque`、按需 `SceneColorPyramid`、`SSR`、`SSGI`、`ScreenSpaceLightingComposite`、`MainForward Transparent`、`TAA`、可选 `Bloom`、`ToneMap` 和 `Present + UI` 分配 begin/end query。总时间从第一个 Pass begin 到最后一个 Pass end 计算。

`FrameSync::beginFrame()` 已等待对应 slot 的 fence 后，Profiler 才使用不带 `WAIT_BIT` 的 `vkGetQueryPoolResults()` 读取旧结果，然后在新 command buffer 中 reset 该 slot。计时不会增加 queue/device idle 或额外 fence wait。换算使用设备 `timestampPeriod`，并按 graphics queue 的 `timestampValidBits` 处理计数器回绕；不支持 timestamp 的设备返回 `available=false`，渲染继续运行。结果显示在 `VulkanLab -> Diagnostics -> Performance`，并由 `render.status.gpuTimings` 返回。

## Tracy 统一时间线

`VKL_ENABLE_TRACY` 只在 `windows-msvc-tracy` 专用配置中开启。Device 持有一个可选 `TracyProfiler`，其生命周期在 `VkDevice` 销毁前结束；关闭开关时链接 no-op 实现，不配置 Tracy submodule，也不创建网络线程或 Vulkan query 资源。

Tracy Vulkan context 使用 graphics queue 和独立 transient command pool完成初始化。各 Pass 在现有 frame command buffer 中写入嵌套 GPU zone；`ModelGpuBuilder` 的 `IncrementalUploadQueue` 写入 `ModelUpload model=<id> profile=<profile>` zone，环境和 legacy 同步 `UploadContext` 保留各自的上传 zone。所有 zone 结束后，每帧调用一次 `TracyVkCollect()`，没有增加 `WAIT_BIT`、queue idle、device idle 或额外 fence。

GPU 层级覆盖 Atmosphere LUTs、DirectionalShadow/ShadowCasters、SkyBackground、MainForward/Opaque/Transparent、可选 Bloom Downsample/Upsample、ToneMap、Present/ImGui、ScreenshotCopy 和上传 batch。CPU 侧覆盖 Application frame、FrameSync、RenderView/RenderQueue、场景与环境 worker、glTF 各准备阶段、逐帧 GPU builder、资产工具监督、Capture encode 和 pipeline cache miss。单 draw、单纹理和单 mip 不创建 zone。

Tracy 与 `GpuPassProfiler` 并行存在：后者是普通开发构建中的低成本数值统计，前者是按需连接的跨线程时间线。Tracy 编译和连接状态通过 `system.info.diagnostics.tracy` 与 `Diagnostics -> Performance` 显示；完整操作见 [Tracy 性能分析](../guides/tracy_profiling.md)。

## Workspace 与 Viewport 截图

开发运行时提供异步 PNG 截图，入口为 `VulkanLab -> Diagnostics -> Capture`、F12 或 Runtime Control v3。`includeGui=true` 选择最终 Swapchain，输出完整 Workspace；`includeGui=false` 选择当前 frame slot 的 Viewport Color，输出实际 Viewport 原生分辨率。两种来源都在同一个 frame command buffer 中转换到 `TRANSFER_SRC`、执行 image-to-buffer copy，并恢复为 present 或 shader-read layout。

FrameSync 使用单调 submission serial 和正常 frame fence 管理 readback 生命周期。CPU worker 只处理 RGBA bytes、PNG 和 SHA-256，不访问 Vulkan、GLFW、ImGui 或 Scene。截图路径不调用 `vkQueueWaitIdle()` 或 `vkDeviceWaitIdle()`。

## Viewport 与 Swapchain Resize 生命周期

Viewport 内容区变化与操作系统窗口 resize 使用两条独立生命周期。

Viewport resize 采用 120 ms debounce；首次有效尺寸立即在下一帧应用。Application 调用 `FrameSync::waitForAllFrames()` 后移除 ImGui viewport descriptors，Pass 释放 viewport-dependent framebuffer/descriptor，Screen-Space descriptor 临时切回 white/HDR fallback，Registry 重建 HDR、baseline indirect specular/diffuse、composited HDR、MSAA、depth、screen pyramid、SSAO、GTAO、SSR、SSGI、可选 CACAO adapter/output、Bloom 与 Viewport Color，随后 Pass、CACAO contexts 和 ImGui descriptors 重新绑定。TAA、GTAO、SSR 与 SSGI history 在新 extent 上分别重置。该路径不重建 Swapchain、不清空 PipelineCache，也不调用 `vkDeviceWaitIdle()`。

Swapchain resize 只释放 PresentPass 的 swapchain framebuffer，重建 Swapchain、FrameSync 和 Present framebuffer；无 GUI 路径同时把 viewport extent 更新为新的 Swapchain extent。窗口最小化导致 framebuffer extent 为 0 时会延迟重建，并以短暂 sleep 保持主循环和 Runtime Control 可响应。

Fixed shadow map和 Environment cubemap/LUT 不属于 viewport-relative Registry，Viewport 或窗口 resize 时都不会重建。
