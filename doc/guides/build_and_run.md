# 构建与运行

> Status: Current
> Last verified: 2026-07-19
> Verified against: `df02615`

## 环境要求

- Windows 10/11 和支持 Vulkan 的显卡驱动。
- Visual Studio 2022 C++ 工具链。
- CMake 3.20 或更高版本。
- Vulkan SDK。CMake 需要能找到 Vulkan、`glslc` 和 SDK 中的 GLM 头文件。
- 仓库内 `external/` 依赖完整，尤其是 `glfw/lib-vc2022`、ImGui、stb、VMA 和 glTF 头文件。
- 阶段三使用 KTX-Software v4.4.2 submodule。首次克隆或更新后必须递归初始化：

```powershell
git submodule update --init --recursive external/ktx
```

如果模型资产由 Git LFS 管理，还需要先安装 Git LFS 并在仓库根目录执行 `git lfs pull`。KTX2 派生缓存本身不提交到 Git LFS 或普通 Git。

首次配置：

```powershell
cmake -S . -B build-debug
cmake -S . -B build
```

构建 Debug 和 Release：

```powershell
cmake --build build-debug --config Debug
cmake --build build --config Release
```

CMake 会在构建前编译 `shader/` 下登记的 Shader variant，并在构建后把 `shader/`、`textures/` 和 `models/` 复制到可执行文件目录。资源复制会保留源文件时间戳，并跳过没有变化的目标文件，避免普通重建使派生缓存失效。Windows 构建还会生成运行时控制工具。

```text
build-debug/Debug/VulkanLab.exe
build-debug/Debug/VulkanLabCtl.exe
build/Release/VulkanLab.exe
build/Release/VulkanLabCtl.exe
```

阶段三的资产工具接入构建后还会生成：

```text
build-debug/Debug/VulkanLabAssetTool.exe
build/Release/VulkanLabAssetTool.exe
```

## 启动

程序使用相对路径加载资源，应把对应输出目录设为工作目录：

```powershell
cd build-debug\Debug
.\VulkanLab.exe
```

CMake 会把 `vulkanlab_project.json` 写到 Debug/Release 可执行文件旁，IDE 或输出目录启动时会自动定位源码项目。也可以显式指定项目；Catalog 错误会在创建窗口和 Vulkan 前返回：

```powershell
.\VulkanLab.exe --project C:\Project\vulkan_learn
```

Runtime Control 默认关闭。需要从另一个终端控制运行中的渲染器时：

```powershell
.\VulkanLab.exe --runtime-control
```

查看启动参数不会初始化窗口或 Vulkan：

```powershell
.\VulkanLab.exe --help
```

未知参数会打印用法并返回非零退出码。控制工具的完整命令见 [Runtime Control](runtime_control.md)。

## 场景资产

场景由源码项目的 `assets/catalog.json` 注册，新增可选 glTF 不需要修改或重新编译 `main.cpp`。`Viking Room` 使用内建 factory；其余条目由项目相对 `source` 创建 glTF prepare factory。可选源文件缺失时仍显示为 `Unavailable`，但不阻止启动。

Scenes 面板提供 `Import Scene...`：选择 `.glb/.gltf` 后确认名称、稳定 scene ID、profile、Copy/Reference 和是否自动加载。`.gltf` 的本地 `.bin` 与图片依赖会一起复制到 `models/imported/<scene-id>/`；远程、缺失或逃逸依赖不会写入 Catalog。

同一导入事务也可通过 CLI 自动执行：

```powershell
.\VulkanLabAssetTool.exe catalog add `
  --project C:\Project\vulkan_learn `
  --source D:\Assets\Example\scene.gltf `
  --display-name "Example Scene" `
  --scene-id example-scene `
  --profile desktop_1024
```

Catalog 当前包含以下初始 glTF 条目：

- `A Beautiful Game`
- `Anisotropy Barn Lamp`
- `Car Concept`
- `Chronograph Watch`
- `Diffuse Transmission Teacup`
- `Pot of Coals`
- `Main Sponza`

`Main Sponza` 的入口是 `models/main_sponza/NewSponza_Main_glTF_003.gltf`。CMake 当前仍复制整个 `models/` 目录，大型本地资产会明显增加构建后的复制时间。

## KTX2 派生纹理缓存

原始 glTF、GLB、PNG 和 JPEG 不会被修改。`VulkanLabAssetTool` 显式生成独立的 KTX2 缓存。默认共享根目录为 `%LOCALAPPDATA%/VulkanLab/DerivedAssets/<projectId>`，由 ProjectContext 和 Catalog 统一解析，因此 Debug、Release 和资产工具不再各自生成工作目录缓存。

构建完成后，从 Debug 运行目录为 Main Sponza 生成 1024 和 2048 两个 profile：

```powershell
.\VulkanLabAssetTool.exe texture-cache build `
  --project C:\Project\vulkan_learn `
  --scene-id main-sponza `
  --profile desktop_1024

.\VulkanLabAssetTool.exe texture-cache build `
  --project C:\Project\vulkan_learn `
  --scene-id main-sponza `
  --profile desktop_2048
```

`--project` 可省略，此时资产工具与渲染器一样查找 executable locator 或当前目录祖先中的 Catalog。需要把缓存放到其他位置时可显式使用 `--cache-root`。`--scene models/... --texture-limit 1024` 旧参数仍可按 Catalog source/profile 匹配，但新脚本应使用稳定 ID。

缓存按 scene ID 和 profile ID 精确匹配。`desktop_1024` 不会复用 `desktop_2048` manifest。重新执行命令会复用有效 blob，`--force` 用于强制重新编码。工具必须完整成功后才发布新 manifest，失败不会修改源资产。

迁移旧运行目录缓存不会重新编码 KTX2：

```powershell
.\VulkanLabAssetTool.exe texture-cache migrate `
  --project C:\Project\vulkan_learn `
  --legacy-cache-root C:\Project\vulkan_learn\build-debug\Debug\derived_assets
```

使用输出目录启动渲染器时，locator 仍会选择相同的用户级共享缓存：

```powershell
cd build-debug\Debug
.\VulkanLab.exe --runtime-control
```

cache hit 时，worker 读取预生成 mip chain，并优先转码为 BC7；设备不支持 BC7 时转为 RGBA32。cache miss、manifest 过期、KTX2 损坏或 profile 不匹配时回退到现有 stb decode、CPU resize 和 GPU mip blit 路径，场景仍应可以加载。

可在日志、`Stats -> Last Scene Load` 或 Runtime Control 的加载统计中检查 cache lookup/hit/miss/invalid、KTX2 读取与转码耗时、BC7/RGBA32 数量、prebuilt mip 数量和实际上传字节。首次验证建议先加载 1024 profile，再与无缓存加载结果比较。

## 运行注意事项

- glTF 纹理尺寸默认限制为 `2048`，可在 Renderer 面板切换为 `Full`、`2048`、`1024` 或 `512`。切换会创建新的场景加载任务。
- glTF 解析、图片解码和 CPU 缩放在 worker 执行；GPU 创建和上传由主线程按帧推进。加载进度和取消操作位于 ImGui `Loading` 面板。
- GPU build 前会释放旧 Scene 以控制大场景切换时的显存峰值，因此该阶段可能只显示 ImGui 和空场景。
- `Full` 对 Main Sponza 仍是高风险选项。KTX2 缓存只覆盖已经显式生成且精确匹配的 profile；未命中时仍会回退 RGBA8。
- 日志写入运行目录下的 `logs/VulkanLab.log`。加载统计也显示在 ImGui 的 `Stats -> Last Scene Load`。
