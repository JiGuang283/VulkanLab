# VulkanLab

VulkanLab is a Vulkan 1.3 real-time renderer under active development for
Windows. It provides a focused scene-authoring workspace and offline asset
pipeline for implementing, profiling, and comparing modern rendering
techniques.

## Status

This is a renderer development project, not a production-ready game engine.
The editor exists to author repeatable renderer test scenes, and the asset
tools prepare data for the renderer. VulkanLab does not aim to provide a
general-purpose game editor, DCC workflow, gameplay framework, or stable public
API.

Scene formats, shader ABI, renderer architecture, and visual output may change
between commits while the project is under active development.

## Rendering Features

- Vulkan 1.3 RenderGraph using Dynamic Rendering and Synchronization2.
- Forward PBR with normal mapping, glTF alpha modes, transmission, HDR tone
  mapping, bindless material textures, and a legacy descriptor fallback.
- Cascaded directional shadows plus point-light cube-array and spot-light
  array shadows.
- SSAO, GTAO, optional FidelityFX CACAO, SSR, SSGI, DDGI, reflection probes,
  IBL, procedural atmosphere, aerial perspective, TAA, and bloom.
- CPU frustum, distance, small-object and shadow culling, followed by Hi-Z
  occlusion culling and indirect draw suppression.
- GPU timestamp diagnostics for RenderGraph nodes.

## Architecture Highlights

- `Application` is a composition root. Scene runtime, project workflows,
  render settings, editor behavior, and Runtime Control are separate services.
- `RenderGraph` is the only frame-pass execution and synchronization path.
- `RenderResourcePool` tracks logical activity and physical
  `Unallocated / Resident / Retiring` state using submission serials.
- Renderer code is grouped into explicit feature modules for forward/HDR,
  shadows/visibility, ambient occlusion, reflections, global illumination,
  atmosphere/environment, and temporal/post-processing work.
- `AssetRepository` shares immutable GPU `ModelAsset` generations across model
  previews and Native Scene instances.
- Optional development systems are selected at CMake target boundaries and do
  not change the renderer's material or shader ABI.

## Scene Authoring And Assets

- Native `.vkscene.json` documents with entities, hierarchy, transforms,
  models, cameras, scalable lights, atmosphere, reflection probes, and DDGI
  volumes.
- Docking editor with a dedicated scene viewport, Outliner, Inspector,
  undo/redo, CPU bounds picking, model drag-and-drop, and ImGuizmo transforms.
- glTF/GLB model import guarded by the Khronos glTF Validator.
- Offline KTX2/native BC7 texture caches and baked KTX2 IBL environments.
- Transactional scene loading, incremental GPU upload, asset generations, and
  submission-serial retirement.
- Native Scene dependency closure, package verification, and minimal cooked
  runtime output.

## Developer Tooling

- `VulkanLabAssetTool.exe` for validation, import, derived assets, cache
  maintenance, cooking, and package verification.
- Optional local Named Pipe Runtime Control and `VulkanLabCtl.exe` for
  automation and diagnostics.
- RenderDoc object names and event labels, selectable Vulkan validation
  profiles, automated capture/visual comparison infrastructure, and optional
  Tracy CPU/Vulkan profiling.

These tools support renderer development; they are not end-user runtime
features. The minimal runtime preset compiles them out where applicable.

## Requirements

- Windows 10 or 11
- Visual Studio 2022 with the Desktop development with C++ workload
- CMake 3.22 or newer
- Vulkan SDK with `glslc` and `spirv-val`
- A Vulkan 1.3-capable GPU and current graphics driver

## Clone

The project uses Git submodules for KTX-Software, DirectXTex, Tracy,
SPIRV-Reflect, ImGuizmo, and FidelityFX CACAO.

```powershell
git clone --recursive https://github.com/JiGuang283/VulkanLab.git
cd VulkanLab
```

For an existing checkout:

```powershell
git submodule update --init --recursive
```

## Build And Run

The fast development preset builds the editor-enabled Debug renderer without
test targets:

```powershell
cmake --preset windows-msvc-dev-fast
cmake --build --preset windows-msvc-dev-fast
./build/windows-msvc-dev-fast/run/Debug/VulkanLab.exe --project .
```

The minimal runtime configuration is built with:

```powershell
cmake --preset windows-msvc-runtime
cmake --build --preset windows-msvc-runtime
./build/windows-msvc-runtime/run/Release/VulkanLab.exe --project .
```

Large optional models, source HDR environments, generated KTX2 caches, build
outputs, captures, and logs are intentionally excluded from Git. Missing
optional catalog entries remain unavailable until their source assets are
imported locally.

Runnable images are isolated under `build/<preset>/run/<Config>`. Writable
runtime data defaults to `%LOCALAPPDATA%/VulkanLab/Workspaces/<projectId>` and
can be redirected with `--workspace <path>` for automation or diagnostics.

## Current Limitations

- Rendering uses one Graphics Queue and one primary command buffer; there is no
  async compute, parallel command recording, or queue ownership scheduler.
- The Forward path still records draws on the CPU. Hi-Z suppresses GPU draw
  work but does not compact draw calls into a fully GPU-driven stream.
- RenderGraph does not yet alias transient memory. Feature resources use
  serial-based retirement, while renderer startup still has a temporary
  all-resource bootstrap allocation peak.
- Screen-space AO, reflections, and GI inherit visibility, disocclusion, and
  temporal-history limitations; DDGI is a bounded development implementation,
  not a production probe system.
- The renderer currently targets Windows desktop Vulkan and native BC7 cooked
  assets. Cross-platform texture profiles and runtime streaming are not
  implemented.
- Scene authoring is intentionally renderer-focused: no gameplay, scripting,
  physics, networking, multi-user editing, or complete ECS is provided.

## Documentation

- [Documentation index](doc/README.md)
- [Build and run guide](doc/guides/build_and_run.md)
- [System architecture](doc/architecture/overview.md)
- [Rendering architecture](doc/architecture/rendering.md)
- [RenderGraph architecture](doc/architecture/render_graph.md)
- [Resource loading architecture](doc/architecture/resource_loading.md)
- [Scene documents](doc/architecture/scene_documents.md)
- [Runtime Control](doc/guides/runtime_control.md)
- [RenderDoc and validation](doc/guides/renderdoc_validation.md)
- [Tracy profiling](doc/guides/tracy_profiling.md)

## License

VulkanLab source code is available under the [MIT License](LICENSE).
Third-party libraries, models, textures, and reference material remain subject
to their respective licenses.
