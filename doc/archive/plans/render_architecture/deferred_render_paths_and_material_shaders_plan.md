# Forward / Deferred Render Path 与 Material Shader Family 实施计划

> Status: Completed
> Last verified: 2026-08-21
> Verified against: working tree based on `11b9e02`

## Summary

在现有 Vulkan 1.3、RenderGraph、Dynamic Rendering 和 Bindless Material 基础上，引入可切换的 Forward / Deferred 不透明渲染路径，并把当前全局 `ShaderVariant` 拆成三个相互独立的概念：

```text
Renderer
  ├─ Shared Frame Infrastructure
  │    ├─ RenderGraph / Resource Pool / Pipeline Cache
  │    ├─ Visibility / Shadows / Atmosphere
  │    ├─ AO / SSR / SSGI / DDGI
  │    ├─ TAA / Bloom / ToneMap / Present
  │    └─ Forward Transparent
  │
  └─ Opaque Path Assembly
       ├─ Forward
       │    └─ MainForwardOpaque
       └─ Deferred
            ├─ GBufferPass
            ├─ ClusteredLightCullingPass
            └─ DeferredLightingPass

Material
  ├─ MaterialShaderFamily  // 每种材质可参与哪些 Pass
  ├─ MaterialTemplate      // shader family + render state
  └─ MaterialInstance      // 参数、纹理、GpuMaterial handle

Editor
  ├─ Render Path           // Forward / Deferred
  └─ View Mode             // Lit / BaseColor / Normal / ...
```

不创建两套完整 `Renderer`，不复制 Shadow、Post Process、资源池或帧同步。Forward 和 Deferred 只负责不透明表面的几何与光照方式；BLEND、Transmission 和其他难以进入 GBuffer 的材质继续使用 Forward Transparent。

## Goals

- 运行时可在 Forward 与 Deferred 间切换，不重建设备、Swapchain 或顶层 Renderer。
- 材质决定自己使用的 shader family；编辑器全局控件只决定 Render Path 和 View Mode。
- Forward 和 Deferred 共用同一套材质求值、BRDF、阴影、IBL、大气和后处理实现。
- Deferred v1 与现有 SSR、SSGI、DDGI、TAA 和透明路径兼容。
- 为后续 Tiled/Clustered Lighting、Decal 和更多 Shading Model 建立稳定接口。
- 保留 Forward 作为少量灯光、MSAA、透明和兼容性路径。

## Non-Goals

- 不引入第二个顶层 Renderer、RHI 或另一套 RenderGraph。
- 不实现 Vulkan Subpass Deferred、Tile-based Deferred 或 GBuffer transient aliasing。
- Deferred v1 不支持 MSAA；使用单采样 GBuffer 与 TAA。
- 不实现材质节点图、任意用户 shader、运行时 shader 编译或 shader hot reload。
- 不实现透明 Deferred、OIT、Deferred Decal、Ray Traced Deferred 或 async compute。
- 不在本阶段移除 Legacy Forward；它继续作为兼容和基线模式。

## Current Architecture Assessment

现有架构已经具备实现 Deferred 的主要基础：

- RenderGraph 能按 feature 构建条件节点、跟踪资源版本并生成 Synchronization2 barrier。
- Dynamic Rendering 和完整 `PipelineRenderingSignature` 已消除固定 RenderPass 依赖。
- `SurfacePrepass` 已能输出 Depth、Normal/Roughness、Motion 和 Albedo/Metallic。
- Main Forward 已输出 HDR、Baseline Diffuse 和 Baseline Specular，SSR/SSGI Composite 可以继续复用这些产品。
- Bindless `MaterialSystem` 已提供全局 `GpuMaterial[]` 和纹理表。
- `PipelineCache` 已按真实 Vulkan 状态建 key，可以自然缓存不同 Render Path 的 pipeline。

当前主要问题不是缺少 Pass，而是概念耦合：

- `ShaderVariant` 同时表示材质 shader、全局调试视图、Tone Mapping policy 和屏幕空间能力。
- `MainForwardPass` 使用一个全局 variant 绘制全部材质，材质本身不拥有 shader family。
- `FrameFeatureResolver` 从全局 variant 推导功能，无法正确表达混合材质和不同 Render Path。
- `SurfacePrepass` 位于 `core_forward` 目录，但实际已经是跨路径共享的 Surface/GBuffer 基础。
- Visibility Hi-Z 使用最大深度，Screen Depth Pyramid 使用最小深度，重复构建且未来 Clustered Lighting 同时需要两者。

因此先收口 shader/material 和共享 surface 数据，再实现 Deferred Lighting。直接新增一个全屏光照 Pass会继续放大现有耦合。

## Architectural Boundaries

### Renderer

`Renderer` 继续唯一拥有：

- `RenderGraphCompiler`、`RenderGraphExecutor` 和物理资源池。
- `PipelineCache`、descriptor 基础设施和 frame resources。
- Visibility、ShadowSystem、GPU profiler 和 Capture 接入。
- 当前 `RenderPathMode` 和 `ViewMode`。

`Renderer` 不根据路径创建两个长期并存的完整对象。请求值与最终活动路径必须分开表示，
避免把 `Auto` 当成可执行路径传播到 Pass：

```cpp
enum class RenderPathRequest {
    Auto,
    Forward,
    Deferred,
};

enum class RenderPathMode {
    Forward,
    Deferred,
};

struct RenderPathSelection {
    RenderPathRequest requested{RenderPathRequest::Auto};
    RenderPathMode active{RenderPathMode::Forward};
    RenderPathCapabilities capabilities;
    bool viewModeCompatible{true};
    std::string fallbackReason;
};
```

路径选择由一个无 Vulkan 副作用的 resolver 在 Graph build 前完成。`Auto` 可以回退，强制
`Deferred` 在设备、View Mode 或 shader family contract 不满足时必须明确失败。路径装配保持
为无状态的 Graph 构建逻辑，不创建第二个顶层 Renderer，也不为了两个分支引入长期多态对象：

```cpp
OpaqueRenderProducts addOpaquePath(
    RenderGraphBuilder& graph,
    const RenderPathBuildContext& context,
    RenderPathMode activePath);
```

它只选择已有 Pass 的条件节点并返回标准化输出，不拥有 Device、Swapchain、frame fence 或独立
资源池。未来只有在出现第三条 opaque path、独立插件装配或明显重复逻辑时，才把该函数边界升级
为 `IOpaqueRenderPath`；v1 不提前支付抽象成本。

### Standard Opaque Products

Forward 和 Deferred 必须返回同一逻辑产品。路径自己的 depth attachment 与共享屏幕空间
Surface 数据必须分开；二者格式和编码并不等价：

```cpp
struct ScreenSpaceSurfaceProducts {
    RgImageHandle depth;            // sampled linear/reconstructable surface depth
    RgImageHandle normalRoughness;  // canonical screen-space encoding
    RgImageHandle motion;
};

struct OpaqueRenderProducts {
    RgImageHandle hdrColor;
    RgImageHandle baselineDiffuse;
    RgImageHandle baselineSpecular;
    RgImageHandle geometryDepth;    // Forward main depth or Deferred GBuffer depth
    ScreenSpaceSurfaceProducts screenSpace;
};
```

Stage 5 先保留共享 `SurfacePrepass` 作为 GTAO、SSR、SSGI 和 TAA 的 canonical surface
producer，避免这些算法直接解释 GBuffer 的 oct-normal/flags 编码。Deferred 使用 GBuffer depth
作为透明绘制的 `geometryDepth`，但屏幕空间算法仍读取 `screenSpace`。后续确认编码与带宽收益后，
可以让 GBuffer 同时实现 canonical surface contract，移除重复 prepass，而不改变消费者接口。

SSR、SSGI、DDGI Composite、透明渲染、TAA 和后处理只消费活动路径返回的标准产品，不在各
Pass 中散布 `if (deferred)`。若某产品在特定设置下没有真实 producer，路径必须提供合法
fallback 或明确关闭对应 feature。

### Shared Features

以下功能不属于 Forward 或 Deferred，继续由顶层 Graph 组装：

- Directional/Point/Spot Shadow。
- Atmosphere LUT 和 Sky Background。
- Visibility、Hi-Z 和 Occlusion Cull。
- GTAO/SSAO、SSR、SSGI、DDGI。
- Main Forward Transparent。
- TAA、Bloom、ToneMap、Present 和 Capture。

### Transparent Policy

以下材质始终进入 Forward Transparent：

- `AlphaMode::Blend`。
- Transmission 大于零的材质。
- 未来无法稳定编码进 GBuffer 的特殊透明 shading model。

Opaque 和 MASK 可以进入 Forward Opaque 或 GBuffer。MASK 在 GBuffer Pass 中执行相同 alpha cutoff，Shadow 行为不变。

## Material Shader Architecture

### Concepts

将当前 `ShaderVariant` 拆为：

```cpp
enum class MaterialDomain {
    Surface,
    Transparent,
};

enum class ShadingModel {
    DefaultLit,
    Unlit,
};

enum class MaterialPass {
    ForwardOpaque,
    ForwardTransparent,
    GBuffer,
    ShadowMask,
    SurfaceDepth,
};

enum class ViewMode {
    Lit,
    BaseColor,
    WorldNormal,
    Metallic,
    Roughness,
    Occlusion,
    Emissive,
    ShadowVisibility,
    ScreenSpaceDebug,
};
```

`MaterialShaderFamily` 描述一个材质体系在不同 Pass 中使用的程序：

```cpp
struct MaterialShaderFamily {
    std::string id;
    std::string displayName;
    MaterialDomain domain;
    ShadingModel shadingModel;
    std::unordered_map<MaterialPass, ShaderProgramId> programs;
    MaterialShaderCapabilities capabilities;
};
```

`MaterialTemplate` 改为持有：

- `MaterialShaderFamilyHandle`。
- alpha、cull、depth write/test 等 pipeline render state。
- `MaterialSystem` 提供的 descriptor layout 引用。

`MaterialInstance` 继续持有：

- CPU `MaterialParams`，供 Inspector、排序和 pass classification 使用。
- Texture 引用。
- `MaterialHandle` / `GpuMaterial` index。
- 对共享 `MaterialTemplate` 的引用。

glTF PBR 材质默认绑定内置 `builtin.default-lit` family。未来材质资产可以选择 family，但 Deferred v1 不新增独立 `.vkmat` 文件。

### Program Resolution

Pass 不再读取全局 shader variant，而是按 draw 解析程序：

```cpp
const auto& family = material.shaderFamily();
const auto program = shaderRegistry.resolve({
    .family = family.id,
    .pass = MaterialPass::GBuffer,
    .viewMode = viewMode,
    .bindingMode = materialSystem.bindingMode(),
});

const auto pipeline = pipelineCache.getOrCreate(
    program,
    material.pipelineState(),
    graphPass.renderingSignature());
```

Bindless 模式仍只绑定一次全局 material set，每 draw push `materialIndex`。Legacy binding backend 继续按材质绑定固定 descriptor，但使用相同 `GpuMaterial` ABI。

### Deferred Shading Model

`DeferredLightingPass` 不是每个材质独立运行的 shader。GBuffer 保存解析后的材质属性和小型 `ShadingModelId`，Lighting Pass 根据该 ID 选择内置 shading 分支。

v1 只支持：

- `DefaultLit`：完整 PBR、阴影、IBL、AO、大气和屏幕空间效果。
- `Unlit`：输出 emissive/base color，不执行灯光循环。

未知或不支持的 shading model：

- Auto/Forward 下回退 Forward。
- 强制 Deferred 时记录原因并使用 DefaultLit fallback material，不允许静默产生黑色材质。

### Shader Manifest v3

Manifest 从当前 program/variant 混合结构演进为：

```json
{
  "schemaVersion": 3,
  "programs": [],
  "materialShaders": [
    {
      "id": "builtin.default-lit",
      "domain": "surface",
      "shadingModel": "default-lit",
      "passes": {
        "forwardOpaque": "material.default-lit.forward",
        "forwardTransparent": "material.default-lit.forward-transparent",
        "gbuffer": "material.default-lit.gbuffer",
        "shadowMask": "material.default-lit.shadow-mask",
        "surfaceDepth": "material.default-lit.surface"
      }
    }
  ],
  "viewModes": []
}
```

规则：

- `programs` 仍是实际 vertex/fragment/compute SPIR-V 定义。
- `materialShaders` 只做 family/pass 到 program 的映射。
- `viewModes` 定义全局可视化策略和 Tone Mapping/Bloom policy。
- Bindless/Legacy SPIR-V 仍由 program backend 选择，不复制 family。
- Manifest 加载时校验 family 的必需 Pass、domain 和 attachment contract。

旧 manifest schema v2 在开发构建中可迁移到内存中的默认 family/view mode；Cooked package 必须重新 Cook 并使用 schema v3。

## Shared GLSL Boundaries

在增加 Deferred 前先拆分共享 GLSL，避免 Forward 和 Deferred 两套 PBR 逐步漂移：

```text
shader/include/material_surface.glsl
  - texture/material table 求值
  - normal mapping
  - alpha cutoff
  - resolved SurfaceData

shader/include/pbr_brdf.glsl
  - GGX / Fresnel / geometry terms

shader/include/direct_lighting.glsl
  - directional/point/spot
  - shadow visibility

shader/include/indirect_lighting.glsl
  - ambient / IBL / AO / DDGI

shader/include/aerial_perspective.glsl
  - atmosphere transmittance/in-scattering

shader/include/surface_encoding.glsl
  - oct normal、GBuffer pack/unpack 和 flags
```

Forward Fragment 直接消费 `SurfaceData`；GBuffer Fragment 编码 `SurfaceData`；Deferred Lighting 解码后调用同一 BRDF/Lighting helper。

## GBuffer Contract v1

Deferred v1 使用单采样、全分辨率资源：

| Resource | Preferred Format | Contents |
| --- | --- | --- |
| Surface Depth | existing supported depth format | 普通 Z 深度 |
| GBuffer0 | `R8G8B8A8_UNORM` | BaseColor RGB, Metallic |
| GBuffer1 | `R16G16B16A16_SFLOAT` | Oct World Normal XY, Roughness, Material AO |
| GBuffer2 | `R16G16B16A16_SFLOAT` | Emissive RGB, integer Surface Flags as float |
| Motion | `R16G16_SFLOAT` | Current-to-previous UV motion |

`Surface Flags` 使用小整数 bit field，并以 float 精确保存：

```text
bit 0      history valid
bits 1..3 shading model
bit 4      receives screen-space AO
bits 5..7 reserved
```

所有消费者通过 `surface_encoding.glsl` 访问，不直接解释 attachment channel。这样可以把当前存储在 Normal/Roughness alpha 的 history validity迁入统一 flags，同时为 shading model保留稳定 ABI。

约束：

- GBuffer 存最终材质值，不存 material index替代纹理采样；Deferred Lighting没有 UV/tangent，不能重新求值贴图。
- Position 从 full-resolution Surface Depth 和 inverse view-projection重建，不增加 position attachment。
- Normal 使用现有 oct encoding约定。
- Emissive 保留 HDR 范围，因此使用 FP16。
- Deferred v1 不对 GBuffer 使用 MSAA。

按 1920×1080 估算，Depth + 三张 GBuffer + Motion约为 28 bytes/pixel，即每个 frame slot约 55–60 MiB。现有 SurfacePrepass资源会被替换或复用，因此诊断必须同时显示 active bytes、resident bytes和切换路径后的增量，而不能只计算新 attachment总和。

设备初始化时校验各 format 的 color attachment、sampled 和需要时 storage能力。缺少必需格式时禁用 Deferred，Forward 继续运行，并报告具体 format capability原因。

## Depth Hierarchy Consolidation

在 Deferred Lighting 前收口当前两套深度金字塔：

```text
DepthHierarchy RG32F
  R = nearest/min depth
  G = farthest/max depth
```

- Occlusion Cull 读取 max depth。
- GTAO、SSR、SSGI 读取 min depth。
- 后续 Clustered Light Culling同时使用 tile min/max。
- mip 0从 Surface Depth生成，后续每级同时 reduce min/max。

如果设备不支持 `RG32F` sampled + storage，则保留当前双 `R32F` 实现作为 fallback。该改动独立于 Render Path；它不能成为 Deferred correctness 的硬依赖。

## RenderGraph Topologies

### Forward

```text
Shadow / Atmosphere
  -> SurfacePrepass (按 feature 需要输出)
  -> Visibility / Depth Hierarchy / AO
  -> SkyBackground
  -> MainForwardOpaque
  -> SSR / SSGI / DDGI Composite
  -> MainForwardTransparent
  -> TAA / Bloom / ToneMap / Present
```

### Deferred

```text
Shadow / Atmosphere
  -> GBufferPass
  -> Visibility / Depth Hierarchy / AO
  -> LightCullingPass
  -> SkyBackground (clear/write HDR background)
  -> DeferredLightingPass (skip background depth)
  -> SSR / SSGI / DDGI Composite
  -> MainForwardTransparent (read GBuffer depth)
  -> TAA / Bloom / ToneMap / Present
```

`DeferredLightingPass` 使用 compute，原因是：

- 能自然读取 GBuffer、light lists 和 shadow resources。
- 写入 HDR/Baseline Diffuse/Baseline Specular storage image。
- 后续 Clustered Lighting和空背景 early-out更直接。
- 不依赖 fullscreen graphics pipeline和额外 color attachment签名。

Graph 负责 Sky color attachment write到 Deferred compute storage read/write的 layout与 barrier。

### Topology Switching

`RenderPathMode` 纳入 `RenderGraphTopologyKey`。切换时：

- 重编译 Graph topology。
- 懒创建目标路径需要的物理资源；已创建资源保留 resident，避免来回切换抖动。
- 不重建设备、Swapchain、MaterialSystem或 PipelineCache。
- 不调用 `vkDeviceWaitIdle()`。
- 通过现有 frame-fence/serial机制安全更新 descriptor。
- 重置 TAA、SSR、SSGI及其他依赖 shading输出的 temporal history。
- Shadow和 Atmosphere静态 history不因路径切换重建。

## Feature Resolution

用以下输入替代当前 `ShaderVariant` capability：

```cpp
struct FrameFeatureInput {
    RenderPathMode renderPath;
    ViewMode viewMode;
    RenderPathCapabilities pathCapabilities;
    SceneMaterialCapabilities materialCapabilities;
    DeviceFeatureSupport deviceSupport;
    RenderSettings settings;
};
```

规则：

- View Mode决定最终展示和 post-process policy。
- Render Path决定 opaque producer能力。
- 材质 capability只用于 queue classification和 fallback统计，不让单个材质任意改变全帧 Graph。
- AO/SSR/SSGI/DDGI是否执行由设置、路径输出和设备能力共同决定。
- Bloom/Tone Mapping不再挂在 material shader family上。
- Legacy View Mode可以强制 Forward，并在 UI明确显示 `Forward Only`。

## Tiled / Clustered Lighting Direction

Deferred correctness完成后再引入共享 Light Culling，避免一开始同时调试 GBuffer和复杂 light list：

```text
DepthHierarchy
  -> ClusterBuildPass
       -> ClusterGrid SSBO
       -> ClusterLightIndices SSBO

DeferredLighting      -> consume cluster lists
ForwardTransparent    -> consume same cluster lists
ForwardOpaque(optional) -> consume same lists
```

建议 v1 参数：

- XY tile：16×16 pixels。
- Z slices：24，按 view-space logarithmic分布。
- Directional lights不进入 cluster，始终单独遍历。
- Point/Spot对 cluster frustum做保守相交。
- 首版继续受当前 256 scene light上限约束。
- index overflow必须计数和报告，不能越界或静默覆盖。

Clustered Lighting是 Deferred在多灯场景中体现价值的关键阶段，但不作为 Deferred最初画面正确性的前置条件。

## Editor And Runtime Control

### Editor

Render 面板将当前 Shader选择拆为：

```text
Rendering
  Render Path   [Forward | Deferred]
  View Mode     [Lit | BaseColor | Normal | ...]

Material Inspector
  Shader Family builtin.default-lit
  Domain        Surface
  Shading Model Default Lit
  Deferred      Supported
```

行为：

- Render Path是全局会话设置，不写入 SceneDocument v3。
- View Mode是编辑器调试状态，不写入 SceneDocument。
- 切到 Deferred不支持的 View Mode时，UI禁用选择或明确回退 Forward；不得隐式产生不同画面。
- Materials面板显示 family、domain、shading model和实际 pass fallback。
- Diagnostics显示当前路径、GBuffer格式/内存、各队列 draw数和 path fallback材质数。

### Runtime Control

新增：

```text
render_path.get
render_path.set forward|deferred
view_mode.get
view_mode.set <id>
```

现有 `shader.list/current/set` 保留一个迁移周期：

- Debug shader ID映射到对应 View Mode。
- PBR Forward ID映射到 `ViewMode::Lit`，不强制修改材质 family。
- Legacy ID映射到 Legacy View Mode并强制 Forward。
- 响应增加 `deprecated=true` 和替代方法。

协议版本只在现有字段无法兼容时升级；命令错误必须返回 `render_path_unsupported`、`view_mode_unsupported` 或 `material_family_incompatible` 等明确原因。

## Cook And Compatibility

- glTF、Native BC7、Catalog、SceneDocument和 Environment缓存不迁移。
- 所有现有 glTF PBR材质绑定 `builtin.default-lit`。
- SceneDocument v3不新增 render path字段；Cooked runtime由启动配置选择路径。
- Cook复制 Manifest中 Forward、Deferred、Legacy和 View Mode所引用的全部 SPIR-V。
- Manifest schema和 GBuffer ABI变化后，旧 Cooked package必须重新 Cook。
- 强制 Deferred但设备不支持时，Cooked runtime启动失败并报告具体 capability；Auto模式回退 Forward。
- Runtime preset仍可包含两个路径。未来如需裁剪，可增加独立 build feature，但不在本阶段引入。

## Implementation Stages

### Stage 0：基线、契约与目录收口（Completed 2026-08-16）

目标：在改变画面前固定性能和接口基线。

工作：

- 记录 Algorithm Playground与 Main Sponza在 Forward下的 CPU/GPU pass时间、draw数和显存。
- 记录 AO、SSR、SSGI、DDGI、TAA不同组合下的 Graph topology。
- 将 `SurfacePrepass` 从 `core_forward`迁入共享 `surface`或 `geometry` feature目录。
- 定义 `RenderPathMode`、`ViewMode`、`OpaqueRenderProducts`和 `RenderPathCapabilities`。
- 写出 GBuffer C++/GLSL静态 ABI，确定所有 attachment format和 flags。
- 给 Diagnostics增加只读的现有 path/output状态，为后续对比提供入口。

完成条件：

- 仍只运行现有 Forward路径。
- Shader、画面和现有 RenderGraph topology不变。
- GBuffer ABI由 C++ static assertions和 shader contract描述，不存在未定 channel。

实施结果：

- 新增 `render/path/RenderPath.h`，固定 `RenderPathMode`、
  `OpaqueRenderProducts`、capabilities和只读状态接口；ViewMode 的权威定义已在
  Stage 1 迁入 Shader Registry。
- 新增 C++/GLSL共享 GBuffer contract，固定三张 attachment、surface flags、
  FP16精确编码范围、28 B/px名义总开销和 oct normal helper。
- `SurfacePrepass`已从 `core_forward`迁入共享 `features/surface`，Pass名称、
  condition、shader program和 RenderGraph topology不变。
- Diagnostics和 `render.status.renderPath`已显示当前路径、兼容 ViewMode、
  opaque outputs和尚未实现的 Deferred contract。
- 性能工具已记录 topology hash、完整 execution order、screen-space状态，并增加
  TAA、DDGI和 SSGI+DDGI profile。

Stage 0 Release baseline：RTX 4060 Laptop GPU、1280×720、Bindless、无 GUI、
Validation Off；每项使用1秒 warmup和3秒采样。机器数据保存在忽略目录
`build/perf-results/deferred-stage0/before/`，不提交到仓库。

| Scene / Profile | FPS | GPU ms | Active Passes | Dependencies | Barriers |
| --- | ---: | ---: | ---: | ---: | ---: |
| Algorithm Playground / Minimal | 331.0 | 0.70 | 9 | 15 | 83 |
| Algorithm Playground / Default | 326.2 | 1.10 | 42 | 77 | 145 |
| Algorithm Playground / SSAO | 329.0 | 0.87 | 14 | 28 | 93 |
| Algorithm Playground / SSR | 331.1 | 1.23 | 40 | 120 | 172 |
| Algorithm Playground / SSGI | 327.9 | 2.21 | 40 | 121 | 180 |
| Main Sponza / Minimal | 329.9 | 1.87 | 5 | 4 | 13 |
| Main Sponza / Default | 160.5 | 6.11 | 24 | 38 | 47 |
| Main Sponza / SSAO | 238.7 | 4.17 | 10 | 17 | 23 |
| Main Sponza / SSR | 210.1 | 4.50 | 36 | 109 | 102 |
| Main Sponza / SSGI | 194.7 | 5.07 | 36 | 110 | 110 |

### Stage 1：Shader Registry 与 Material Shader Family迁移（Completed 2026-08-17）

目标：消除“一个全局 shader绘制所有材质”的架构限制，但不启用 Deferred。

工作：

- 实现 Manifest schema v3和 schema v2开发期迁移。
- 新增 `MaterialShaderFamilyRegistry`。
- `MaterialTemplate`持有 family handle。
- 把当前 NormalMapped PBR注册为 `builtin.default-lit`。
- 把 debug variants迁为 `ViewMode`。
- 把 Legacy保留为 forward-only family/view policy。
- MainForward、Surface和 Shadow Mask按 material family/pass解析 program。
- `FrameFeatureResolver`改用 Render Path、View Mode和能力输入。
- 保持 Bindless/Legacy material binding两种 backend。

完成条件：

- Forward画面和现有调试视图行为保持一致。
- 同一 RenderQueue中可以存在不同 shader family的 opaque材质。
- 全局 UI不再把 Material Shader与 View Mode混为一个 combo。

实施结果：

- Shader Manifest 已升级到 schema v3，根对象明确拆分为 `programs`、
  `materialShaderFamilies` 和 `viewModes`；运行时仍可读取 schema v2 并迁移
  旧 `variants`。
- 新增只读 `MaterialShaderFamilyRegistry`、稳定 family handle 和七类材质
  pass contract。`MaterialTemplate` 持有 family handle，Repository 为每个注册
  family 复用一份模板。
- `builtin.default-lit` 使用现有 NormalMapped PBR；新增 `builtin.unlit`，并将
  glTF `KHR_materials_unlit` 映射到该 family。同一 ModelAsset/RenderQueue 可混合
  PBR 与 Unlit 材质而不复制纹理或 Mesh。
- MainForward、SurfacePrepass、Directional/Point/Spot MASK Shadow 均按
  `(MaterialShaderFamily, MaterialShaderPass)` 解析 program。RendererProgramCatalog
  不再保存全局材质 fragment 路径。
- 全局选择已迁为 `ViewMode`。默认 Lit 使用材质 family；Legacy、简化 PBR 和
  Debug 保留为显式 override policy。Feature Resolver、ToneMap、Bloom、Atmosphere、
  DDGI、编辑器和 Runtime adapter 均消费 ViewMode。
- 外部 `shader.*` Runtime 命令及既有 shader ID 暂时保留为兼容入口，但返回项
  标记 `kind=viewMode`；UI 标签改为 `View Mode`，Materials Inspector 显示实际
  Shader Family。
- Bindless 与 Legacy backend 共用同一 family/program 解析链路。dev-fast 已完成
  Lit、Legacy、Debug Normal 启动冒烟，三条路径均无 validation error。

### Stage 2：共享 Surface Evaluation 与 Depth Hierarchy

目标：Forward/GBuffer复用材质和表面求值，并为 light culling准备深度基础。

> Status: Completed
> Started: 2026-08-17
> Completed: 2026-08-18

#### 现状审计与约束

- `SurfacePrepass` 已经生成 sampled depth、octahedral world normal、roughness、
  motion 和按需 albedo/metallic，但材质纹理采样、normal mapping、alpha cutoff、
  roughness/metallic/AO/emissive 计算仍与 Forward shader 重复。
- Visibility Hi-Z 当前保存普通 Z 的 `max`，用于保守 occlusion culling；Screen
  Depth Pyramid 保存普通 Z 的 `min`，用于 GTAO、SSR、SSGI 和 depth debug。两者
  reduction 语义相反，不能把其中一条直接改名后复用。
- 两套 depth pyramid 都从同一个 `Surface Depth` 构建，拥有相同 extent、mip 数、
  descriptor layout 和 dispatch 结构。支持 `RG32F` sampled/storage 的设备可以在
  一次 dispatch 中同时保存 `(nearest, farthest)`；不支持时必须保留双 `R32F`
  回退，不能因此禁用已有算法。
- 当前 `Surface Normal Roughness.a` 只写 history-valid 浮点值，Stage 0 已定义的
  shading model / receives-screen-AO flags 尚未成为实际 shader ABI。Stage 2 将其
  正式切换为可在 FP16 中精确表示的整数 bit field。

#### 2.1 共享 Surface 与 Lighting GLSL

- 新增 `include/material_surface.glsl`，定义唯一的
  `EvaluatedMaterialSurface` 以及以下权威操作：
  - BaseColor、metallic/roughness、AO、emissive、transmission 和 alpha mode 求值。
  - UV0/UV1 occlusion 选择、alpha cutoff、front-face normal 修正。
  - 可选 tangent-space normal mapping；调用方显式选择 geometric 或 mapped normal。
- 新增 `include/pbr_brdf.glsl` 和 `include/pbr_direct_lighting.glsl`，迁移 GGX、
  Fresnel、geometry term、距离/spot attenuation 和 Scene Light 循环。
- 两个 PBR Forward fragment 变成薄 wrapper，共用一个 Forward 主实现；差异仅为
  是否提供 tangent input / normal mapping。
- `SurfacePrepass` 使用同一 `material_surface.glsl`，不再自行重复纹理和材质求值。
- 为 `builtin.unlit` 增加专用 Surface program：写入 Unlit shading-model flag、
  禁用 screen AO，并保持 MASK cutoff；Default Lit 写入 DefaultLit 与 receives AO。
- `surface_encoding.glsl` 增加完整 encode/decode helper。TAA 和后续 temporal consumer
  必须按 history bit 解码，不再使用 `packed.a > 0.5` 猜测。

#### 2.2 统一 Depth Hierarchy

- 新增 `DepthHierarchySupport`：
  - `CombinedMinMax`：`VK_FORMAT_R32G32_SFLOAT` 同时支持 sampled/storage，且设备启用
    `shaderStorageImageExtendedFormats`。
  - `SplitR32`：沿用当前两个 `VK_FORMAT_R32_SFLOAT` image。
  - unavailable：R32 sampled/storage 或 graphics-compute 条件不满足。
- `RendererResourceHandles` 增加 `depthHierarchyMinMax`。Combined 模式只注册这一条
  full-mip per-frame image；Split 模式只注册现有 nearest/farthest 两条 image，避免
  逻辑资源重复驻留。
- 用一个 `DepthHierarchyPass` 替换 `HiZBuildPass` 和 NearestDepth 版
  `ScreenSpacePyramidPass`：
  - Combined init 写 `(depth, depth)`，reduce 写 `(min nearest, max farthest)`。
  - Split 回退按 frame feature 只构建实际请求的 min 或 max chain。
  - 当 occlusion 与 screen-space effect 同时启用时，Combined 路径每 mip 仅 dispatch
    一次。
  - Scene Color Pyramid 保持独立，因为其 source、format 和 box-filter 语义不同。
- 新增 C++ `DepthHierarchyResources` 和 GLSL `depth_hierarchy.glsl` 访问器：
  - nearest consumer 总是读 R。
  - farthest consumer 在 Combined 模式读 G，在 Split 模式读 R。
  - Pass 只接收语义资源，不再直接依赖 `visibilityHiZ` / `screenDepthPyramid` 字段。
- `FrameRenderFeatures` 增加派生的 `depthHierarchyRequired`；RenderGraph topology key
  仍分别保留 occlusion 与 screen-space feature bit，以正确裁剪 consumer。

#### 2.3 Consumer 与诊断迁移

- Occlusion Cull 改读 farthest 语义；GTAO、SSR、SSGI 和 nearest-depth debug 改读
  nearest 语义。SSAO 继续直接读 full-resolution Surface Depth。
- 所有 depth hierarchy descriptor 和 mip count 通过统一 helper 获取；consumer 不
  判断物理 format，也不复制设备 capability 分支。
- `SurfaceDataStatus` 增加实际 flags ABI；`ScreenSpaceEffectsStatus` 增加 hierarchy
  mode、format、producer dispatch 数、consumer 列表、logical/resident bytes 和相对
  Split R32 的节省量。
- RenderGraph 节点统一命名为 `DepthHierarchy/Combined/MipN` 或
  `DepthHierarchy/Nearest|Farthest/MipN`，RenderDoc、Tracy 和 GPU profiler由节点名
  自动继承。
- `RG32F` 与两张 `R32F` 的理论字节数相同；Combined 路径的收益是每级只执行一次
  producer dispatch、一次链式同步和一套 descriptor，不把它计作显存节省。

#### 2.4 实施顺序

1. 先落地共享 Surface/BRDF include，并保持现有 attachment 和画面语义。
2. 将 surface flags 切换为正式 bit field并迁移 TAA history 解码。
3. 增加设备 capability、资源选择和统一 `DepthHierarchyPass`。
4. 逐个迁移 Occlusion、GTAO、SSR、SSGI、ToneMap debug，最后删除旧 pyramid builder。
5. 增加诊断并执行 dev-fast/runtime 构建和启动冒烟。

工作：

- 提取 `SurfaceData`和共享 material evaluation GLSL。
- 提取 BRDF、direct、indirect和 atmosphere helper。
- 更新 SurfacePrepass消费者，通过 `surface_encoding.glsl`读取 normal、roughness和 history flags。
- 实现 RG32F min/max Depth Hierarchy；保留双 R32 fallback。
- Occlusion、GTAO、SSR和 SSGI迁移到统一 depth hierarchy访问器。
- 对 Depth Hierarchy增加 producer/consumer和内存诊断。

完成条件：

- Forward和所有现有 screen-space算法继续运行。
- 同一帧不再无条件构建两套等价深度 mip链；fallback设备除外。
- 材质求值代码只有一个权威实现。

完成记录：

- `material_surface.glsl`、`pbr_brdf.glsl`、`pbr_direct_lighting.glsl` 和
  `forward_common.glsl` 已成为 Default Lit / Unlit Surface 与两个 PBR Forward
  program 的共享实现；Surface flags 已切换为正式 bit field，TAA 按 history-valid
  bit 解码。
- 当前设备选择 `CombinedMinMax / VK_FORMAT_R32G32_SFLOAT`。Algorithm Playground
  同时启用 Occlusion、GTAO、SSR 和 SSGI 时，RenderGraph 只生成 10 个
  `DepthHierarchy/Combined/MipN` dispatch；68 个 active pass 下 GPU 总时间约
  `2.85 ms`，Validation error 为 0。
- 将上述效果全部关闭并等待资源 retirement 后重新开启，active pass 从 68 降至
  27 再恢复到 68；未出现空 descriptor、VUID 或 residency refresh 错误。
- `windows-msvc-dev-fast` 与 `windows-msvc-runtime` 均构建成功；Release runtime
  启动 6 秒无 error 日志。按照项目策略未运行测试套件。

### Stage 3：Deferred GBuffer Path

目标：生成完整 GBuffer，但暂不替代 Forward最终画面。

> Status: Completed
> Started: 2026-08-18
> Completed: 2026-08-18

#### 现状审计与边界

- 当前 `SurfacePrepass` 是 screen-space/temporal 算法的紧凑辅助数据，不是完整
  GBuffer：Normal attachment 的 alpha 保存 surface flags，而正式 GBuffer 的对应通道
  必须保存 material occlusion；emissive 也尚未写入。不能直接重命名或原地改变编码，
  否则 TAA、GTAO、SSR 和 SSGI 会在同一阶段被迫迁移。
- Surface 与 GBuffer 都需要当前/上一帧矩阵、render-item history、材质 family、MASK
  cutoff、double-sided cull 和 normal mapping。上述 draw preparation 必须共享，不能复制
  两套 per-frame mapped buffer 与 descriptor 生命周期。
- Stage 3 仍由 Forward 产生最终 HDR。GBuffer 只在 GBuffer debug view 请求时 dual-run，
  使用 RenderGraph lazy residency；普通 Forward 帧不得常驻约 28 B/pixel 的额外资源。
- Deferred path 的正式启用、lighting、透明混合和运行时 path switch 仍分别属于
  Stage 4/5。本阶段不把半成品 Deferred 暴露为可选择的最终 render path。

#### 3.1 Surface Frame Data 共享

- 从 `SurfacePrepass` 提取 `SurfaceFrameData`，统一持有：
  - 每 frame slot 的 `SurfaceFrameUbo`。
  - `GpuRenderItemHistory[]` mapped SSBO、capacity growth 和 descriptor set。
  - descriptor layout、prepare generation 和内存诊断。
- `SurfacePrepass` 与 `GBufferPass` 引用同一个对象；同一 frame slot/generation 只准备
  一次 CPU 数据。Renderer 保证它晚于两个 Pass 销毁。
- 提取共享 `SurfaceDrawRecorder` 或等价 pipeline/draw helper，集中处理 Material Family
  program 解析、Bindless/Legacy set、indirect draw、push block 和 cull mode。两个 Pass
  只提供 pass kind、attachment signature 和 depth state。

#### 3.2 GBuffer Capability 与资源

- 新增 `GBufferSupport`，启动时验证以下格式均支持 color attachment + sampled image，
  depth 支持 attachment + sampled，且 `maxColorAttachments >= 4`：
  - BaseColorMetallic：`R8G8B8A8_UNORM`。
  - NormalRoughnessOcclusion：`R16G16B16A16_SFLOAT`。
  - EmissiveSurfaceFlags：`R16G16B16A16_SFLOAT`。
  - Motion：`R16G16_SFLOAT`。
- Registry 新增五张 per-frame、viewport-relative、单采样、lazy-resident image：四张
  color attachment 和一张 sampled depth。GBuffer 资源不得与 SurfacePrepass image
  物理复用，Stage 3 dual-run 时两套编码必须同时有效。
- `GBufferContractStatus::implemented` 表示 capability、资源和 shader contract 均存在；
  `RenderPathCapabilities.gBuffer` 同步更新，但 `deferred` 保持 false。

#### 3.3 Material Family 与 GBuffer Pass

- `MaterialShaderPass` 增加 `GBufferOpaque/GBufferMask`，Shader Manifest family 必须显式
  映射；新增 `GBuffer` program contract，禁止把 Forward/Surface program误用于 GBuffer。
- 新增共享 `gbuffer_common.glsl` 和 DefaultLit/Unlit、Opaque/MASK 薄 wrapper：
  - GBuffer0 = linear baseColor.rgb + metallic。
  - GBuffer1 = oct world normal + roughness + material occlusion。
  - GBuffer2 = emissive.rgb + exactly representable surface flags。
  - GBuffer3 = 与 SurfacePrepass 完全相同的 unjittered-history motion 约定。
- GBuffer 使用 `material_surface.glsl` 的唯一材质求值；DefaultLit启用 normal map和 screen
  AO flag，Unlit使用 geometric normal、Unlit flag并禁用 screen AO。
- `GBufferPass` 只处理 camera opaque/MASK；BLEND/transmission不写 GBuffer。使用
  Dynamic Rendering、4 个 color attachment、独立 depth和同一 visibility ordering。

#### 3.4 Dual-run Debug 与诊断

- 新增独立 `GBufferDebugView`：BaseColor、Normal、Metallic、Roughness、Occlusion、
  Emissive、Motion、SurfaceFlags。它与 Surface/Screen-space debug互斥。
- 仅非 `None` debug view设置 `gBufferRequired`。RenderGraph 顺序为 Surface/Visibility
  工作之后、Sky/Forward之前生成 GBuffer；Forward仍完整执行，ToneMap最后选择 GBuffer
  attachment显示。
- ToneMap固定绑定全部 GBuffer debug sources；未驻留时绑定合法 fallback。切换 debug、
  viewport resize和资源 retirement后更新 descriptor，不允许写入空 set/image view。
- Diagnostics/Runtime JSON 输出 support、active、formats、extent、resident bytes、draw数和
  attachment语义；RenderDoc/Tracy/GPU profiler节点统一为 `Deferred/GBuffer`。

#### 3.5 实施顺序

1. 提取共享 `SurfaceFrameData`，保持现有 SurfacePrepass画面与运行结果不变。
2. 增加 capability、五张逻辑资源与 GBuffer ABI访问器。
3. 扩展 Material Family contract并实现四个 GBuffer fragment wrapper。
4. 实现 `GBufferPass`，接入 RenderGraph lazy residency与 shared draw path。
5. 接入 debug view、ToneMap、UI/Runtime diagnostics。
6. 构建 dev-fast/runtime，运行 GBuffer各通道、MASK、Unlit与动态开关冒烟。

工作：

- 实现 `DeferredRenderPath`资源声明和 `GBufferPass`。
- GBuffer支持 Opaque、MASK、double-sided和 normal mapping。
- 写入 Depth、GBuffer0/1/2和 Motion。
- 增加 GBuffer debug View Modes。
- 增加 dual-run开发诊断：可在 Forward显示时后台仅生成 GBuffer，用于检查，不执行 Deferred Lighting。
- 对 attachment format、extent、sample count和 shader output contract做 Graph校验。

完成条件：

- BaseColor、Normal、Metallic、Roughness、AO、Emissive、Motion和 flags调试输出正确。
- MASK轮廓与 Forward一致。
- TAA motion与现有 Forward SurfacePrepass一致，不重复 jitter补偿。

完成记录（2026-08-18）：

- `SurfaceFrameData` 统一管理 Surface/GBuffer 的 frame UBO、render-item history、
  descriptor 和 generation preparation；`SurfaceDrawRecorder` 统一 Material Family、
  Bindless/Legacy、MASK、double-sided、indirect draw 和 push ABI。
- Device capability、五张 lazy-resident GBuffer image、Material Family GBuffer pass、
  DefaultLit/Unlit Opaque/MASK shader、`Deferred/GBuffer` RenderGraph 节点和
  Dynamic Rendering attachment contract 已接入。
- ToneMap、Editor、Runtime Control 与 `VulkanLabCtl --gbuffer-debug` 支持 BaseColor、
  Normal、Metallic、Roughness、Occlusion、Emissive、Motion 和 SurfaceFlags；三类
  Surface/GBuffer/Screen-space debug view 使用统一互斥规则。
- `render.status.renderPath.gBuffer` 输出 capability、active、formats、extent、draws 和
  resident bytes；GPU profiler 节点稳定命名为 `Deferred/GBuffer`。
- `windows-msvc-dev-fast` 和完整 Debug VulkanLab/VulkanLabCtl 构建通过。实际运行
  Algorithm Playground 时 GBuffer 记录 140 个 draw，八个通道均可切换，Normal 与
  BaseColor capture有效，日志无 Validation error/VUID。按照项目策略未运行测试套件。

### Stage 4：Deferred Lighting Parity

目标：Deferred路径能够输出与 Forward语义一致的 opaque products。

> Status: Completed
> Started: 2026-08-18
> Completed: 2026-08-18

#### 现状审计与边界

- Forward 的权威光照语义目前集中在 `pbr_lite/forward_common.glsl`：Direct Lighting、
  ambient/IBL、Reflection Probe、material/screen AO、DDGI、Emissive 和 Aerial
  Perspective。直接复制为 compute shader会产生两套很快分叉的实现，必须先提取共享的
  surface-lighting function。
- Global、Lighting、Atmosphere、ScreenSpace 与 DDGI descriptor已经包含 Deferred所需
  数据，但 Scene Light SSBO、Lighting、ScreenSpace 和 DDGI sampling layout目前只允许
  Fragment stage。Stage 4只扩展 stage visibility，不改变 binding、set编号或资源内容。
- MainForward Opaque当前负责把 SkyBackground的 MSAA target resolve到单采样 HDR。
  Stage 4仍执行 Forward，并让 Deferred背景像素读取已 resolve的 HDR；几何像素完全由
  GBuffer重建和着色。Stage 5正式切换路径时再让 Sky直接提供 Deferred背景产品。
- Stage 4不开放 `RenderPathMode::Deferred`。新增独立 Deferred Lighting Debug View来
  请求 dual-run并比较 Final/Diffuse/Specular/Difference，确保未完成透明策略前用户无法
  误选半成品路径。

#### 4.1 共享 Surface Lighting

- 新增不依赖材质纹理 descriptor的 `pbr_surface_lighting.glsl`：
  - `PbrSurfaceLightingInput`保存 position、normal、baseColor、roughness、metallic、
    material occlusion、emissive和 receives-screen-AO。
  - `PbrSurfaceLightingResult`保存 opaque HDR、baseline diffuse和 baseline specular。
  - 唯一函数执行 Directional/Point/Spot direct lighting、CSM/Punctual Shadow、
    ambient/IBL/Reflection Probe、DDGI、AO作用域和 Atmosphere sunlight/aerial。
- Forward wrapper从 `EvaluatedMaterialSurface`构造输入；Transmission/BLEND继续在
  Forward wrapper外执行现有近似。Deferred只处理 Opaque/MASK，因此不需要编码
  transmission参数。
- Unlit按现有 Forward语义输出 baseColor，不接收灯光、AO或 atmosphere；baseline输出
  为零。GBuffer中的 shading-model flags是唯一分支来源。
- `screen_space_lighting.glsl`增加显式 UV采样入口，Fragment的 `gl_FragCoord` helper和
  Deferred compute共用同一 active-mode逻辑。

#### 4.2 Deferred Lighting资源与 Pass

- 新增三张 per-frame、viewport-relative、单采样、lazy-resident `RGBA16F`：
  - `Deferred HDR Color`。
  - `Deferred Baseline Diffuse`，alpha继续保存 material occlusion。
  - `Deferred Baseline Specular`。
- 新增 `DeferredLightingPass`和 `deferred.lighting` compute program，8×8 dispatch：
  - set 0：Global UBO + Scene Light SSBO。
  - set 1：GBuffer0/1/2、Depth、Forward HDR background和三个 storage outputs。
  - set 2：Shadow/IBL/Reflection Probe Lighting descriptor。
  - set 3：Atmosphere。
  - set 4：Screen-space AO。
  - set 5：DDGI sampling。
- 从 depth和 `inverseViewProjection`重建 world position，解码 oct normal、surface flags和
  GBuffer material参数。Depth背景像素复制 Forward HDR background，baseline清零。
- DefaultLit调用共享 surface-lighting function；Unlit按 flags直接输出 baseColor。
- Graph自动声明 GBuffer sampled read、shadow/environment/AO/DDGI依赖和三个 storage
  write；Pass内部不录制 barrier。

#### 4.3 Dual-run Parity Debug

- 新增 `DeferredLightingDebugView`：None、Final Color、Baseline Diffuse、Baseline
  Specular、Forward Difference。它与 Surface、GBuffer和 Screen-space debug互斥。
- 非 None时同时设置 `gBufferRequired`和 `deferredLightingRequired`；普通 Forward帧不创建
  或执行 Deferred Lighting资源。
- ToneMap增加三个 Deferred输入：
  - Final/Diffuse/Specular使用现有 exposure/tone mapper。
  - Difference显示 `abs(forwardOpaque - deferredOpaque)`的固定增强结果，不叠加 Bloom。
- Stage 4的 SSR/SSGI仍消费 Forward标准产品；Deferred baseline仅用于 parity检查。Stage 5
  再由 opaque path assembly 返回标准产品，避免 Composite Pass出现路径判断。

#### 4.4 Contract 与诊断

- 新增 `DeferredLighting` Shader contract，校验六个 descriptor set、GBuffer采样格式、
  storage output、Global/Scene Light/Atmosphere/DDGI ABI和 8×8 local size。
- `DeferredLightingStatus`输出 support、active、extent、dispatch、resident bytes和当前
  debug mode；GPU profiler/RenderDoc/Tracy节点命名为 `Deferred/Lighting`。
- Runtime Control、CLI和 Editor Advanced接入 parity debug；`render.status.renderPath`
  同时报告 GBuffer和 Deferred Lighting状态，但 `capabilities.deferred`继续保持 false。

#### 4.5 实施顺序

1. 提取共享 DefaultLit opaque lighting函数，并让 Forward薄 wrapper消费它。
2. 扩展既有 frame descriptor的 Compute stage visibility和 Shader contract。
3. 注册三张 Deferred输出，新增 feature condition和 compute program。
4. 实现 `DeferredLightingPass`、GBuffer解码、world-position重建和 Unlit分支。
5. 接入 ToneMap parity debug、UI/Runtime/CLI和诊断。
6. 构建 dev-fast/runtime，实际比较 Algorithm Playground与 Main Sponza。

工作：

- 实现 `DeferredLightingPass` compute shader。
- 重建 world position，执行 DefaultLit/Unlit分支。
- 接入方向光、Point/Spot SSBO、CSM、Point/Spot shadows。
- 接入 ambient、IBL、material AO、screen-space AO、DDGI和 atmosphere sunlight/aerial perspective。
- 输出 HDR、Baseline Diffuse和 Baseline Specular。
- SSR/SSGI Composite消费标准产品，不感知路径。
- 背景 depth执行 early-out，保留 SkyBackground HDR。

完成条件：

- Algorithm Playground和 Sponza在主要材质上的 Forward/Deferred视觉结果可解释地接近。
- Direct、Emissive、AO和 indirect作用域与 Forward一致。
- Debug/Legacy不因 Deferred路径出现黑屏或错误 fallback。

完成记录（2026-08-18）：

- `pbr_surface_lighting.glsl` 已成为 Forward 与 Deferred 共用的 opaque PBR
  光照实现；Direct、Shadow、IBL/ambient、Reflection Probe、AO、DDGI、Emissive
  与 Aerial Perspective 不再维护两份独立公式。
- `DeferredLightingPass` 使用 8×8 compute 从 GBuffer重建 world position，并输出
  Deferred HDR、Baseline Diffuse 和 Baseline Specular。三张输出保持 lazy residency，
  普通 Forward 帧不执行 GBuffer 或 Deferred Lighting。
- Editor、Viewport Debug、Runtime Control 与 `VulkanLabCtl` 已接入 Final、Diffuse、
  Specular 和 Forward Difference；四类 Surface/GBuffer/Deferred/Screen-space debug
  view 使用统一互斥规则。
- `windows-msvc-dev-fast`、完整 Debug 的 VulkanLab/VulkanLabCtl 和
  `windows-msvc-runtime` 均构建通过；精简 Runtime 实际持续运行 6 秒。
- 使用 Core Validation 在 Sheen Chair、Algorithm Playground 和 Main Sponza 切换全部
  Deferred parity 视图并完成 800×600 capture。RenderGraph 拓扑包含
  `Deferred/GBuffer -> MainForwardOpaque -> DeferredLighting/Compute -> ToneMap`，日志无
  Validation error。Difference 主要位于几何边缘和 GBuffer量化/位置重建误差区域，
  Sponza大部分表面接近零差异。
- 按项目策略未运行测试套件。额外编译 CPU 测试目标时，Stage 4 修改到的 Shader
  Registry、Shader Contract 和 Runtime Dispatcher源已通过编译，但完整测试目标仍被
  既有的纹理 manifest、旧 PipelineKey 与旧 SceneLight include测试代码阻断。

### Stage 5：Hybrid Transparent 与运行时切换

> Status: Completed

目标：完成可实际使用的混合管线。

#### 5.1 路径选择与兼容策略

- `RenderSettings` 保存 `RenderPathRequest`，`FrameRenderFeatures` 保存已经解析的
  `RenderPathSelection`；Pass 只能读取 `active`，不能自行解释 `Auto`。
- 选择顺序固定为：设备 capability、活动 View Mode、场景材质 family contract、用户请求。
- `Lit` 且全部 Opaque/MASK 材质具有 GBuffer program时允许 Deferred；Legacy、材质通道
  Debug 或其他覆盖材质 program的 View Mode固定走 Forward。
- BLEND、Transmission 和 Forward-only family不阻止 Deferred opaque path，它们进入
  Forward Transparent/fallback queue；不透明且缺失 GBuffer program的材质必须计数并给出
  明确 fallback reason。
- `Auto` v1默认选择 Forward，只有策略显式开启后才自动选择 Deferred；这样不会在升级后
  无提示改变现有项目的性能和画面。
- `RenderPathMode` 纳入 topology key；产品 handle本身不进入 key，因为它们随 Renderer
  生命周期稳定，活动 mode已经足以区分拓扑。

#### 5.2 标准产品解析

- 新增集中式 `resolveOpaqueRenderProducts(selection, resources)`，由 Graph setup、descriptor
  更新和 record共同使用。
- Forward产品：
  - `hdrColor = Forward HDR Resolve`。
  - `geometryDepth = Main Forward Depth`。
  - baseline使用 Forward MRT。
  - `screenSpace`使用共享 SurfacePrepass产品。
- Deferred产品：
  - `hdrColor = Deferred Lighting HDR`。
  - `geometryDepth = GBuffer Depth`。
  - baseline使用 Deferred Lighting输出。
  - `screenSpace`在 v1仍使用共享 SurfacePrepass产品。
- 新增 `resolvePostLightingHdr()`：有 SSR/SSGI/DDGI composite时返回 composite target，
  否则返回活动 opaque HDR。Transparent、TAA、Bloom、ToneMap和 Capture使用该函数或其
  后续产品，禁止继续硬编码 Forward HDR handle。
- 逐项迁移 `ScreenSpacePyramidPass`、SSR、SSGI、`HdrCompositePass`、Transparent、TAA、
  Bloom、ToneMap与无 GUI Present的输入，防止切到 Deferred后仍读取旧 Forward资源。

#### 5.3 Sky 与 Deferred opaque

- Forward活动时维持现有 Sky写入 MSAA HDR、Forward opaque resolve到单采样 HDR的路径。
- Deferred活动时 Sky直接写单采样背景 HDR；GBuffer不写颜色背景，Deferred Lighting对
  background depth像素复制 Sky HDR，对几何像素输出计算后的 Deferred HDR。
- Stage 4 parity debug仍允许 Forward和 Deferred Lighting dual-run；此模式不是活动 Deferred
  path，不改变主后处理输入。
- Deferred活动时关闭 MainForwardOpaque节点，GBuffer和 DeferredLighting成为唯一 opaque
  producer；不得只靠 Pass内部 early return保留空节点。

#### 5.4 Hybrid Transparent

- `MainForwardTransparent`读取活动路径的最终 opaque/composite HDR并执行 load + blend。
- Forward使用 Main Forward Depth；Deferred使用 GBuffer Depth，均以 read-only depth
  attachment参与透明深度测试。
- BLEND/Transmission始终按 material family解析 `ForwardTransparent` program，继续使用
  back-to-front排序、Atmosphere、Shadow、IBL和 Scene Light数据。
- 透明材质继续禁用 screen-space AO；SSR/SSGI v1只处理 opaque结果，不把透明表面写入
  GBuffer或 Scene Color Pyramid。
- Deferred路径固定单采样；透明绘制也使用 1x pipeline signature。Forward仍保留现有
  MSAA策略。

#### 5.5 切换、history 与资源生命周期

- 路径变化时在下一帧边界重建 Graph topology，不重建设备、Swapchain、MaterialSystem或
  PipelineCache，也不调用 `vkDeviceWaitIdle()`。
- 通过现有 frame serial/fence确保当前 slot descriptor只在安全时更新；已经创建的 Forward
  和 Deferred物理资源保留 resident，来回切换不反复分配大图像。
- 路径变化必须统一触发 temporal invalidation：TAA、SSR、SSGI、GTAO temporal和依赖
  opaque shading结果的 history失效。Shadow与 Atmosphere LUT不失效。
- invalidation reason固定为 `render_path_changed`，只触发一次，不得因 UI每帧重复写入设置而
  连续清空 history。
- 场景切换、Viewport resize与 shader family reload继续使用现有各自的 invalidation reason。

#### 5.6 UI、命令行与诊断

- Render面板提供 Requested/Active Path、fallback原因和 Forward-only View Mode提示。
- 启动参数接入：
  ```text
  --render-path auto|forward|deferred
  ```
- Runtime Control增加 `render_path.get/set`；设置成功返回 requested、active、topology
  generation和 history reset状态。
- Diagnostics显示 opaque/transparent/fallback draw数、活动产品、GBuffer resident/active
  bytes、Forward/Deferred GPU时间和 fallback材质列表。

#### 5.7 实施顺序

1. 引入 request/selection与纯 resolver，保持 Forward为活动路径。
2. 引入标准产品解析，并迁移所有下游 HDR/depth消费者。
3. 让 Sky、GBuffer和 Deferred Lighting形成可独立执行的 Deferred opaque拓扑。
4. 接入 Forward Transparent的 Deferred color/depth输入。
5. 接入 topology切换、descriptor更新和 temporal invalidation。
6. 接入 Editor、启动参数、Runtime Control与诊断。
7. 删除 Stage 4仅用于 parity的硬编码输入，但保留 Deferred parity debug view。

完成条件：

- 场景切换、Viewport resize和 Render Path切换不调用 device idle。
- 透明、玻璃、MASK和阴影在两条路径下行为正确。
- 来回切换路径不造成 descriptor、history或 resident资源生命周期错误。
- Deferred活动时 Graph中不存在 `MainForwardOpaque`，Forward活动且未请求 parity debug时不存在
  `DeferredLighting`。
- 下游 Pass不直接读取 Forward/Deferred专有 handle，只通过标准产品解析访问。
- 强制 Deferred不支持时明确失败，Auto回退时明确报告原因。

完成记录（2026-08-19）：

- 新增独立的 `RenderPathRequest`、`RenderPathMode`、`RenderPathSelection` 和集中式
  `OpaqueRenderProducts`。`Auto` 保持 Forward，强制 Deferred 会验证设备 capability 与
  View Mode contract；活动路径进入 RenderGraph topology key。
- Sky、Forward opaque、GBuffer、Deferred Lighting、Transparent、Scene Color Pyramid、
  HDR Composite、TAA、Bloom、ToneMap、HDR Capture 和无 GUI Present 已统一消费活动路径
  产品。Deferred 活动时 Graph 不包含 `MainForwardOpaque`。
- Hybrid Transparent 使用 Deferred HDR Color 的 color-attachment read/write 与 GBuffer
  read-only depth；Deferred HDR 资源契约补充 `COLOR_ATTACHMENT` usage。Main Sponza 验证
  401 个 opaque draw 进入 GBuffer、4 个 transparent draw 在 Deferred Lighting 后由
  `MainForwardTransparent` 绘制。
- 路径切换通过 topology 重编译完成，不清空 Pipeline Cache、不调用 device idle；路径身份
  进入已有 temporal history identity。TAA 开启时 Forward → Deferred → Forward 往返后
  history generation 正确递增并恢复积累。
- Editor Render 面板、启动参数 `--render-path`、Runtime Control `render_path.get/set`、
  `VulkanLabCtl render-path get/set` 和 `render-settings set --render-path` 已接入。协议返回
  requested/active、capability、fallback、topology hash 与 TAA/GTAO/SSR/SSGI history 状态。
- `windows-msvc-dev-fast`、完整 Debug VulkanLab/VulkanLabCtl 和
  `windows-msvc-runtime` 均构建通过；精简 Runtime 以 Deferred 持续运行 6 秒。Core
  Validation 下完成 Sheen Chair 路径往返和 Main Sponza Hybrid Transparent 验证，日志无
  Validation error。按项目策略未运行测试套件。

### Stage 6：Tiled / Clustered Light Culling

> Status: Completed 2026-08-20

目标：让 Deferred在多灯场景中获得明确收益，并让 Forward Transparent共享同一 light list。

#### 6.1 Grid 与正确性边界

- 使用 screen XY tile + logarithmic view-Z 的 3D grid。Z slice固定为 24；基础 tile为
  `16×16`，当高分辨率导致每个 frame slot的 index buffer超过 32 MiB时，tile按 2 的幂
  增长到 32/64，避免 4K/8K viewport产生不可控显存增长。
- Cluster range由相机 near/far、projection和 tile边界直接构造，不依赖 opaque depth占用。
  这是对原简略计划中“从 Depth Hierarchy生成 cluster范围”的修正：若按 opaque min/max
  删除空 cluster，天空前的透明物体和 opaque后方透明物体会缺失 Point/Spot Light。
- 现有 combined min/max Depth Hierarchy继续只负责 Hi-Z visibility、GTAO、SSR和SSGI；
  clustered lighting不再制造第二种 depth hierarchy语义。
- Directional Light继续使用全局连续段；Point和 Spot写入 cluster list。Spot v1用 range
  sphere做保守 broad phase，shader仍执行精确 cone attenuation，保证只增加候选、不漏光。

#### 6.2 GPU ABI 与资源所有权

- 新增 `ClusteredLightingResources`，由 Renderer持有，管理每个 frame slot的：
  - `uint clusterCounts[]`。
  - `uint clusterLightIndices[]`，保存 Scene Light SSBO绝对 index。
  - host-visible `GpuClusterStatistics`。
  - build descriptor和 shading descriptor。
- 每个 cluster最多保存 32个 Point/Spot index。超出时在 count中设置 overflow bit；consumer
  对该 cluster回退为遍历全局 punctual段，因此容量压力只影响性能，不影响光照正确性。
- `GlobalFrameUbo`增加 cluster grid、tile、near/far和 logarithmic slice参数；Scene Light ABI、
  `GpuLight` stride和 Shadow slot约定不变。
- shading descriptor固定为 `set=6`，只加入两个 PBR Forward program与 Deferred Lighting。
  Shadow、Surface、GBuffer、Legacy、Unlit和 Debug program不声明 cluster资源。
- build compute使用独立 `set=1` descriptor，避免为了写 cluster buffer给 compute pipeline插入
  set 1–5占位 layout。两套 descriptor引用相同物理 buffer，但 stage/access contract独立。
- viewport resize后重建所有 frame slot的 cluster buffers；正常帧不扩容、不替换 descriptor，
  不调用 device idle。资源大小进入 diagnostics，但不纳入 RenderResourcePool image residency。

#### 6.3 Cluster Build Pass

- 新增 `ClusteredLightCullingPass`，Graph节点固定为：
  ```text
  ClusteredLighting/ClearStats
  ClusteredLighting/Build
  ```
- Clear使用 transfer fill；Build每个 invocation处理一个 cluster，遍历当前 Point/Spot段并做
  view-space sphere/AABB保守测试，输出 count/index并原子累计统计。
- RenderGraph声明 Scene Light SSBO read、cluster count/index write与 stats transfer/write，自动
  生成 transfer→compute及 compute→fragment/compute barrier。
- Pass在当前 frame fence已经完成后读取上一轮 stats，不增加 wait；GPU profiler、Tracy和
  RenderDoc统一显示 `ClusteredLighting`。
- 少于 8盏 punctual light时不执行 build，consumer使用现有全局循环，避免小场景支付固定
  compute和buffer读取成本。阈值是内部策略，不增加 SceneDocument字段。

#### 6.4 Shading Consumers

- `scene_lights.glsl`增加统一 cluster lookup helper；`pbr_direct_lighting.glsl`保留 Directional
  全局循环，并把 Point/Spot改为：active cluster list → overflow/global fallback → 普通全局
  fallback。
- Deferred compute按 pixel和重建的 world position查询 cluster；Forward Opaque与 Forward
  Transparent按 `gl_FragCoord`/world position查询同一表。
- Forward Opaque也使用 cluster，保证 Forward/Deferred性能对比不会混入不同灯光算法；
  Legacy仍保持原 baseline光照。
- Point/Spot Shadow继续从 `GpuLight.params.z/w`读取 slot/far，cluster list只改变候选索引，
  不改变阴影选择或采样。

#### 6.5 状态、诊断与性能门

- `FrameRenderFeatures`增加 `clusteredLightingRequired/Active`和 punctual count；topology key只
  在阈值跨越或 capability变化时切换。
- Diagnostics与 `render.status.lighting.clustered`显示 supported/active、tile/grid尺寸、cluster
  数、index容量、active/non-empty、平均/最大 references、overflow cluster/reference数量、
  resident bytes和 GPU时间。
- overflow cluster必须回退全局循环并明确计数；禁止静默截断。
- 记录 Algorithm Playground、Main Sponza和 32/64/128/256灯场景的 Forward/Deferred
  `ClusteredLighting`、opaque lighting和总 GPU时间。Stage 7依据结果决定 Auto策略。

完成条件：

- 32–256灯场景中不存在全屏逐灯循环。
- overflow安全且可诊断。
- 少量灯和大量灯分别有 Forward/Deferred性能数据，不用主观帧率判断路径价值。

完成记录（2026-08-20）：

- 新增每 frame slot 的 Cluster Count、Light Index 和 host-visible Statistics buffer；使用
  `16×16×24` logarithmic grid，并在高分辨率下按 32 MiB/slot 预算自动扩大 tile。
- RenderGraph 已生成 `ClusteredLighting/ClearStats -> ClusteredLighting/Build`，自动处理
  Scene Light read、transfer clear、compute write 到 Forward fragment/Deferred compute read
  的同步。少于 8 盏 punctual light 时节点被裁剪。
- PBR Forward Opaque/Transparent 与 Deferred Lighting 共享 set 6 cluster list；Directional
  继续全局遍历，Point/Spot 读取绝对 Scene Light index。overflow cluster 使用完整 punctual
  loop 回退，不会静默漏光。
- Shader Manifest 使用显式 `clusteredLighting` capability；SPIR-V contract 覆盖 Forward、
  Deferred Lighting 与 Cluster Build 的 descriptor set/binding/type，Global UBO 保持单一 ABI。
- Diagnostics、Runtime Control 和 Tracy plots 已覆盖 grid、occupancy、平均/最大引用、overflow
  cluster/reference、resident bytes 和 GPU pass timing。
- RTX 4060 Laptop GPU、1280×720、无 GUI、Validation Off，在 Algorithm Playground 派生的
  临时场景中等待 90 个稳定帧后取 5 帧平均，结果如下（单位 ms）：

  | Punctual | Path | Cluster | Opaque/Lighting | Total | Overflow |
  |---:|---|---:|---:|---:|---:|
  | 6 | Forward | 0.0000 | 0.4987 | 1.1354 | 0 |
  | 6 | Deferred | 0.0000 | 0.1808 | 0.6643 | 0 |
  | 32 | Forward | 0.0505 | 0.5247 | 1.0296 | 0 |
  | 32 | Deferred | 0.0512 | 0.1724 | 0.5429 | 0 |
  | 64 | Forward | 0.0826 | 0.6611 | 1.2028 | 0 |
  | 64 | Deferred | 0.0838 | 0.2111 | 0.6179 | 0 |
  | 128 | Forward | 0.1564 | 0.7737 | 1.3930 | 0 |
  | 128 | Deferred | 0.1585 | 0.2478 | 0.7327 | 0 |
  | 256 | Forward | 0.2873 | 0.8790 | 1.6289 | 12 clusters / 19 refs |
  | 256 | Deferred | 0.2804 | 0.2597 | 0.8644 | 12 clusters / 19 refs |

- 256 灯场景平均每个非空 cluster 为 5.39 refs、最大 37 refs；仅 12 个 cluster 进入正确性
  回退。临时测试工程与指标文件位于忽略的 `build/` 目录，未修改项目 Scene/Catalog。
- `windows-msvc-dev-fast` 与 `windows-msvc-runtime` 编译完成；Forward/Deferred 活动路径均
  实际执行 cluster build，当前运行日志无本次新增 Vulkan error。按项目策略未运行测试套件。

### Stage 7：收口、兼容与文档（Completed 2026-08-21）

> Status: Completed

目标：删除 Stage 0–6 留下的迁移层，固定 Render Path、View Mode 和 Material Shader
Family 的最终职责，并让开发构建、Cooked Runtime、诊断导出和文档使用同一套术语。
本阶段不增加新画面功能，也不改变 GBuffer、Cluster、Descriptor 或 SceneDocument ABI。

#### 7.1 默认路径策略

根据 Stage 6 的同机、同分辨率测量，Deferred 在 32/64/128/256 盏灯时的总 GPU 时间均
低于 Forward；6 盏灯场景中 Deferred 也未表现出不可接受的固定成本。因此固定以下策略：

```text
Requested Auto
  ├─ Deferred capability 可用且 View Mode 兼容 -> Deferred
  └─ 否则                                  -> Forward + fallback reason

Requested Forward
  └─ 始终 Forward

Requested Deferred
  ├─ capability 和 View Mode 均兼容 -> Deferred
  └─ 否则明确失败或拒绝设置，不静默改变用户请求
```

- `Auto` 是策略，不是可执行路径；Pass 和 Shader 只能看到最终 `RenderPathMode`。
- Forward 继续承担 MSAA、Legacy、Forward-only View Mode、透明和 Transmission。
- Deferred 是兼容设备上 `Lit` 模式的默认 opaque path；透明仍由共享 Forward Transparent
  处理。
- 路径切换继续只使 Graph topology 和 temporal history失效，不重建 Device、Swapchain、
  MaterialSystem 或 PipelineCache。
- resolver 保持无 Vulkan 副作用，并更新对应测试源码；按项目策略只编译，不默认运行测试。

#### 7.2 删除全局 ShaderVariant 迁移层

当前 `ShaderVariant.h` 已不再定义真正的全局 variant，但文件名和以下兼容字段仍传播旧概念：

```text
RenderPathStatus::viewModeMigrated
RenderPathStatus::compatibilityViewMode
Renderer::compatibilityViewModeId_
render.status.renderPath.compatibilityShaderVariant
```

收口方式：

- 将 `ShaderVariant.h` 重命名为 `ShaderTypes.h`，保留其中稳定的 `ShaderProgram`、
  `MaterialShaderFamily`、`MaterialShaderPass` 和 `ViewMode` 类型。
- 更新源码、测试和文档 include；禁止再新增 `ShaderVariant` 命名。
- `RenderPathStatus` 直接保存 `viewMode`，删除 migration flag 和 compatibility alias。
- Runtime Control 只输出 `viewMode`；删除 `compatibilityShaderVariant`。协议版本不变，因为
  该字段属于开发期迁移字段，尚未形成稳定 Cooked Runtime 契约。
- Diagnostics、Editor 和日志统一显示 `Render Path`、`View Mode`、`Material Family`，不再把
  三者合并为 Shader selector。
- 全仓库检查任何 Pass 不得从 Application 全局 shader selection决定材质程序；程序解析必须
  经过 `(MaterialShaderFamily, MaterialShaderPass, binding backend)`。

#### 7.3 Feature 目录和所有权收口

`core_forward` 已不再准确描述其中所有模块。按职责移动，不改变类名和行为：

```text
src/render/features/forward/
  MainForwardPass.*

src/render/features/post_process/
  HdrCompositePass.*
  ToneMapPass.*
  PresentPass.*

src/render/features/surface/
  SurfacePrepass.*
  GBufferPass.*
  DepthHierarchyPass.*
  SurfaceDrawRecorder.*

src/render/features/deferred/
  DeferredLightingPass.*

src/render/features/lighting/
  ClusteredLighting.*
  ClusteredLightCullingPass.*
```

- 使用文件移动保留历史，更新 `src/CMakeLists.txt` 和所有 include。
- Forward 目录只包含真正属于 Forward path 的 opaque/transparent draw pass。
- ToneMap、Present 和 HDR Composite 属于共享后处理，不得依赖活动 opaque path 的具体资源
  handle；它们只消费 `OpaqueRenderProducts` 或后续标准产品。
- 不为 Forward/Deferred 创建第二个 Renderer、PipelineCache、ResourcePool 或 descriptor
  infrastructure。
- 不为追求形式引入空壳 `IOpaqueRenderPath` 多态对象；当前由 `FrameRenderFeatures` 与 Graph
  条件节点表达路径已经足够。只有未来出现第三条 opaque path 且装配逻辑开始重复时再引入。

#### 7.4 RenderGraph 诊断契约

扩展 RenderGraph diagnostics，使路径切换可以从机器可读输出中直接检查：

```cpp
struct RenderGraphPassDiagnostic {
    std::string name;
    std::string groupName;
    RgPassType type;
    RgQueueClass queue;
    bool active;
};
```

- Compiler 同时记录 active 和 culled pass 的 group、type、queue 和稳定顺序。
- JSON 增加 `passes[]`；保留已有 `executionOrder`，避免破坏现有工具。
- DOT 使用 `groupName` 生成 subgraph cluster，并以 Graphics/Compute/Transfer 区分样式。
- Render Path 的标准产品继续由 `render.status.renderPath.opaqueProducts` 输出，不让通用
  RenderGraph 依赖 Renderer 的路径语义。
- Forward 图中必须能识别 `MainForwardOpaque`，Deferred 图中必须能识别
  `Deferred/GBuffer` 与 `Deferred/Lighting`；两者之后汇合到相同的 screen-space、transparent
  和 post-process group。
- 导出只读诊断，不影响 topology key、barrier generation 或执行顺序。

#### 7.5 Shader、Cook 与 Package 收口

- Shader Contract 完整编译并校验 Manifest schema v3：
  - Material family 的 Forward/GBuffer/Shadow/Surface pass 映射。
  - Deferred Lighting、Cluster Build 和 set 6 descriptor。
  - Forward/Deferred 共用的 Global UBO、Material SSBO、Lighting、Atmosphere 和 screen-space
    descriptor ABI。
- `ShaderRegistry::spirvPaths()` 继续作为 Cook shader closure 的唯一来源；禁止 Cook 工具维护
  第二份硬编码 SPIR-V 列表。
- Package verify 必须加载 Shader Manifest，并确认以下内置契约存在且所有文件 hash有效：
  - 默认 PBR family 的 Forward Opaque/Transparent 与 GBuffer programs。
  - Deferred Lighting program。
  - Cluster Build program。
  - ToneMap、Present 和必要 Shadow/Surface programs。
- Cooked Runtime 保留 `--render-path auto|forward|deferred`；`--build-info-json` 报告 Deferred
  和 Bindless 编译能力，启动日志与 `render.status` 报告 requested/active/fallback。
- `Auto` 包必须携带 Forward 和 Deferred 两套所需 SPIR-V，确保设备 capability 或 View Mode
  不兼容时可以回退。
- 不迁移 Catalog、SceneDocument、Native BC7、Environment 或 Runtime Package schema；只有
  Shader ABI 变更时要求重新 Cook。

#### 7.6 Editor、Runtime Control 与文档

- Render Settings 的路径控制保持 `Auto / Forward / Deferred` segmented control，并同时显示：
  - requested 和 active。
  - capability 和 View Mode compatible 状态。
  - fallback reason。
  - opaque、transparent、fallback material draw 数。
- Materials Inspector 显示 family、shading model、Forward/GBuffer支持和实际 fallback，不提供
  全局材质 shader切换。
- `render_settings.get/set` 和 `render.status` 统一使用 `renderPath`、`renderPathActive` 与
  `viewMode`；清理旧 compatibility字段。
- 更新当前文档：
  - `doc/architecture/rendering.md`：最终 Frame Graph、标准产品、Cluster与 Hybrid Transparent。
  - `doc/guides/shader_registry.md`：Manifest v3、program/family/view mode分层。
  - `doc/guides/runtime_control.md`：路径设置、fallback和诊断字段。
  - `doc/guides/editor_ui.md`：Render Path、View Mode和 Materials Inspector工作流。
- 完成后将本计划状态改为 Completed，并按文档规则移入 archive；当前架构说明成为权威实现
  依据。

#### 7.7 实施顺序

1. 修改 resolver，使 `Auto` 在受支持且兼容时选择 Deferred；同步状态展示和测试源码。
2. 删除 ShaderVariant兼容状态，重命名公共 shader类型头并更新 include。
3. 移动 Forward/Post Process模块，更新构建文件和 Graph装配 include。
4. 扩展 RenderGraph JSON/DOT diagnostics，并保持旧字段兼容。
5. 核对 Shader closure、Cook复制和 package verify，补齐缺失的契约检查。
6. 更新 Editor、Runtime Control、架构和使用文档。
7. 构建并启动开发与 Runtime配置，执行 `git diff --check`。

#### 7.8 完成条件

- 没有 Pass 直接依赖旧全局 shader selection或 `ShaderVariant` 命名。
- `Auto`、显式 Forward和显式 Deferred具有确定、可诊断的行为。
- Forward/Deferred共享 Material、Shadow、Atmosphere、screen-space、Transparent与 Post Process。
- RenderGraph JSON/DOT可以区分路径节点、共享节点、active和culled pass。
- Cooked Runtime携带两条路径所需 shader，并能明确选择和报告活动 Render Path。
- Debug与Runtime构建成功，实际启动后 Forward/Deferred切换无新增错误。
- 不修改用户 Scene、Catalog、Environment、派生缓存和 `imgui.ini`。

完成记录（2026-08-21）：

- `Auto` 在 Deferred capability 和 View Mode contract兼容时选择 Deferred；显式 Forward保持
  Forward，显式 Deferred继续执行严格校验。RTX 4060 Laptop GPU实际验证结果为
  `Auto -> Deferred -> Forward -> Deferred`，路径切换不重建 Device、Swapchain或 Pipeline
  Cache。
- 删除 Render Path状态中的迁移字段，将公共 shader类型头从 `ShaderVariant.h` 重命名为
  `ShaderTypes.h`。Runtime Control、Diagnostics和文档统一输出 `viewMode`，不再暴露
  compatibility alias。
- `MainForwardPass`迁入 `features/forward`；HDR Composite、ToneMap和 Present迁入
  `features/post_process`；Surface/GBuffer/Depth Hierarchy继续由共享 `features/surface`管理。
- RenderGraph JSON新增完整 `passes[]`，记录 active/culled、group、type和 queue；DOT按 group
  输出 subgraph cluster。实际状态包含 98个 pass定义，Deferred活动图执行9个节点。
- Runtime Package校验现在加载 Manifest schema v3，验证默认 Material Family的 Forward、
  GBuffer程序，以及 Deferred Lighting、Cluster Build、ToneMap、Present和全部 SPIR-V hash闭包。
  `--build-info-json`明确报告 Forward/Deferred、Legacy/Bindless和 Shader Manifest schema。
- 使用临时 primitive项目生成 schema v3 Cooked package，共132个文件；`package verify`通过。
  包从自身目录独立启动，读取包内 startup Native Scene并构建 Deferred RenderGraph。旧 schema
  package按设计被拒绝，必须重新 Cook。
- `windows-msvc-dev-fast`、完整 `windows-msvc-debug`和 `windows-msvc-runtime`均构建成功；
  Debug测试目标已编译但按项目策略未执行测试套件。开发构建和精简 Cooked Runtime均完成
  实际启动验证。

## Diagnostics And Performance Gates

新增 Diagnostics → Rendering Path：

- requested/active path及 fallback原因。
- path topology generation和 history reset reason。
- opaque/transparent/fallback draw数。
- GBuffer active/resident bytes。
- GBuffer、Light Culling、Deferred Lighting GPU时间。
- 每种 shading model和 family的材质数量。
- cluster occupancy、index count和 overflow。
- Forward/Deferred pipeline cache hit/miss。

性能判断使用相同分辨率、相机、场景和功能组合：

- Algorithm Playground：少量灯基线，验证 Deferred固定成本。
- Main Sponza：复杂几何、材质和屏幕空间效果。
- Scalable Lights测试场景：32、64、128、256灯。

验收原则：

- Deferred不是所有场景都必须更快。
- 少量灯场景允许 Forward更快。
- 多灯场景中 Deferred + Clustered必须表现出可测量收益，否则不改默认路径。
- 切换路径后 CPU submission、Graph compile和 pipeline miss不能长期增加。

## Verification

遵循项目级开发速度策略，只构建和实际运行，不执行 CTest、Golden、视觉回归或 Validation smoke：

1. 每个 Stage构建并启动 `windows-msvc-dev-fast`。
2. Stage 5后构建并启动 `windows-msvc-runtime`。
3. 手动检查 Algorithm Playground、Main Sponza和多灯 Native Scene。
4. 检查 Opaque、MASK、BLEND、Transmission、Unlit和 DefaultLit。
5. 检查 Shadow、Atmosphere、IBL、AO、SSR、SSGI、DDGI、TAA和 Bloom组合。
6. 检查 Viewport resize、scene reload、shader/material family切换和路径切换。
7. 使用 RenderDoc检查 GBuffer、compute lighting、cluster buffers和自动 barrier。
8. 使用现有 GPU profiler/Tracy记录 Forward与 Deferred对比。
9. 执行 `git diff --check`。

## Delivery

建议提交顺序：

1. `refactor: define opaque render path contracts`
2. `refactor: bind shader families to materials`
3. `refactor: share surface material evaluation`
4. `refactor: consolidate screen depth hierarchies`
5. `feat: add deferred gbuffer path`
6. `feat: shade opaque surfaces with deferred lighting`
7. `feat: add hybrid deferred and forward rendering`
8. `feat: cull scene lights into clusters`
9. `feat: expose render path diagnostics and controls`
10. `docs: document forward and deferred render paths`

## Assumptions

- 顶层 Renderer、RenderGraph、PipelineCache和 MaterialSystem继续唯一。
- Deferred v1单采样并依赖 TAA；Forward继续支持现有 MSAA策略。
- PBR DefaultLit是第一种完整支持两条路径的 shader family。
- Legacy保持 Forward Only；透明和 Transmission保持 Forward。
- Render Path和 View Mode是会话设置，不进入 SceneDocument。
- 第一版不需要新增外部库。
- 实现过程中不修改用户 Scene、Catalog、Environment、派生资产和 `imgui.ini`。
