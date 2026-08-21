# Renderer Performance Benchmark

`Measure-Renderer.ps1` drives VulkanLab through Runtime Control and records a
repeatable frame-rate and GPU-pass baseline. Use a Release build for renderer
performance comparisons; Debug results are useful for development workflow
diagnostics but magnify per-draw C++ overhead.

```powershell
.\tools\performance\Measure-Renderer.ps1 `
  -Renderer .\build\windows-msvc-release\Release\VulkanLab.exe `
  -ControlTool .\build\windows-msvc-release\Release\VulkanLabCtl.exe `
  -Scene "Main Sponza" `
  -Profile Default `
  -MaterialBinding bindless `
  -Output .\build\perf-results\main-sponza-default.json
```

Profiles:

- `Minimal`: shadows, occlusion, AO, TAA, SSR, SSGI, IBL, skybox, and bloom off.
- `Default`: keep the renderer's startup settings.
- `Ssao`: Minimal plus SSAO.
- `Ssr`: Minimal plus SSR.
- `Ssgi`: Minimal plus SSGI.
- `Taa`: Minimal plus TAA.
- `Ddgi`: Minimal plus DDGI; the selected Native Scene must contain an
  active DDGI probe volume and the device must support ray query.
- `SsgiDdgi`: Minimal plus combined SSGI/DDGI under the same DDGI
  prerequisites.

The benchmark defaults to a 1280x720 no-GUI automation window. Pass `-Gui` to
include the editor workspace. The result includes presented FPS, estimated CPU
frame time, median GPU frame/pass times, active RenderGraph counts, material
binding backend, active render path, full RenderGraph execution order,
screen-space runtime state, and validation error count.
