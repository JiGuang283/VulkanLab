# Shader Registry

> Status: Current
> Last verified: 2026-08-21
> Verified against: Forward / Deferred Stage 7 working tree

[`shader/manifest.json`](../../shader/manifest.json) 是 Shader program、材质 Shader
Family 和全局 View Mode 的唯一权威来源。运行时不扫描目录，也不根据文件名猜测 stage
组合；CMake、VulkanLab、CookPackageBuilder、Package Verify 和 Shader Contract 都读取同一
份 Manifest。

## 三层模型

### Program

`programs[]` 描述一个可创建 graphics 或 compute pipeline 的低级 stage 组合：

- `id`：稳定的小写机器 ID。
- `contract`：`main-forward`、`shadow-depth`、`punctual-shadow-depth`、
  `surface-prepass`、`gbuffer`、`deferred-lighting`、`fullscreen` 或 `compute`。
- `vertex` / `fragment` / `compute`：相对 `shader/` 的 GLSL 路径。
- `materialTextures`：使用 Material SSBO 与材质纹理；同时生成 Legacy 和 Bindless fragment
  SPIR-V。
- `sceneLights`：读取 `set=0 binding=1` Scene Light SSBO。
- `clusteredLighting`：读取 `set=6` cluster records/indices。当前 PBR Forward 和 Deferred
  Lighting 使用。
- `atmosphere`、`screenSpace`、`ddgi`：声明对应固定 descriptor ABI。
- `lightingMrt`、`surfaceMrt`：声明构建系统需要生成的缩减 MRT fragment 变体。
- `targetEnv`：可选 GLSL 目标；Ray Query 等程序使用 `vulkan1.2`。

Program 只描述 GPU contract，不是用户可选择的“材质”或“渲染模式”。Shadow、GBuffer、
Deferred Lighting、Cluster Build、ToneMap 和 Present 都属于 Program。

### Material Shader Family

`materialShaderFamilies[]` 决定一种材质在不同 Pass 使用哪个 Program：

```text
forwardOpaque / forwardTransparent
surfaceOpaque / surfaceMask
gBufferOpaque / gBufferMask
directionalShadowMask / pointShadowMask / spotShadowMask
```

当前内置 Family：

- `builtin.default-lit`：glTF metallic-roughness PBR，完整支持 Forward 和 Deferred。
- `builtin.unlit`：用于 `KHR_materials_unlit`，仍参与 depth、GBuffer 和 MASK shadow contract。

`MaterialTemplate` 持有稳定 Family handle 和 pipeline render state；`MaterialInstance` 持有参数、
纹理与 GPU material handle。实际 Program 通过
`(MaterialShaderFamily, MaterialShaderPass, MaterialBindingMode)` 解析，不能由全局 UI 替材质
选择另一套 shader。

### View Mode

`viewModes[]` 是全局观察方式：

- 没有 `program` 的 View Mode 使用各材质自己的 Family。默认 `Lit` 即
  `pbr-lite-normal-mapped`，并支持 Deferred。
- 指定 `program` 的 View Mode 是显式覆盖，例如 Legacy、BaseColor、Normal、Shadow 等调试
  视图；这些模式当前为 Forward-only。
- `toneMapping` 与 `bloom` 描述输出策略，不定义材质本身。
- `default` 必须且只能出现一次；`order` 决定 Editor 排序。

Render Path 与 View Mode 是独立状态。`Auto` 在 Deferred capability 可用且 View Mode 兼容时
选择 Deferred，否则回退 Forward并报告原因；显式 Forward始终使用 Forward。

## 构建产物

所有路径必须使用 `/`、不得包含 `..`，并以 Manifest中记录的 GLSL扩展名结尾。CMake从
Manifest收集源文件，配置 include依赖并运行 `glslc`；产物必须先通过 `spirv-val` 才会复制
到 runtime `shader/`。

`materialTextures=true` 的 fragment同时生成普通与
`VKL_BINDLESS_MATERIALS=1` 版本。`lightingMrt` 和 `surfaceMrt` 还会生成对应 attachment数量的
缩减输出版本。`ShaderRegistry::spirvPaths()` 返回全部实际产物，是 Cook shader closure 的
唯一来源。

## 新增材质 Shader Family

1. 明确 Shading Model，以及它需要支持的 Forward、GBuffer、Surface 和 Shadow Pass。
2. 在 `shader/include/` 中复用共享 Surface求值、BRDF、Direct Lighting和 descriptor ABI。
3. 在 `programs[]` 登记每个低级 Program及 capability字段。
4. 在 `materialShaderFamilies[]` 登记完整 Pass映射。缺少必需映射会在 Manifest加载阶段失败。
5. 构建 Debug，确认所有普通、Bindless和缩减 MRT SPIR-V生成并通过 `spirv-val`。
6. 在 Materials Inspector检查 Family、Shading Model和实际 Pass fallback。

新增调试 View Mode时，只在确实需要覆盖所有材质时增加 `viewModes[]` 条目；不要把普通材质
Family伪装成全局 View Mode。

## 内部 Program 与 Cook

内部 Program只登记在 `programs[]`，不会自动创建 RenderGraph节点。Graph是否执行某节点仍由
`FrameRenderFeatures`、设备能力和 active Render Path决定。

Cook先用 `ShaderRegistry::spirvPaths()` 复制全部 SPIR-V，再复制 Manifest。Package Verify重新
加载 Registry并验证：

- 所有 SPIR-V 位于 package root内并受 package hash清单覆盖。
- 默认 Material Family具有 Forward Opaque/Transparent和 GBuffer Opaque/MASK程序。
- PBR Forward、Deferred Lighting、Cluster Build、ToneMap和 Present内置契约存在。

因此 `Auto` Cooked Runtime同时携带 Forward和 Deferred路径，不依赖项目源码或硬编码的第二份
Shader清单。

## Runtime Control

兼容命令名仍为 `shader.list/current/set`，但其语义已经是 View Mode。响应使用稳定 View Mode
ID和 display name，并通过 `viewMode` 字段报告当前模式。

当前不支持运行时 shader编译、热重载、目录自动扫描、任意用户 shader或第三方 Shader插件。
