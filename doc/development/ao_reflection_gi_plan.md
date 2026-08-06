# AO、反射与全局光照算法路线

> Status: Active
> Last reviewed: 2026-08-06
> Based on: Stage 0–3 plus optional FidelityFX CACAO comparison integration
> Current architecture: [渲染流程](../architecture/rendering.md)

## Summary

本路线在现有 Forward + Compute 管线上逐步增加环境遮蔽、屏幕空间反射和间接漫反射，并为后续 Reflection Probe、DDGI 和可选硬件光追保留清晰边界。

> Progress: Stage 0（共享屏幕空间基础）、Stage 1（SSAO）、FidelityFX CACAO comparison backend、Stage 2（TAA v1）和 Stage 3（GTAO v1）已完成；下一阶段为 Stage 4 SSR。

三个问题必须分开处理：

- **AO** 估算局部可见性，只负责削弱间接光，不产生新的光能。
- **Reflection** 处理间接镜面反射，屏幕空间追踪失败时回退到 IBL 或局部 Reflection Probe。
- **GI** 处理间接漫反射，即光线经其他表面反弹后带来的照明与颜色传递。

推荐演进顺序为：

```text
共享 Screen-Space Effect 基础
  -> SSAO
  -> TAA
  -> GTAO
  -> SSR + IBL fallback
  -> SSGI + IBL fallback
  -> Local Reflection Probes
  -> DDGI / Irradiance Probes
  -> 可选硬件光追反射与 GI
```

第一阶段不引入 Deferred Renderer、RHI 或 RenderGraph。算法选择属于 `RenderSettings` 和 Pass 调度，不应继续扩张为材质 Shader Variant。

## 当前基础与约束

### 已具备的共享输入

当前 Renderer 已经提供：

- 单采样 Surface Depth。
- World Normal、Roughness 和 History Validity。
- Motion Vector。
- 稳定 `RenderItemKey`、previous world transform 和 history generation。
- Viewport resize、scene generation、camera identity 与 camera cut 失效机制。
- 用于 GPU Occlusion 的完整 max-depth Hi-Z mip chain。
- 独立的 nearest/min-depth Screen Depth Pyramid 与 HDR Scene Color Pyramid。
- 半分辨率 SSAO Raw/Temp/Filtered 资源、稳定 kernel 与 bilateral filter。
- 线性 HDR color、Bloom、Tone Mapping 和独立 Viewport output。
- 类型化 `RenderResourceRegistry`、显式 Pass 顺序、compute pipeline cache、GPU timestamp、RenderDoc label 和 Tracy zone。

这些能力已经消除了实现屏幕空间算法前最主要的数据缺口，但还不等于可以直接把所有效果接到 MainForward 后面。

### 必须先解决的架构问题

1. **Visibility Hi-Z 语义不适合直接用于反射射线，Stage 0 已建立独立资源。**

   `VisibilityHiZ` 继续在普通 Z 下保存 2x2 区域的最大深度，目的是保守判断物体是否被遮挡。Stage 0 新增的 `Screen Depth Pyramid` 保存最小深度，当前供调试，后续供 SSR/SSGI 使用；若 thickness 与薄几何问题明显，再升级为 min/max depth。

2. **当前 HDR 输出混合了所有光照分量。**

   MainForward 当前直接输出 direct、ambient/IBL、emissive 和 atmosphere 的最终和。SSR 与 SSGI 如果只在最终 HDR 上做加法，会重复计算已有的 IBL specular 或 diffuse ambient，并造成能量不断增加。

3. **当前 Temporal 基础还没有完整 TAA。**

   已有 motion/history validity，但尚未引入 projection jitter、颜色历史重投影、neighborhood clipping、透明/高亮 reactive mask 和最终 temporal resolve。

4. **Transparent 不写 Surface Data。**

   BLEND 与 transmission 不进入 SurfacePrepass。因此 v1 的 AO、SSR、SSGI 和 TAA 几何判定只保证 Opaque/MASK 正确；透明物体只在最终透明阶段消费已经合成的背景。

5. **程序化 Atmosphere 不会自动生成 IBL。**

   Atmosphere 负责可见天空、太阳透射和 aerial perspective；IBL 仍来自离线 Environment。SSR/SSGI miss 必须明确选择当前 Environment、constant ambient 或黑色作为 fallback，不能假定 Atmosphere 已提供环境卷积。

## 目标与非目标

### 目标

- 在 Editor 中即时切换基础算法并显示 Requested、Supported、Active 和 Fallback 状态。
- 共用 depth、normal、roughness、motion、history 和 color pyramid，避免每个算法重复维护基础设施。
- 所有时域算法统一响应 camera cut、scene reload、viewport resize、Editor/Active Camera 切换和 history-invalid item。
- 保持 Direct Lighting、Indirect Diffuse、Indirect Specular、AO 和 Emissive 的能量语义明确。
- 允许每个算法单独关闭；关闭后不得改变当前画面基线。
- 使用现有 GPU profiler、RenderDoc 和 Tracy 比较效果收益与 Pass 成本。

### 非目标

- 第一轮不实现 Deferred Renderer、完整 G-buffer 或自动 Pass 调度。
- 不把 AO 当作 GI，也不把 SSR 当作完整反射方案。
- 不在 v1 中处理透明表面的 SSR/SSGI、递归反射或多次光线反弹。
- 不首先实现 LPV、VXGI、ReSTIR GI、SVGF、路径追踪或完整光照烘焙系统。
- 不把算法模式写入 SceneDocument；初期保持当前会话级 `RenderSettings`。

## 光照组合语义

目标组合必须遵守：

```text
Direct Lighting    = Directional + Point + Spot + Shadows
Indirect Diffuse   = Ambient / IBL Diffuse / SSGI
Indirect Specular  = IBL Specular / Reflection Probe / SSR
Emissive           = Material Emissive
Final HDR          = Direct + Indirect Diffuse + Indirect Specular + Emissive
```

AO 只影响间接项：

```text
materialAo = occlusionTexture
screenAo   = SSAO or GTAO

fallbackDiffuse = baseIndirectDiffuse * materialAo * screenAo
fallbackSpecular = baseIndirectSpecular * specularOcclusion
```

当 SSGI 和 SSR 存在时应执行带 confidence 的替换，而不是无条件相加：

```text
indirectDiffuse = mix(fallbackDiffuse,
                      ssgiDiffuse * materialAo,
                      ssgiConfidence)

indirectSpecular = mix(fallbackSpecular,
                       ssrRadiance,
                       ssrConfidence)
```

SSGI 命中区域不应再次完整乘 SSAO，否则会产生双重遮蔽。推荐让 screen AO 主要作用于 SSGI miss/fallback，或只以低强度 contact term 作用于 SSGI 结果。

Direct Lighting 和 Emissive 永远不乘 screen-space AO。Atmosphere aerial perspective 在最终场景光照合成后继续执行。

## 共享 Screen-Space Effect 架构

### 建议帧图

目标帧图如下：

```text
Atmosphere LUTs
Directional Shadow
SurfacePrepass
  -> Surface Depth / Normal-Roughness / Motion-History
Occlusion Hi-Z (max depth)
Occlusion Cull
Screen-Space Depth Pyramid (nearest or min/max depth)
AO Trace + Denoise
Sky Background
Opaque Forward Base
  -> Base HDR / indirect-light breakdown
Scene Color Pyramid
SSR Trace + Resolve
SSGI Trace + Resolve
Screen-Space Lighting Composite
Transparent Forward
TAA Resolve
Bloom
ToneMap
Present + UI
```

透明绘制需要从当前 MainForward 中形成明确的后置阶段。Opaque/MASK 先形成可追踪的场景颜色，SSR/SSGI 完成后再绘制 BLEND/transmission，使透明材质能够看到已经合成的背景，同时避免要求透明材质写 Surface Data。

### 深度层级

保留当前 `VisibilityHiZ` 的 max-depth 语义，避免影响已经工作的遮挡剔除。为屏幕空间射线新增独立深度层级：

- v1 推荐新增 `R32_SFLOAT` nearest-depth pyramid。
- 后续若 thickness 与薄几何体问题明显，再升级为 `RG32_SFLOAT` min/max pyramid。
- near-plane 相交、无效 depth 和屏幕边界必须保守失败，交由 fallback 处理。
- 不应为了节省一张图而在同一资源上混用 Occlusion 与 Ray Marching 的相反约定。

### 场景颜色与光照分量

在实现 SSR/SSGI 前必须确定可替换的间接光分量。推荐顺序为：

1. SSAO/GTAO 先直接作为 MainForward 的间接光输入，不要求拆分 HDR。
2. SSR 前为 Opaque Forward 增加 baseline indirect specular 输出。
3. SSGI 前增加 baseline indirect diffuse 输出。
4. Composite 使用同一帧写出的 baseline 分量做减法/替换，禁止凭经验向最终 HDR 叠加。

最直接的 v1 实现可以使用两个 `RGBA16F` auxiliary attachment。它有明确的显存成本，但语义可靠；后续再评估 `R11G11B10`、半分辨率或在 compute composite 中重算间接光。

### 设置接口

建议增加：

```cpp
enum class AmbientOcclusionMode {
    Off,
    Ssao,
    Gtao,
};

enum class AntiAliasingMode {
    Off,
    Taa,
};

enum class ReflectionMode {
    IblOnly,
    Ssr,
};

enum class GlobalIlluminationMode {
    AmbientOrIbl,
    Ssgi,
};
```

模式选择属于 `RenderSettings`。Shader Manifest 继续管理 material shading program 和固定 pass program，不为 `SSAO/GTAO/SSR/SSGI` 的组合生成大量 Forward Shader Variant。

每个功能状态统一报告：

```text
requestedMode
activeMode
supported
fallbackMode
unavailableReason
```

### History 规则

所有 temporal effect 共用一套 frame-level invalidation 信号，但各自保存独立 history validity：

- 首帧无历史。
- Viewport extent 或 projection 改变时失效。
- Editor/Active Camera 切换和 camera cut 时失效。
- Scene generation 或 shader contract 改变时失效。
- 单个 RenderItem 缺少 previous transform 时只拒绝该像素历史。
- 非有限 motion、越界 reproject UV、depth/normal 不连续时拒绝历史。
- Effect 参数发生结构性变化时清空对应 history，不必清空其他 effect。

History 读取继续使用 `RenderResourceRegistry::previousImage()`，不得为读取结果增加 CPU fence 或 queue idle。

### Debug Views

建议把调试模式从现有 Surface Data 扩展为统一枚举或分类菜单：

- AO：Raw、Denoised、History Weight。
- TAA：Motion、Reprojected History、Rejection、Final Weight。
- SSR：Hit UV、Ray Distance、Step Count、Confidence、Resolved Radiance。
- SSGI：Raw Radiance、Confidence、Variance、Denoised Radiance。
- Composite：Indirect Diffuse、Indirect Specular、Fallback Mask。

调试视图输出线性 HDR 时继续使用可配置 Tone Mapping；输出归一化 mask 时使用 PassThrough。

## 参考实现、论文与依赖策略

本路线使用的本地参考资料统一收录在 [实时光照算法参考资料](../../references/rendering_algorithms/README.md)。来源 URL、固定 commit、稀疏检出路径和许可证提示记录在 [sources.json](../../references/rendering_algorithms/sources.json)，实际第三方内容位于被 Git 忽略的 `references/rendering_algorithms/downloads/`。

在仓库根目录可复现下载：

```powershell
powershell -ExecutionPolicy Bypass -File `
  references/rendering_algorithms/Fetch-RenderingReferences.ps1
```

当前索引包含 6 个固定版本源码仓库、7 份论文或演示文稿和 4 份官方文档快照。下载脚本会生成 `SHA256SUMS.txt` 与 `fetch-report.json`；这些文件用于本地研究和完整性检查，不进入 CMake、Cook 或 Runtime package。

### Stage 与参考资料映射

| 路线阶段 | 主要参考 | 使用目的 | 默认采用方式 |
|---|---|---|---|
| Stage 0：共享基础（完成） | `code/taa`、FidelityFX SSSR manual | History、depth/color pyramid、资源和 Pass contract | 参考接口与数据流，在项目内实现 |
| Stage 1：SSAO + CACAO 对比（完成） | `code/fidelityfx-cacao` | Compute AO 分辨率、边缘保持滤波和 Vulkan 集成对照 | SSAO 为项目内 baseline；固定上游 CACAO v1.2 作为可选 comparison backend |
| Stage 2：TAA | `code/taa`、`temporal_supersampling_2014.pptx` | Jitter、history resolve、clipping、ghosting 处理 | 按现有 Motion/History ABI 重写 |
| Stage 3：GTAO | `code/xegtao`、`gtao_2016.pdf` | Horizon search、depth prefilter、denoise、multiple-bounce AO | 移植算法思想到 Vulkan/GLSL，不直接接入 DirectX wrapper |
| Stage 4：SSR | `screen_space_ray_tracing_2014.pdf`、`code/fidelityfx-sssr`、SSSR manual | Perspective-correct DDA、hierarchical trace、SPD 和 denoise | 先实现本项目 contract，再评估集成 FidelityFX SSSR |
| Stage 5：SSGI | `screen_space_indirect_lighting_bitmask_2023.pdf`、`svgf_2017.pdf` | Visibility bitmask、时空方差和 A-Trous 降噪 | 项目内实现基础 SSGI；SVGF 只作为降噪设计参考 |
| Stage 7：DDGI | `ddgi_irradiance_fields_2019.pdf`、`ddgi_scaling_2021.pdf`、`code/rtxgi-ddgi` | Probe visibility、relocation、classification 和滚动更新 | 先固定项目 Probe ABI，再决定参考重写或可选 SDK 模块 |
| Stage 8：硬件光追 | `code/nrd`、`svgf_2017.pdf` | RT diffuse/specular、hit distance contract 和时空降噪 | 优先评估 NRD，不从零开发生产级 RT denoiser |

具体本地论文文件名、源码用途和限制以参考目录 README 为准。HTML 快照只用于离线检索文字；真正接入 SDK 或依据可能更新的 API 开发前，必须重新核对对应官方在线文档。

### Stage 0–1 的实际采用与差异

- TAA 与 SSSR 参考只用于约束 history、nearest depth 和 scene color 的资源职责；当前没有复制其 temporal resolve、SPD 或 ray tracing 实现。
- Screen Depth 与 Visibility Hi-Z 保持为两套资源，分别使用普通 Z 的 min 与 max 归约，避免为节省资源混用相反语义。
- Scene Color Pyramid 当前在完整 MainForward 后生成，仅用于 Debug；SSR 阶段会在拆分 Opaque/Transparent 后调整生产位置。
- SSAO 仍是项目内 GLSL baseline，采用最近 2x2 surface、8/16/32 个稳定半球样本，以及两次 5-tap depth/normal bilateral blur。
- `VKL_ENABLE_CACAO=ON` 时链接固定提交的 FidelityFX CACAO v1.2。它保留上游 adaptive quality、importance map、edge-aware blur 与 upsample；VulkanLab 只实现资源、normal/depth adapter、Pass 和控制适配，不把 CACAO 命名成 SSAO 或 GTAO。
- CACAO Vulkan backend要求输入 depth/normal 为 `SHADER_READ_ONLY_OPTIMAL`，与项目 Surface Depth 的 depth-stencil layout 不同。因此 comparison preset 使用独立的 full-resolution `R32F` depth adapter 与 `RGBA8_UNORM` view-normal adapter，不修改共享 Surface ABI。
- 每个 frame slot 使用独立 CACAO context；Native/Half 重配置先等待现有 frame fences，再用候选 contexts 事务替换，不调用 `vkDeviceWaitIdle()`。普通开发与 Runtime preset 保持 `VKL_ENABLE_CACAO=OFF`，不编译或链接 SDK。
- Stage 1 不做逐帧 kernel 旋转与 temporal accumulation，避免在 TAA 完成前引入闪烁和独立历史链路。

### 参考与直接集成边界

默认在项目内重新实现：

- SSAO、TAA、GTAO 和基础 SSGI。
- Screen-space depth/color pyramid、temporal history、confidence 和 composite。
- 与 VulkanLab `RenderResourceRegistry`、Shader Manifest、GPU profiler 和 Runtime Control 的适配层。

可以在对应阶段单独评估直接集成：

- **FidelityFX SSSR**：已有 Vulkan/GLSL 路径，但接入前必须证明其资源、descriptor 和 barrier 生命周期能服从现有 Pass 架构。
- **RTXGI DDGI**：只有在 Vulkan Ray Tracing、AS 生命周期和 bindless material access 成熟后才评估为可选模块。
- **NVIDIA NRD**：推荐用于后续硬件光追 diffuse/specular/shadow 降噪，但它不能替代 Motion、Normal/Roughness、View Depth 和 Hit Distance contract 的建设。

不允许从 `references/` 直接 include、link 或在运行时读取第三方源码。确需移植代码时，应建立正式依赖或在项目源码中独立实现，并记录上游项目、commit、许可证和必要 attribution。

### 许可证与更新规则

1. 下载内容不提交到主仓库，避免仓库膨胀和无意再分发论文或 SDK。
2. MIT 项目仍需保留版权及许可证声明；NVIDIA SDK 类项目必须在集成前单独审核分发条款。
3. 论文中的算法可依据公式和描述重新实现，但正文、图片和附件不能默认进入产品或 Cooked package。
4. 更新参考版本时修改 `sources.json` 中的固定 commit，并重新生成校验报告；不使用浮动 branch 作为实现依据。
5. 每个算法 Stage 的实施计划应明确列出实际采用的参考项、偏离上游的设计以及最终许可证处理方式。

## 算法分析

### SSAO v1

#### 作用

SSAO 根据当前视角下的 depth 和 normal 估算局部半球被几何体遮挡的比例。它主要改善接触区域、墙角、模型缝隙和小范围空间关系。

#### 推荐实现

- 半分辨率 compute。
- 从 depth 重建 view-space position。
- 使用 16 个固定半球样本，并按像素噪声旋转 kernel。
- 使用 radius、bias 和 range check 防止跨越不相关表面。
- 两次 separable bilateral blur，使用 depth/normal 保边。
- 输出约定固定为 `1 = fully visible`、`0 = fully occluded`。
- 推荐输出 `R16_SFLOAT`；如果设备明确支持 storage `R8_UNORM`，后续可降精度。

#### 编辑参数

```text
Radius
Bias
Intensity
Power
Quality / Sample Count
```

#### 优点与限制

- 优点：实现范围小、结果直观，是验证 screen-space framework 的最佳入口。
- 限制：只有局部信息，远离屏幕或被遮挡几何体不可见；容易产生 halo、噪声和视角相关变化。
- SSAO 是教学与兼容 baseline，GTAO 完成后不建议作为默认高质量模式。

### TAA v1

#### 作用

TAA 本身不产生 AO、反射或 GI，但它是低采样 SSR/SSGI 和稳定 GTAO 的共同降噪基础，因此应在 SSAO baseline 后、SSR/SSGI 前完成。

#### 推荐实现

- Halton 2/3 projection jitter。
- 使用 motion vector 重投影上一帧 HDR。
- 使用 depth、normal、history validity 和 viewport bounds 拒绝错误历史。
- 在 YCoCg 或类似亮度/色度空间执行 neighborhood clipping。
- 根据 motion、亮度变化和 history validity 自适应 blend weight。
- 提供轻量锐化，但不在 v1 中引入复杂 upscaling。
- TAA 发生在 Transparent 之后、Bloom 之前；ImGui 仍在 Present 阶段，不进入 TAA。

#### 主要风险

- SurfacePrepass 不覆盖透明物体，玻璃、粒子和高 emissive 可能产生拖影。
- Projection jitter 必须同时进入 current/previous matrix 约定，不能重复计算进 motion。
- Gizmo、camera cut 和 scene publish 必须显式失效历史。

v1 可以先使用保守 rejection；Reactive Mask 和透明 motion 留到后续。

### GTAO

#### 作用

GTAO 通过沿多个方向搜索 horizon angle，近似更符合几何关系的环境可见性。相比 SSAO，它在墙角、曲面和较大尺度遮蔽上更稳定。

#### 推荐实现

- 半分辨率 compute。
- 4 个方向，每方向 4 到 6 个 steps 作为初始质量档。
- 使用 view-space depth 与 normal 计算 horizon contribution。
- 复用 SSAO 的 bilateral denoise 输出契约。
- 使用 Motion/History 做 temporal accumulation，并对 depth/normal discontinuity 执行 rejection。
- v1 输出 scalar AO；Bent Normal 作为后续扩展，不阻塞第一版。

#### 模式关系

```text
Off   -> AO = 1
SSAO  -> sample-kernel baseline
GTAO  -> horizon-search quality path
```

两种算法必须输出同一 AO contract，MainForward 不感知具体算法。

### SSR

#### 作用

SSR 根据 view vector、surface normal 和 roughness 在屏幕空间追踪反射射线，并从当前不透明 HDR 场景颜色中读取命中 radiance。

#### 推荐实现

- 只处理 Opaque/MASK。
- 使用 nearest 或 min/max depth pyramid 做 hierarchical ray marching。
- 先支持低 roughness 表面；超过 roughness cutoff 直接回退 IBL。
- 屏幕边缘、背面、无效 depth、最大距离和最大 steps 均输出低 confidence。
- 对 Scene Color 建立 mip pyramid，按 roughness 与 ray cone 选择采样 LOD。
- 输出 `RGBA16F`，RGB 为 radiance，A 为 confidence。
- 使用 motion、depth、normal 和 roughness 做 temporal resolve，再进行小范围 bilateral denoise。

#### Fallback 规则

SSR 不是 IBL 的替代品：

```text
finalSpecular = mix(iblOrProbeSpecular,
                    ssrRadiance,
                    ssrConfidence)
```

当未选择 Environment 时，fallback 使用现有 constant ambient specular 约定或黑色；必须由 Renderer 明确报告。

#### 已知限制

- 无法反射屏幕外、相机背后和被前景挡住的物体。
- 薄几何体、depth discontinuity 和 rough surface 容易产生漏光或断裂。
- 不处理透明反射、递归镜面和真实折射。

### SSGI

#### 作用

SSGI 在屏幕空间追踪若干漫反射方向，从场景 radiance 估算一次间接漫反射。它可以表现近距离颜色反弹，例如红墙对相邻白色物体的染色。

#### 推荐实现

- 半分辨率 compute。
- 每像素 4 到 8 条 cosine-weighted rays，逐帧旋转采样方向。
- 使用与 SSR 相同的 screen-space depth pyramid 和 Scene Color pyramid。
- 输入应为未包含当前帧 SSGI 的 Base HDR，禁止从已经合成 SSGI 的颜色递归采样。
- 输出 `RGBA16F`，RGB 为 indirect radiance，A 为 hit/confidence。
- Temporal accumulation 后执行 variance-guided 或 depth/normal bilateral denoise。
- 对高 emissive 采样值执行合理 clamp，避免少数像素污染整片历史。

#### Fallback 与 AO

```text
finalDiffuse = mix(ambientOrIblDiffuse * materialAo * screenAo,
                   ssgiDiffuse * materialAo,
                   ssgiConfidence)
```

SSGI miss 必须回退到 ambient/IBL；不能让屏幕外区域突然变黑。Screen AO 不应对已命中的 SSGI 再做完整乘法。

#### 已知限制

- 只能使用屏幕可见信息，离屏发光体和被遮挡房间不会贡献。
- 大范围间接光不稳定，摄像机移动时可能出现 disocclusion noise。
- 不适合作为最终完整 GI，但非常适合作为编辑器中的实时基础模式和后续 DDGI 的近场补充。

### Local Reflection Probes

#### 作用

Local Reflection Probe 为 SSR miss 提供与当前位置更匹配的 cubemap，解决单一全局 IBL 在室内房间、走廊和局部空间中明显错误的问题。

#### 推荐范围

- Scene Entity 引用离线或编辑器捕获的 cubemap。
- Box/Sphere influence volume。
- 选择最近或最高优先级 Probe。
- Box projection/parallax correction。
- SSR hit 优先，Local Probe 次之，Global IBL 最后。

这一阶段需要新增 SceneDocument component、Cook closure、Probe capture/bake 和资源生命周期，不与 SSR v1 混在一个提交中。

### DDGI / Irradiance Probes

#### 作用

DDGI 使用三维 Probe Grid 保存方向性 irradiance 和 visibility，能够处理屏幕外间接光，并随场景光照变化逐步更新。

#### 前置条件

- 明确的世界空间 Probe Volume component。
- 场景几何查询能力：硬件 Ray Query、软件 BVH 或专用 raster capture 路径。
- Probe relocation/classification，避免探针落在墙内。
- Depth moments/visibility，抑制跨墙漏光。
- Probe update budget、滚动更新和异步资源生命周期。
- Trilinear/visibility-weighted sample，并与 near-field SSGI 组合。

#### 推荐组合

```text
near-field indirect diffuse = SSGI
off-screen / low-frequency = DDGI
specular fallback           = Reflection Probe / Global IBL
```

DDGI 不应作为屏幕空间基础设施完成前的第一个 GI 算法。

### 可选硬件光追路径

硬件光追可以进一步实现 Ray-Traced Reflection、Ray-Traced AO、Ray-Traced GI 或 Path-Traced Reference，但需要单独路线：

- Vulkan ray tracing capability 与 feature fallback。
- BLAS/TLAS build、update、compaction 和实例生命周期。
- Material/texture 的 shader-visible random access，通常需要 bindless descriptor。
- 多帧 sample accumulation、denoiser 和 disocclusion handling。
- 与 raster shadow、SSR/SSGI、Probe fallback 的混合策略。

建议先实现一个低速 Path-Traced Reference 或 RT Reflection，用于校验 raster/screen-space 算法，而不是立即用硬件光追替换全部 Forward Lighting。

### 不优先选择的算法

| 算法 | 暂不优先的原因 |
|---|---|
| HBAO | 与 GTAO 目标重叠，增加维护模式但收益有限 |
| LPV | 光泄漏明显、质量上限低，现代项目价值有限 |
| VXGI / Voxel Cone Tracing | 体素化、显存和更新成本较高，当前场景基础设施尚不匹配 |
| Baked Lightmaps | 需要 UV2、atlas、baker、静态/动态分类和 Cook 集成，是独立工具链项目 |
| Planar Reflection | 对水面/镜面很有效，但只解决特定平面，不是通用反射路线 |
| ReSTIR GI | 依赖完整 RT、reservoir history 和高质量 denoiser，复杂度远超当前阶段 |

## 分阶段实施计划

### Stage 0：共享 Screen-Space Effect 基础（已完成）

- 增加算法 mode、support/status 和统一参数验证。
- 增加独立 screen-space nearest-depth pyramid。
- 增加 Scene Color mip pyramid。
- 固化 temporal history/rejection 公共 helper。
- 为 GPU profiler、RenderDoc、Tracy 和 Debug View 建立统一命名。
- 设计 Opaque/Transparent 分离与 indirect-light breakdown，但只在 SSR 前实施需要的部分。

完成标准：关闭全部新功能时 Pass 调度和画面保持当前基线；调试视图能检查 depth pyramid、color pyramid、motion 和 history validity。

### Stage 1：SSAO Baseline（已完成）

- 半分辨率 SSAO trace。
- depth/normal bilateral blur。
- MainForward 只在间接项采样 AO。
- Editor 提供 Off/SSAO、参数和 Raw/Filtered debug。

实际实现还提供 Nearest Depth 与 Scene Color 指定 mip 调试、功能级 capability fallback、固定 `set=4` ScreenSpace ABI，以及 Runtime Control/CLI 状态查询。关闭效果时不 dispatch SSAO；非 PBR variant 仅在显式 AO Debug 时执行。

可选 CACAO comparison backend 复用同一个 `set=4` AO output contract，Editor 与 Runtime Control 可在 Off/SSAO/CACAO 间即时切换，并提供 Quality、Native/Half、Radius、Intensity、Power 和 `CACAO Output` 调试视图。它只在 `windows-msvc-ao-compare` 中启用；普通配置继续使用内置 SSAO 且没有 FidelityFX 链接依赖。

完成标准：Algorithm Playground 与 Sponza 墙角出现稳定接触遮蔽，Direct Light 和 Emissive 不被压暗。

### Stage 2：TAA v1（已完成）

- Projection jitter。
- HDR history reproject、clamp、reject 和 resolve。
- Camera cut、resize、scene publish 和 active camera 切换失效。
- Editor 提供 Off/TAA 和 history debug。

实际实现使用 8-phase Halton 2/3 jitter，并明确分离 stable projection 与 jittered projection：前者用于 camera/shadow culling 和 history 失效判断，后者只进入渲染、motion 与重投影。每个 frame slot 持有全分辨率 `RGBA16F` history/debug image；Registry 的 previous-frame sampled-read 契约用于表达跨 slot 依赖。

Resolve 使用 nearest-depth motion、depth/normal/history-validity rejection、3x3 YCoCg variance clipping、自适应 motion/luminance history weight 和轻量 sharpening。Sky 使用 previous view-projection 重投影；透明材质当前没有 reactive mask，因此只要帧中存在透明 draw 就保守限制 history weight。Camera cut、viewport resize、scene generation、camera mode、projection、Shader variant 和不连续执行都会重置 history。

TAA 位于完整 MainForward 之后、Scene Color Pyramid/Bloom/ToneMap 之前。激活时后三者直接读取 resolve history，关闭时继续读取原 HDR，不增加全屏复制。Editor、Runtime Control 与 `VulkanLabCtl` 提供 Off/TAA、History Weight、Sharpness，以及 History、Rejection、History Weight 调试视图；GPU profiler、Tracy 和 RenderDoc 使用独立 `TAA` 区域。

实现参考本地 `references/rendering_algorithms/code/taa` 中固定提交 `39786709cf70a1e0906196c600f6079571a33ceb` 的 MIT 代码，以及 UE4 2014 temporal supersampling 资料，用于核对 jitter、reprojection 和 neighborhood clipping 的职责边界。正式实现是项目内 Vulkan/GLSL 重写，不 include、link 或运行参考代码，因此没有新增运行时依赖。

完成标准：静态边缘稳定，移动时无明显长时间拖影；UI 不经过 TAA。

### Stage 3：GTAO（已完成）

- Horizon search。
- 复用 AO output contract 与 bilateral filter。
- 接入 temporal accumulation。
- Editor 提供 Off/SSAO/GTAO。

实际实现为项目内 Vulkan/GLSL 重写：半分辨率 trace 沿 2/3/4 个切片执行 2/4/6 步 horizon search，并复用 nearest-depth Screen Pyramid 选择保守 LOD。Trace 结果通过独立的 GTAO history 做 motion reprojection、depth/normal rejection、3x3 neighborhood clamp 和可调 history weight，再复用 SSAO 的两次 5-tap bilateral blur 输出统一 AO contract。

GTAO history 独立于 TAA，响应 frame-level camera cut、viewport resize、scene generation、camera mode、projection、Shader variant、设置签名和执行序列中断。Editor、Runtime Control 与 `VulkanLabCtl` 提供 Quality、Radius、Falloff、Intensity、Power、History Weight，以及 Raw、Temporal、Filtered、Rejection 和 History Weight 调试视图。GPU profiler、Tracy 和 RenderDoc 使用独立 `GTAO` 区域。

实现参考本地 `references/rendering_algorithms/downloads/code/xegtao` 固定提交 `a5b1686c7ea37788eeb3576b5be47f7c03db532c` 和 `gtao_2016.pdf`。参考代码采用 MIT 许可证，仅用于核对 horizon integral、采样分布和 temporal/denoise 职责；正式实现不 include、link 或运行 XeGTAO，并暂不移植其 Hilbert LUT、fp16 path、depth prefilter 和 multiple-bounce approximation。

完成标准：GTAO 在大尺度遮蔽和视角变化下优于 SSAO，切换模式不重建场景或材质。

### Stage 4：SSR

- 拆分 Opaque 与 Transparent 执行位置。
- 输出 baseline indirect specular。
- hierarchical ray trace、color pyramid、confidence 和 temporal resolve。
- 按 confidence 替换 IBL/Probe specular。

完成标准：ChronographWatch、CarConcept 和 Sponza 光滑表面获得可见反射，屏幕边缘和 miss 区域平滑回退 IBL。

### Stage 5：SSGI

- 输出 baseline indirect diffuse。
- diffuse ray trace、confidence、temporal accumulation 和 denoise。
- 按 confidence 混合 ambient/IBL fallback。
- 明确避免 AO 双重变暗和 SSGI feedback。

完成标准：GI Stress 场景出现稳定的近场颜色反弹；镜头移动时没有大面积历史残影或场景颜色持续增亮。

### Stage 6：Local Reflection Probes

- Scene component、资源导入/捕获、influence volume 和 parallax correction。
- SSR -> Local Probe -> Global IBL fallback 链。
- Native Scene Cook closure。

### Stage 7：DDGI

- Probe Volume、更新预算、visibility、relocation/classification。
- 与 SSGI 的 near/far-field 混合。
- Editor 可视化 Probe 状态与更新成本。

### Stage 8：可选硬件光追与 Reference

- Vulkan RT capability、AS 管理和 material access。
- 优先实现 reference path 或 RT Reflection。
- 再评估 RT AO/GI、denoiser 和 hybrid fallback。

## Editor 与 Runtime Control

建议在 `Render -> Lighting` 和 `Render -> Post Processing` 中组织：

```text
Ambient Occlusion
  Mode: Off / SSAO / GTAO
  Radius / Intensity / Quality

Anti-Aliasing
  Mode: Off / TAA
  History Weight / Sharpness

Reflections
  Mode: IBL Only / SSR
  Max Distance / Thickness / Roughness Cutoff / Quality

Global Illumination
  Mode: Ambient or IBL / SSGI
  Intensity / Ray Distance / Quality
```

UI 应同时显示 Support、Active、Fallback 和最近 history reset 原因。算法设置由 UI 与 Runtime Control 共用同一校验和写入函数。

Runtime Control 继续扩展 `render_settings.get/set` 和 `render.status`，不为每个算法创建独立协议。RenderTest schema 只在需要自动固定算法配置时升级，并保持旧 schema 默认关闭新增效果。

## 性能与资源策略

| 阶段 | 推荐分辨率 | 主要成本 |
|---|---:|---|
| SSAO/GTAO | 1/2 | depth/normal samples、blur、history |
| SSR | 1/2 起步 | hierarchical trace、color pyramid、temporal |
| SSGI | 1/2 | 多射线 trace、temporal、denoise |
| TAA | Full | HDR history read/write、neighborhood clamp |
| Composite | Full | 多张 indirect buffer 读取 |

- Quality preset 映射 sample/direction/step 数，Shader 不读取自由变化的超大 loop 上限。
- Disabled effect 不 dispatch；v1 可保留其资源以保证即时切换，显存压力明确后再做 lazy residency。
- 所有 history 和 effect image 使用 per-frame 资源，禁止跨未完成 frame slot 写入。
- 每个 Pass 独立 GPU timestamp；效果面板同时显示分辨率、dispatch 数和历史有效率。
- Main Sponza 与 GI Stress 必须比较 MainForward、screen effect 和总 GPU 时间，不能只看单个 Pass。

## 诊断场景

- **Algorithm Playground**：检查 AO 半径、薄几何体、SSR 边缘和 TAA motion。
- **Sponza GI Stress**：检查室内遮蔽、颜色反弹、屏幕外 fallback 和漏光。
- **ChronographWatch**：检查金属、粗糙度变化和 SSR/IBL 混合。
- **CarConcept**：检查车漆、玻璃排除、强反射和高动态范围。
- **Atmosphere Scene**：检查没有 Environment 时的明确 fallback，以及 aerial perspective 合成顺序。

## Verification

遵循项目默认开发策略，只进行受影响配置的构建和实际运行，不默认执行 CTest、Golden、视觉回归或 Validation smoke。

每一阶段至少完成：

1. 构建并启动 `windows-msvc-dev-fast`。
2. 构建 `windows-msvc-runtime`，确认无 Editor 路径仍可运行。
3. 在对应诊断场景手动切换 Off 与新算法，确认关闭后恢复基线。
4. 检查 viewport resize、scene reload、camera cut 和 Editor/Active Camera 切换后的 history。
5. 使用 GPU timings 比较新增 Pass 与 MainForward 总成本。
6. 必要时使用 RenderDoc 检查 resource、mip、descriptor、barrier 和 history image。
7. 执行 `git diff --check`。

需要修改 history、barrier 或跨 Pass layout 时，建议额外人工运行 Synchronization Validation，但不将其加入默认快速验证流程。

## 关键决策

- AO 第一版选择 SSAO，质量模式选择 GTAO，不同时实现 HBAO。
- TAA 在 SSR/SSGI 之前完成。
- 当前 Occlusion max-depth Hi-Z 保持不变，屏幕空间射线使用独立 depth pyramid。
- SSR 和 SSGI 使用 confidence 替换 fallback，不向最终 HDR 无条件相加。
- v1 屏幕空间算法只保证 Opaque/MASK；Transparent 在 composite 后绘制。
- SSR miss 回退 IBL，SSGI miss 回退 ambient/IBL。
- Reflection Probe 和 DDGI 分别作为局部 specular 与 off-screen diffuse 的后续方案。
- 硬件光追是可选增强路径，不成为基础 renderer 的启动要求。

## Assumptions

- 继续使用 Vulkan graphics queue 上的 compute，不先引入 async compute。
- 当前 Surface Data format 和 Motion Vector 精度可满足第一版 temporal 算法。
- 第一版目标是可比较、可切换、可诊断的基础算法，不追求与 UE/Lumen 等完整系统等价。
- Atmosphere-derived dynamic IBL、透明 motion、specular AA、upscaling 和 ray-traced denoiser分别留给后续独立阶段。
- 实施时每个 Stage 先单独计划，再开始代码修改。
