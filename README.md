# VulkanLab

VulkanLab is an experimental Vulkan renderer and scene editor for Windows. It
is a learning and research project focused on explicit rendering architecture,
glTF asset workflows, real-time lighting, and graphics algorithm development.

## Highlights

- Forward PBR rendering with normal maps, alpha modes, transmission, and HDR
  tone mapping.
- Native scene documents with model instances, hierarchy editing, lights,
  cameras, undo/redo, viewport picking, and transform gizmos.
- Directional shadows, scalable scene lights, IBL, procedural atmosphere,
  bloom, TAA, SSAO, and optional FidelityFX CACAO comparison.
- CPU frustum, distance, small-object, shadow, and Hi-Z occlusion culling.
- Offline KTX2/native BC7 asset pipeline, glTF validation, package cooking,
  and incremental GPU upload.
- RenderDoc labels, Vulkan validation profiles, GPU pass timings, and optional
  Tracy profiling.

The project is under active development. APIs, scene formats, and rendering
results may change between commits.

## Requirements

- Windows 10 or 11
- Visual Studio 2022 with the Desktop development with C++ workload
- CMake 3.22 or newer
- Vulkan SDK with `glslc` and `spirv-val`
- A Vulkan-capable GPU and current graphics driver

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
the test targets:

```powershell
cmake --preset windows-msvc-dev-fast
cmake --build --preset windows-msvc-dev-fast
./build/windows-msvc-dev-fast/Debug/VulkanLab.exe --project .
```

The minimal runtime configuration is built with:

```powershell
cmake --preset windows-msvc-runtime
cmake --build --preset windows-msvc-runtime
```

Large optional models, source HDR environments, generated KTX2 caches, build
outputs, captures, and logs are intentionally excluded from Git. Missing
optional catalog entries remain unavailable until their source assets are
imported locally.

## Documentation

- [Documentation index](doc/README.md)
- [Build and run guide](doc/guides/build_and_run.md)
- [Rendering architecture](doc/architecture/rendering.md)
- [Resource loading architecture](doc/architecture/resource_loading.md)
- [Scene documents](doc/architecture/scene_documents.md)
- [RenderDoc and validation](doc/guides/renderdoc_validation.md)
- [Tracy profiling](doc/guides/tracy_profiling.md)

## License

VulkanLab source code is available under the [MIT License](LICENSE).
Third-party libraries, models, textures, and reference material remain subject
to their respective licenses.
