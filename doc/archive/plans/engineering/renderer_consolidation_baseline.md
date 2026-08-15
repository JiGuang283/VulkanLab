# VulkanLab 渲染器收口 Stage 0 基线

> Status: Archived Baseline
> Last verified: 2026-08-15
> Verified against: `e75be09d6af0d04a49a1dcafeff85a0bcd795ab6`

## Purpose

本文固化[渲染器收口与 Application 重构计划](renderer_consolidation_and_app_refactor_plan.md) Stage 0 的代码、运行时资源和功能范围基线。后续阶段必须使用同一采样条件对比，不能通过删除渲染算法来获得表面上的代码或显存下降。

本基线描述的是当前实现，不代表目标架构已经完成。

## Reproducible Runtime Sample

### Environment

| Item | Value |
|---|---|
| Code revision | `e75be09d6af0d04a49a1dcafeff85a0bcd795ab6` |
| Build | `windows-msvc-debug`, Debug |
| GPU | NVIDIA GeForce RTX 4060 Laptop GPU |
| Vulkan SDK | 1.4.335 |
| Material binding | Auto resolved to Bindless |
| Validation | Off; Debug Utils remains enabled |
| Window / render extent | 800 x 600 |
| GUI | Disabled, so viewport is exactly 800 x 600 |
| Fixed delta | 0.0166667 seconds |
| Scene | Viking Room, model preview |
| Shader | PBR-lite NormalMapped |
| Texture limit | 2048 |

Debug build reported `dirty=true` because this baseline and its parent plan were uncommitted documentation changes. The executable code itself was built from the revision above.

### Commands

Build only the renderer and control client; no test target is executed:

```powershell
cmake --build build/windows-msvc-debug --config Debug `
  --target VulkanLab VulkanLabCtl
```

From `build/windows-msvc-debug/Debug` start the renderer:

```powershell
./VulkanLab.exe `
  --project C:/Project/vulkan_learn `
  --runtime-control `
  --runtime-control-pipe consolidation_baseline `
  --automation `
  --window-size 800x600 `
  --fixed-delta 0.0166667 `
  --no-gui `
  --validation off
```

From a second terminal in the same directory:

```powershell
./VulkanLabCtl.exe --pipe consolidation_baseline `
  render wait --stable-frames 12 --timeout-ms 30000

./VulkanLabCtl.exe --pipe consolidation_baseline --json `
  render status > consolidation-baseline-render-status.json

./VulkanLabCtl.exe --pipe consolidation_baseline --json `
  info > consolidation-baseline-system-info.json

./VulkanLabCtl.exe --pipe consolidation_baseline quit
```

The JSON files are local measurement artifacts and are not committed. The authoritative fields used below are `result.renderGraph`, `result.screenSpace`, `result.viewport`, and `result.gpuTimings`.

## Runtime Baseline

### RenderGraph

| Metric | Baseline |
|---|---:|
| Active passes | 24 |
| Culled passes | 91 |
| Dependency edges | 38 |
| Automatic barriers | 61 |
| Layout barriers | 31 |
| Hazard barriers | 59 |
| Active image bytes | 341,028,376 bytes / 325.23 MiB |
| Resident image bytes | 520,848,560 bytes / 496.72 MiB |
| Active-to-resident gap | 179,820,184 bytes / 171.49 MiB |
| Screen-space estimated bytes | 78.73 MiB |
| Topology hash | `ccb081de891731db` |

The active-to-resident gap is the primary Stage 3 comparison target. The current `RenderResourceRegistry` realizes registered resources even when their RenderGraph nodes are culled.

### Active Execution Order

```text
DirectionalShadow/Cascade0..3
SurfacePrepass
HiZBuild/Mip0..9
OcclusionCull/ClearCounter
OcclusionCull/Dispatch
OcclusionCull/IndirectReady
SkyBackground
MainForwardOpaque
ScreenSpaceLightingComposite/Copy
MainForwardTransparent
ToneMap
Present + UI
```

The default frame still pays for SurfacePrepass, Hi-Z, and OcclusionCull with one source draw. `ScreenSpaceLightingComposite/Copy` remains active while SSR and SSGI are disabled.

### Largest Resident Images

| Resource | Lifetime | Resident MiB | Current reason |
|---|---|---:|---|
| Directional Shadow Depth | Imported | 64.00 | Four 2048 CSM layers |
| Baseline Indirect Specular MSAA | PerFrame | 58.59 | MainForward MRT even with SSR off |
| Baseline Indirect Diffuse MSAA | PerFrame | 58.59 | MainForward MRT even with SSGI off |
| HDR MSAA Color | PerFrame | 58.59 | Sky and forward HDR target |
| Main Depth | PerFrame | 29.30 | Multisampled forward depth |
| HDR Color | PerFrame | 7.32 | Resolved HDR |
| Surface Normal Roughness | History | 7.32 | Surface data path |
| Baseline Indirect Diffuse | PerFrame | 7.32 | Resolved indirect diffuse |
| Baseline Indirect Specular | PerFrame | 7.32 | Resolved indirect specular |
| Composited HDR Color | PerFrame | 7.32 | Fallback copy and transparent target |
| Visibility Hi-Z | PerFrame | 4.88 | Ten-mip occlusion pyramid |
| Surface Albedo Metallic | PerFrame | 3.66 | Surface data path |
| Surface Depth | History | 3.66 | Surface data path |
| Surface Motion | History | 3.66 | Surface data path |
| Viewport Color | PerFrame | 3.66 | ToneMap output |

The three 58.59 MiB rows are the dominant 4x MSAA color attachments. Removing algorithms is not an accepted optimization; Stage 3 must make their attachments conditional on active frame features.

### Default Feature State

| Feature | Default state |
|---|---|
| Directional shadows | Enabled |
| Point / Spot shadow capacity | 2 / 2 selected lights |
| Frustum culling | Enabled |
| Shadow culling | Enabled |
| Hi-Z occlusion culling | Enabled |
| Distance culling | Disabled |
| Small-object culling | Disabled |
| Bloom | Disabled |
| IBL | Disabled |
| Skybox | Disabled |
| AO | Off |
| TAA | Off |
| Reflection | IBL-only baseline; SSR inactive |
| GI | Ambient-or-IBL baseline; SSGI/DDGI inactive |
| Atmosphere | Scene-driven; absent in this sample |

## Source And Dependency Baseline

Line counts use PowerShell `Get-Content` on tracked `.cpp` and `.h` files. They are intended for trend comparison, not language-level LOC accounting.

### Key Files

| File | Lines | Direct includes |
|---|---:|---:|
| `src/app/Application.cpp` | 9,932 | 92 |
| `src/app/Application.h` | 368 | 26 |
| `src/render/Renderer.cpp` | 1,807 | 53 |
| `src/render/Renderer.h` | 333 | 22 |
| `src/render/RenderGraph.cpp` | 1,629 | 17 |
| `src/render/RenderResourceRegistry.cpp` | 1,155 | 14 |

### Source Areas

| Directory | C++ files | Lines |
|---|---:|---:|
| `src/app` | 5 | 10,130 |
| `src/render` | 122 | 23,709 |
| `src/render/pass` | 50 | 9,857 |
| `src/assets` | 42 | 7,221 |
| `src/scene` | 43 | 5,462 |
| `src/core` | 45 | 4,898 |
| `src/editor` | 22 | 3,828 |
| `src/control` | 10 | 1,197 |

### Application Coupling

`Application.cpp` directly includes and coordinates all of these domains:

- Asset catalog, import, validation, artifact, environment, and derived-cache workflows.
- Named Pipe Runtime Control and protocol JSON.
- Device, descriptors, FrameSync, upload, swapchain, logging, and profiling.
- Dock workspace, authoring session, viewport controller, panels, and Win32 dialogs.
- Renderer, Shader paths, materials, shadows, temporal state, ray tracing, and DDGI details.
- AssetRepository, EnvironmentAssetRepository, SceneLoadManager, RuntimeWorld, and scene registry.

`Application` directly implements `RuntimeControlHost`; 36 `Application::runtime*` methods are present. This is the Stage 4-9 extraction baseline.

## Compatibility And Migration Debt Inventory

### Viking / OBJ Compatibility Island

Tracked Viking-specific assets and parser files:

```text
models/viking_room.obj
textures/viking_room.png
src/tiny_obj_loader.cpp
external/tiny_obj_loader.h/tiny_obj_loader.h
tests/render/goldens/viking-legacy.json
tests/render/goldens/viking-legacy.png
tests/render/viking_debug_shadow_smoke.json
tests/render/viking_legacy_golden.json
tests/render/viking_legacy_smoke.json
tests/render/viking_pbr_shadow_golden.json
tests/render/viking_pbr_shadow_smoke.json
```

Current non-archive source and current-document references are contained in:

```text
assets/catalog.json
CMakeLists.txt
src/app/Application.cpp
src/app/Application.h
src/app/Config.h
src/app/SceneWorkflowController.h
src/render/Mesh.cpp
src/render/RenderCommand.h
src/render/Visibility.cpp
src/scene/AssetRepository.h
src/scene/BuiltinScenes.cpp
src/scene/BuiltinScenes.h
src/scene/ModelSourceResolver.cpp
src/scene/ModelSourceResolver.h
src/scene/PrimitiveModelFactory.h
src/scene/Scene.cpp
src/scene/Scene.h
src/scene/SceneFactory.h
src/scene/SceneObject.h
src/scene/SceneRegistryBuilder.cpp
src/scene/SceneRegistryBuilder.h
tests/AssetToolCatalogImportTest.cmake
tests/EnvironmentIblVisualTest.cmake
tests/RenderTestSpecTests.cpp
tests/SceneCatalogEditorTests.cpp
tests/SceneImportServiceTests.cpp
tools/validation/Run-ValidationSmoke.ps1
doc/architecture/overview.md
doc/architecture/rendering.md
doc/architecture/resource_loading.md
doc/architecture/scene_documents.md
doc/guides/build_and_run.md
doc/guides/renderdoc_validation.md
doc/guides/runtime_control.md
doc/guides/tracy_profiling.md
doc/guides/visual_regression.md
```

Historical references under `doc/archive/` are intentionally excluded and will remain unchanged.

The compatibility path currently includes synchronous `SceneFactory`, `SceneObject`, `Mesh::fromOBJ()`, `RenderItemOwnerKind::LegacyObject`, `vkl_obj_parser`, `Config::texturePath`, and the synchronous `Application::loadScene()` device-idle/pipeline-cache-clear path.

### Legacy Pass Interface

Twenty-four Pass implementation files still define an old `execute()` path in addition to RenderGraph setup/recording:

```text
AtmosphereLutPass
BloomPass
CacaoNormalAdapterPass
CacaoPass / CacaoPassDisabled
DdgiPass
DirectionalShadowPass
FrameGraphExternalPasses
GtaoPass
HdrCompositePass
HiZBuildPass
MainForwardPass
OcclusionCullPass
PointShadowPass
PresentPass
ScreenSpacePyramidPass
SkyBackgroundPass
SpotShadowPass
SsaoPass
SsgiPass
SsrPass
SurfacePrepass
TaaPass
ToneMapPass
```

This inventory is the Stage 2 deletion scope. RenderGraph `setup()` and `recordNode()` are the retained path.

### Runtime Self-Tests

Normal application startup still invokes:

- `runResourcePoolSelfTest()` from `Application::init()`.
- `runModelAssetSharingSmoke()` after a model asset becomes ready.

These are Stage 2 migration debt. They must move to explicit tests or diagnostics tooling rather than remain in normal runtime behavior.

### Compatibility Aliases

Current aliases to retire after their callers have migrated:

```text
PreparedSceneData -> PreparedModelData
ScenePrepareFactory -> ModelPrepareFactory
RenderCommand -> RenderItem
```

## Feature Retention Matrix

All rows below are maintained renderer capabilities. Refactoring may change ownership and resource activation, but may not silently delete these features.

| Family | Capability | Decision |
|---|---|---|
| Core Forward / HDR | PBR-lite Forward and NormalMapped | Keep |
| Core Forward / HDR | Legacy and material debug variants | Keep as diagnostics/baseline |
| Core Forward / HDR | HDR, ACES/Reinhard/PassThrough ToneMap | Keep |
| Materials | glTF alpha, transmission, emissive, double-sided, UV/AO | Keep |
| Materials | Bindless and Legacy binding backends | Keep |
| Lighting | Directional, Point, and Spot lights | Keep |
| Shadows | Four-cascade directional shadow | Keep |
| Shadows | Point cube-array and Spot shadows | Keep |
| Visibility | Frustum, distance, and small-object CPU culling | Keep |
| Visibility | Surface prepass, Hi-Z, and occlusion culling | Keep |
| Environment | IBL and Skybox | Keep |
| Environment | Procedural atmosphere, Sun, and aerial perspective | Keep |
| Ambient Occlusion | SSAO | Keep |
| Ambient Occlusion | GTAO | Keep |
| Ambient Occlusion | Optional FidelityFX CACAO comparison | Keep |
| Reflections | SSR | Keep |
| Reflections | Local reflection probes | Keep |
| Global Illumination | SSGI | Keep |
| Global Illumination | Ray-query DDGI | Keep |
| Temporal / Post | TAA | Keep |
| Temporal / Post | Bloom | Keep |
| Tooling | RenderGraph diagnostics, RenderDoc labels, GPU timings, Tracy | Keep |
| Authoring | Native Scene workspace and offline asset pipeline | Keep |
| Automation | Optional Runtime Control | Keep, isolate as developer adapter |

Viking Room, OBJ loading, synchronous SceneFactory, duplicate old Pass execution, and runtime self-tests are compatibility or migration debt rather than renderer algorithms; they are not protected by this matrix.

## Comparison Rules

For later performance/resource comparisons:

1. Use the exact 800x600 no-GUI automation launch above.
2. Keep PBR-lite NormalMapped, validation off, auto material binding, and a deterministic smoke scene.
3. After Stage 1, replace only the Viking scene with the committed Renderer Smoke Scene; record that scene change beside the measurement.
4. Wait at least 12 stable frames and require pending upload to be zero.
5. Compare active and resident bytes separately. Lower resident bytes caused by lazy allocation is valid; deleting an algorithm is not.
6. Record the topology hash and active execution order so a resource change cannot be mistaken for the same frame topology.
7. GPU timing is diagnostic only; Debug timing and driver variance are not a hard acceptance gate.

## Stage 0 Completion Evidence

- The current Debug renderer and control client build successfully.
- Runtime Control reproduced an exact 800x600 sample and reported stable frames.
- Runtime Graph, resource, source scale, dependency, compatibility, and alias baselines are recorded above.
- The feature retention matrix explicitly preserves all current rendering algorithms.
- README now positions VulkanLab as a Vulkan 1.3 real-time renderer under active development and distinguishes its authoring/tooling roles.
- No renderer algorithm or behavior was removed during Stage 0.
