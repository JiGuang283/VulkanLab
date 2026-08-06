# 实时光照算法参考资料

> Status: Reference Index
> Last refreshed: 2026-08-05

本目录保存 AO、Temporal AA、屏幕空间反射、屏幕空间 GI、Probe GI 和光追降噪相关的参考实现与论文索引。

这里的内容不是 VulkanLab 的构建依赖：

- `README.md`、`sources.json` 和下载脚本可以提交。
- 实际第三方源码、论文和网页快照位于 `downloads/`，由 `.gitignore` 排除。
- CMake 不扫描本目录，Cook 和 Runtime package 也不得复制这里的内容。
- 将参考代码移入 `src/` 或 `shader/` 前，必须单独检查许可证、保留 attribution，并按 VulkanLab 的资源和 Shader ABI 重新实现。

## 获取资料

在仓库根目录运行：

```powershell
powershell -ExecutionPolicy Bypass -File `
  references/rendering_algorithms/Fetch-RenderingReferences.ps1
```

重新下载全部内容：

```powershell
powershell -ExecutionPolicy Bypass -File `
  references/rendering_algorithms/Fetch-RenderingReferences.ps1 -Force
```

只下载论文和文档：

```powershell
powershell -ExecutionPolicy Bypass -File `
  references/rendering_algorithms/Fetch-RenderingReferences.ps1 `
  -SkipCode
```

脚本完成后会生成：

```text
downloads/
  code/
  papers/
  docs/
  SHA256SUMS.txt
  fetch-report.json
```

## 源码参考

| 目录 | 用途 | 使用方式 | 重要限制 |
|---|---|---|---|
| `code/xegtao` | GTAO、depth prefilter、spatial denoise、bent normal | 算法参考 | 已归档；DirectX/HLSL；不直接接入 Runtime |
| `code/taa` | Projection jitter、history resolve、clipping | 算法参考 | DirectX/HLSL；必须适配现有 motion/history 约定 |
| `code/fidelityfx-cacao` | 成熟 Compute AO | 已用于设计审计与质量对照 | 正式 comparison backend 来自固定的 `external/FidelityFX-CACAO` submodule；本参考副本不参与编译 |
| `code/fidelityfx-sssr` | SSSR、SPD、Denoiser 与 Vulkan sample | SSR 集成候选 | SDK 适配成本较高；不得绕过 Registry 和 Pass 生命周期 |
| `code/rtxgi-ddgi` | Probe update、visibility、relocation/classification | DDGI 参考或可选模块 | 需要 Vulkan Ray Tracing；Shader 使用 HLSL/DXC |
| `code/nrd` | RT diffuse/specular/shadow 时空降噪 | RT 阶段集成候选 | 需要 hit distance、normal/roughness、view depth 和 motion contract |

所有源码固定到 `sources.json` 中记录的 commit。下载目录使用 detached HEAD，防止无意中跟随上游变化。

## 论文与演讲

| 文件 | 主要用途 |
|---|---|
| `gtao_2016.pdf` | GTAO 的可见性积分、近场遮蔽和多次反弹近似 |
| `temporal_supersampling_2014.pptx` | UE4 TAA、HDR clipping、ghosting 和 translucency 处理 |
| `screen_space_ray_tracing_2014.pdf` | Perspective-correct DDA 与多层 depth 思路 |
| `screen_space_indirect_lighting_bitmask_2023.pdf` | Visibility bitmask AO/SSGI |
| `ddgi_irradiance_fields_2019.pdf` | DDGI irradiance、distance moments 和 visibility query |
| `ddgi_scaling_2021.pdf` | Probe classification、relocation 和大场景扩展 |
| `svgf_2017.pdf` | 稀疏光追信号的时空方差估计与 A-Trous 滤波 |

为控制体积，JCGT 论文优先下载 low-resolution 版本。下载文件仅用于本地研究，引用、再分发和代码移植必须遵守各自页面与文件中的许可。

## 官方文档快照

`docs/` 保存关键官方网页的 HTML 快照，便于断网检索。网页可能依赖在线样式和图片，因此 README 中的原始 URL 始终是权威入口：

- FidelityFX SSSR technique manual。
- FidelityFX CACAO product page。
- NVIDIA NRD README。
- NVIDIA RTXGI algorithm guide。

快照只用于搜索文字和核对接口，不应被视为固定 API 文档；真正接入 SDK 前必须重新查看对应上游版本。

## 与开发路线的关系

参考资料对应 [AO、反射与全局光照算法路线](../../doc/development/ao_reflection_gi_plan.md)：

```text
SSAO             -> 项目内实现；可选 FidelityFX CACAO backend 用于质量/性能对照
TAA              -> 项目内实现，TAA repo 用于 history 策略参考
GTAO             -> 参考 XeGTAO 后移植为 Vulkan/GLSL
SSR              -> 先固定本项目 contract，再决定是否接入 FidelityFX SSSR
SSGI             -> 项目内实现，参考 visibility bitmask 论文
Reflection Probe -> 复用现有 KTX2/Environment 管线
DDGI             -> 参考 RTXGI，等 Vulkan RT 基础完成后决定依赖方式
RT Denoising     -> 优先评估 NRD，不从零开发生产级 denoiser
```

## 引用与许可证规则

1. 不从参考目录直接 include、link 或复制二进制。
2. 移植算法时在源码注释和项目文档中记录论文、仓库、commit 与许可证。
3. MIT 源码仍需保留版权和许可声明。
4. NVIDIA SDK 等非 MIT 项目必须在接入前单独审核其许可证和分发条款。
5. 论文中的公式可以按学术引用重新实现，但图像、正文和附件不得默认纳入项目发布包。
