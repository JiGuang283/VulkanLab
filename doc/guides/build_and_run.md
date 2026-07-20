# 构建与运行

> Status: Current
> Last verified: 2026-07-20
> Verified against: `a25f8ad`

## 环境要求

- Windows 10/11 和支持 Vulkan 的显卡驱动。
- Visual Studio 2022 C++ 工具链。
- CMake 3.22 或更高版本。
- Vulkan SDK。CMake 需要能找到 Vulkan、`glslc` 和 SDK 中的 GLM 头文件。
- 仓库内 `external/` 依赖完整，尤其是 `glfw/lib-vc2022`、ImGui、stb、VMA 和 glTF 头文件。
- 阶段三使用 KTX-Software v4.4.2 submodule。首次克隆或更新后必须递归初始化：

```powershell
git submodule update --init --recursive external/ktx
```

如果模型资产由 Git LFS 管理，还需要先安装 Git LFS 并在仓库根目录执行 `git lfs pull`。KTX2 派生缓存本身不提交到 Git LFS 或普通 Git。

推荐使用仓库 presets 配置、构建和测试：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-test

cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
```

Debug 和 Release 分别使用 `build/windows-msvc-debug/` 与 `build/windows-msvc-release/`。也可以继续使用自定义 build 目录，首次配置：

```powershell
cmake -S . -B build-debug
cmake -S . -B build
```

构建 Debug 和 Release：

```powershell
cmake --build build-debug --config Debug
cmake --build build --config Release
```

构建会生成 Git revision/dirty、configuration、compiler、Vulkan SDK 和 `glslc` 版本信息。启用 Runtime Control 后可通过 `VulkanLabCtl.exe --json info` 查看。确定性窗口、fixed delta、无 GUI 和诊断输出配置见 [诊断与自动化启动配置](diagnostics.md)。

CMake 将 `shader/` 下显式登记的 15 个 GLSL 源增量编译到 `build-*/generated/<Config>/shader/`，再把对应 SPIR-V stage 到可执行文件旁的 `shader/`。源码树不保存 SPIR-V，也没有独立的 `compile.bat`；普通 C++ rebuild 不会重新调用 `glslc`，修改一个 Shader 只更新对应产物。需要单独构建 Shader 时使用：

```powershell
cmake --build build-debug --config Debug --target VulkanLabShaders
```

开发构建只把 executable、运行时工具、`vulkanlab_project.json` locator 和生成的 SPIR-V 放入输出目录，不复制完整 `models/` 或 `textures/`。开发场景直接通过 ProjectContext 从源码项目读取源资产；Release 交付使用后文的 `cook` 命令生成经过校验的最小闭包。Windows 构建还会生成运行时控制工具。

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

程序不依赖当前工作目录。下面是从输出目录启动的常用方式：

```powershell
cd build-debug\Debug
.\VulkanLab.exe
```

CMake 会把 `vulkanlab_project.json` 写到 Debug/Release 可执行文件旁，因此从仓库根、输出目录或任意其他工作目录启动同一个 executable，都会定位到同一源码项目。也可以显式指定项目；Catalog 错误会在创建窗口和 Vulkan 前返回：

```powershell
.\VulkanLab.exe --project C:\Project\vulkan_learn
```

Runtime Control 默认关闭。需要从另一个终端控制运行中的渲染器时：

```powershell
.\VulkanLab.exe --runtime-control
```

并行运行或自动化时应为实例指定唯一 endpoint：

```powershell
.\VulkanLab.exe --runtime-control --runtime-control-pipe smoke_01
.\VulkanLabCtl.exe --pipe smoke_01 ping
```

确定性截图运行还可组合 `--automation --window-size 800x600 --fixed-delta 0.016666667 --no-gui --capture-root <path>`。完整命令、相机控制、稳定帧等待和异步截图流程见 [Runtime Control](runtime_control.md)与[诊断配置](diagnostics.md)。

派生资产运行模式通过启动参数显式选择，Debug/Release 默认都使用 `OnDemand`：

```powershell
.\VulkanLab.exe --asset-mode ondemand
.\VulkanLab.exe --asset-mode readonly
.\VulkanLab.exe --asset-mode cooked-only
```

- `ondemand`：缺失、过期或损坏的精确 scene/profile artifact 会在独立资产工具进程中自动重建。
- `readonly`：不启动编码进程；可在 UI 中显式选择 `Load Source Fallback`。
- `cooked-only`：不启动编码进程，也禁止 source fallback。手工开发运行可以显式选择该模式；正式 cooked package 会自动强制使用它。

测试或 CI 可用 `--cache-root <path>` 隔离派生缓存；正常开发运行省略该参数，使用项目级共享缓存。`--asset-tool <path>` 只用于覆盖与渲染器同目录的 `VulkanLabAssetTool.exe`。

查看启动参数不会初始化窗口或 Vulkan：

```powershell
.\VulkanLab.exe --help
```

未知参数会打印用法并返回非零退出码。控制工具的完整命令见 [Runtime Control](runtime_control.md)。

## 场景资产

场景由源码项目的 `assets/catalog.json` 注册，新增可选 glTF 不需要修改或重新编译 `main.cpp`。`Viking Room` 使用内建 factory；其余条目由项目相对 `source` 创建 glTF prepare factory。可选源文件缺失时仍显示为 `Unavailable`，但不阻止启动。

Scenes 面板提供搜索、单击选择、双击/`Load`、`Reimport`、显式 source fallback、保存当前相机和从 Catalog 移除条目。`Import Scene...` 选择 `.glb/.gltf` 后确认名称、稳定 scene ID、profile、Copy/Reference 和是否自动加载。`.gltf` 的本地 `.bin` 与图片依赖会一起复制到 `models/imported/<scene-id>/`；远程、缺失或逃逸依赖不会写入 Catalog。Catalog 注册成功后会自动提交 KTX2 import，勾选自动加载时再连续执行 CPU prepare 和 GPU upload。

`Assets` 面板显示当前项目、Catalog、cache root、运行模式、索引 Ready 记录数、cache/unreferenced blob 用量、选中 scene/profile 的 `Ready/Missing/Stale/Invalid/Importing` 状态、最近失败，以及资产任务的真实纹理进度、encoded/reused/failed、worker、耗时、日志和最近历史。长任务不会停留在 modal 中，面板不会逐帧扫描全部 manifest。

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

`Main Sponza` 的入口是 `models/main_sponza/NewSponza_Main_glTF_003.gltf`。该路径从 `projectRoot` 解析；大型本地资产不会在普通构建后被复制到 Debug/Release 输出目录。

## KTX2 派生纹理缓存

原始 glTF、GLB、PNG 和 JPEG 不会被修改。`VulkanLabAssetTool` 生成独立的 KTX2 缓存；既可显式调用，也可由 OnDemand admission 监督调用。默认共享根目录为 `%LOCALAPPDATA%/VulkanLab/DerivedAssets/<projectId>`，由 ProjectContext 和 Catalog 统一解析，因此 Debug、Release 和资产工具不再各自生成工作目录缓存。

构建完成后，从 Debug 运行目录为 Main Sponza 生成 1024 和 2048 两个 profile：

```powershell
.\VulkanLabAssetTool.exe texture-cache build `
  --project C:\Project\vulkan_learn `
  --scene-id main-sponza `
  --profile desktop_1024 `
  --workers 4 `
  --memory-budget-mib 2048

.\VulkanLabAssetTool.exe texture-cache build `
  --project C:\Project\vulkan_learn `
  --scene-id main-sponza `
  --profile desktop_2048
```

与运行时自动导入等价的稳定入口是：

```powershell
.\VulkanLabAssetTool.exe import scene `
  --project C:\Project\vulkan_learn `
  --scene-id main-sponza `
  --profile desktop_1024 `
  --progress ndjson
```

`--project` 可省略，此时资产工具与渲染器一样查找 executable locator 或当前目录祖先中的 Catalog。需要把缓存放到其他位置时可显式使用 `--cache-root`。`--scene models/... --texture-limit 1024` 旧参数仍可按 Catalog source/profile 匹配，但新脚本应使用稳定 ID。

缓存按 scene ID 和 profile ID 精确匹配。`desktop_1024` 不会复用 `desktop_2048` manifest。重新执行命令会复用有效 blob，`--force` 用于强制重新编码。工具必须完整成功后才发布新 manifest，失败不会修改源资产。

并行导入参数：

- `--workers N`：编码子进程上限；默认取逻辑 CPU 一半并限制到 4。
- `--memory-budget-mib N`：估算编码工作集预算，默认 2048 MiB；预算优先于 worker 上限。
- `--preset development|production`：显式覆盖 profile preset。development 保持兼容参数，production 使用更高 quality/Zstd 并生成不同 cache key。
- `--progress ndjson` 或 `--progress-json`：stdout 只输出机器可读 NDJSON，普通诊断与 `ktx` 输出写 stderr。

按 Ctrl+C 会取消整个 Job Object 中的 `ktx.exe`。已完成 blob 可在下次命令中复用，但取消时不会发布 scene manifest；命令正常取消返回 `130`。自动化脚本应以最终 `completed`、`failed` 或 `cancelled` 事件作为结果，不根据 artifact 完成数量推断 manifest 已发布。

Stage B 的 Release/Main Sponza 1024 基准（2026-07-19，同一机器、clean 独立 cache root）为：单 worker `261.19 s`，四 worker `117.15 s`，约 `2.23x` 加速；两次生成的 72-entry manifest SHA-256 完全一致。cache hit 为 `encoded=0/reused=72`，约 `3.11 s`。这些数字用于回归趋势，不是跨机器性能门槛。

迁移旧运行目录缓存不会重新编码 KTX2：

```powershell
.\VulkanLabAssetTool.exe texture-cache migrate `
  --project C:\Project\vulkan_learn `
  --legacy-cache-root C:\Project\vulkan_learn\build-debug\Debug\derived_assets
```

索引可以删除后自动重建，也可以显式维护：

```powershell
.\VulkanLabAssetTool.exe cache index rebuild `
  --project C:\Project\vulkan_learn

.\VulkanLabAssetTool.exe cache prune `
  --project C:\Project\vulkan_learn `
  --older-than-days 7
```

`cache prune` 默认是 dry-run，只列出候选。确认列表后才使用 `--execute`；执行时会与 import/migration 互斥，并在锁内重新计算保护闭包。任何已发布 manifest 引用的 blob、正在导入的 blob 和保留期内的孤立 blob 都不会删除。若任一 manifest 损坏或包含非法 blob 路径，命令会 fail closed 并保持 cache 不变。测试隔离 cache 可以用 `--older-than-days 0 --execute` 立即删除全部无引用 blob，正常共享 cache 不建议这样使用。

使用输出目录启动渲染器时，locator 仍会选择相同的用户级共享缓存：

```powershell
cd build-debug\Debug
.\VulkanLab.exe --runtime-control
```

cache hit 时，worker 读取预生成 mip chain，并优先转码为 BC7；设备不支持 BC7 时转为 RGBA32。场景加载前的 admission 会检查 manifest 身份、source stamp、blob 存在性和 KTX2 header。OnDemand 下的 Missing/Stale/Invalid 会先异步重建，失败时保留当前 Scene；只有用户显式选择 source fallback 才走 stb decode、CPU resize 和 GPU mip blit。ReadOnly/CookedOnly 不会静默启动编码器。

可在日志、`Stats -> Last Scene Load` 或 Runtime Control 的加载统计中检查 cache lookup/hit/miss/invalid、KTX2 读取与转码耗时、BC7/RGBA32 数量、prebuilt mip 数量和实际上传字节。首次验证建议先加载 1024 profile，再与无缓存加载结果比较。

## Cook 与独立运行包

先构建 Release，并确保目标 scene/profile 的派生纹理处于 Ready。下面的命令只发布 Main Sponza 1024：

```powershell
cmake --build build-release --config Release

.\build-release\Release\VulkanLabAssetTool.exe cook `
  --project . `
  --runtime-dir .\build-release\Release `
  --output .\dist\main-sponza `
  --platform windows-x64 `
  --profile desktop_1024 `
  --scene-id main-sponza
```

可以重复传入 `--scene-id` 选择多个场景。完全省略时会选择 Catalog 中 `optional=false` 的场景。默认要求 artifact 已经 Ready；需要在 cook 前自动补建时显式增加 `--build-missing`，也可以用 `--workers` 和 `--memory-budget-mib` 控制编码器。省略 `--cache-root` 时使用项目共享缓存。

交付前使用同一套 hash 校验：

```powershell
.\build-release\Release\VulkanLabAssetTool.exe package verify `
  --path .\dist\main-sponza
```

典型输出布局为：

```text
dist/main-sponza/
  VulkanLab.exe
  VulkanLabCtl.exe
  package_manifest.json
  assets/catalog.json
  shader/...                         # 仅 kShaderVariants 使用的 SPIR-V
  models/...                         # glTF/GLB 与必要 buffer，不含源图片
  runtime_assets/artifact_index.json
  runtime_assets/manifests/...
  runtime_assets/blobs/*.ktx2
```

Cook 先在输出目录旁构建 staging，验证 package manifest 后才原子替换旧包。输出目录不能与 runtime 或 cache 相互包含，也不能包含项目根目录。失败不会发布 staging，也不会破坏已经存在的包。

包内运行不需要源码 locator 或用户级 derived cache：

```powershell
cd .\dist\main-sponza
.\VulkanLab.exe --runtime-control
```

程序从可执行文件旁发现 `package_manifest.json`，在创建窗口/Vulkan 前校验所有列出文件的大小和 SHA-256，然后使用包内只读 Catalog 与 `runtime_assets`。此时自动强制 `CookedOnly`，纹理限制固定为 package profile，`--project`、`--cache-root`、`--asset-tool`、非 cooked asset mode 和运行时纹理档位修改都会被拒绝。KTX2 manifest、entry、blob、转码或 mip 数据出错会使加载失败，不会读取 PNG/JPEG fallback。Cooked package 也关闭开发 validation layer 依赖；目标机器仍需 Vulkan 显卡驱动和 MSVC Release Runtime。

`package_manifest.json` 包含 `VulkanLab.exe` 本身的 hash。若发布流程还要进行代码签名，应先完成签名，再重新生成 package manifest；当前 `cook` 命令假定输入 executable 已经是最终字节。

Stage F gate 的当前 Release/Main Sponza 1024 基线是总加载约 `1.36 s`、KTX2 read `167 ms`、BC7 transcode `787 ms`、纹理 GPU estimate `96 MiB` 和场景 VMA allocation delta `279.74 MiB`。这些结果暂不支持引入 platform-final BC payload 或 streaming；量化重开条件见 [资源加载](../architecture/resource_loading.md#platform-artifact-与-residency-决策)。

## 运行注意事项

- 开发模式的 glTF 纹理尺寸默认限制为 `2048`，可在 Renderer 面板切换为 `Full`、`2048`、`1024` 或 `512`。切换会创建新的场景加载任务；CookedOnly 控件禁用。
- 开发模式下，KTX2 编码由 `AssetImportManager` 监督的独立 `VulkanLabAssetTool` 进程执行；glTF prepare 在 SceneLoadManager worker 执行；GPU 创建和上传由主线程按帧推进。CookedOnly 不创建 AssetImportManager。资产与场景加载分别显示在 `Assets` 和 `Loading` 面板。
- GPU build 前会释放旧 Scene 以控制大场景切换时的显存峰值，因此该阶段可能只显示 ImGui 和空场景。
- `Full` 对 Main Sponza 仍是高风险选项。开发模式的 KTX2 缓存只覆盖已经显式生成且精确匹配的 profile；未命中时仍可能回退 RGBA8。
- 日志写入运行目录下的 `logs/VulkanLab.log`。加载统计也显示在 ImGui 的 `Stats -> Last Scene Load`。
