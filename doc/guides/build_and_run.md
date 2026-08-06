# 构建与运行

> Status: Current
> Last verified: 2026-08-06
> Verified against: optional FidelityFX CACAO comparison integration

## 环境要求

- Windows 10/11 和支持 Vulkan 的显卡驱动。
- Visual Studio 2022 C++ 工具链。
- CMake 3.22 或更高版本。
- Vulkan SDK。CMake 需要能找到 Vulkan、`glslc`、`spirv-val` 和 SDK 中的 GLM 头文件。
- 仓库内 `external/` 依赖完整，尤其是 `glfw/lib-vc2022`、ImGui、stb、VMA 和 glTF 头文件。
- 运行时使用 KTX-Software v4.4.2，AssetTool 使用 DirectXTex `may2026` 离线压缩 BC7，shader contract tests 使用固定提交的 SPIRV-Reflect，Tracy 专用构建使用 v0.13.1；Editor 使用固定提交 `5ab7676` 的 ImGuizmo，AO comparison 构建使用 FidelityFX CACAO v1.2。它们都是 submodule。首次克隆或更新后必须递归初始化：

```powershell
git submodule update --init --recursive
```

如果模型资产由 Git LFS 管理，还需要先安装 Git LFS 并在仓库根目录执行 `git lfs pull`。KTX2 派生缓存本身不提交到 Git LFS 或普通 Git。

## 构建配置

Windows MSVC 提供六个主要 configure/build preset：

| Preset | 配置 | 用途 | 主要产物 |
|---|---|---|---|
| `windows-msvc-debug` | Debug，全功能，`BUILD_TESTING=ON` | 完整开发与诊断 | VulkanLab、AssetTool、Ctl、RenderTest 和测试目标 |
| `windows-msvc-release` | Release，全功能，`BUILD_TESTING=ON` | 完整 Release 验证与 Cook 输入 | 与 Debug 相同的功能和工具 |
| `windows-msvc-dev-fast` | Debug，全运行时功能，`BUILD_TESTING=OFF` | 日常快速迭代 | VulkanLab 和 AssetTool |
| `windows-msvc-ao-compare` | Debug，基于 dev-fast，CACAO ON | SSAO/CACAO 质量与性能对比 | VulkanLab、AssetTool 和 Ctl |
| `windows-msvc-tracy` | Debug，全运行时功能，Tracy ON，`BUILD_TESTING=OFF` | CPU/Vulkan GPU 深度性能分析 | VulkanLab、AssetTool 和 Ctl |
| `windows-msvc-runtime` | Release，开发基础设施全部关闭 | 精简运行时 | 仅 VulkanLab |

日常修改渲染器时优先使用快速配置：

```powershell
cmake --preset windows-msvc-dev-fast
cmake --build --preset windows-msvc-dev-fast
```

需要统一 CPU/GPU 时间线时使用专用配置：

```powershell
cmake --preset windows-msvc-tracy
cmake --build --preset windows-msvc-tracy
```

Profiler 安装、连接和 capture 工作流见 [Tracy 性能分析](tracy_profiling.md)。

需要比较内置 SSAO 与 FidelityFX CACAO 时使用专用配置。该配置额外要求 Vulkan SDK 中的 `dxc`，并会生成、验证上游 CACAO SPIR-V：

```powershell
cmake --preset windows-msvc-ao-compare
cmake --build --preset windows-msvc-ao-compare
```

构建精简运行时：

```powershell
cmake --preset windows-msvc-runtime
cmake --build --preset windows-msvc-runtime
```

`windows-msvc-runtime` 仍保留 Forward、Directional Shadow、HDR/Tone Mapping、IBL、Skybox、场景加载和 KTX2 读取。它只裁剪编辑器、Runtime Control、截图、资产写入、Validation、GPU Debug Utils、GPU timestamp profiling 和 Tracy，不改变 Shader ABI、材质布局或 descriptor layout。

## 编译期功能开关

除 `VKL_ENABLE_TRACY` 与 `VKL_ENABLE_CACAO` 默认为 `OFF` 外，现有模块和工具选项默认均为 `ON`，可在自定义 CMake 配置中独立设置：

| Option | 控制内容 |
|---|---|
| `VKL_ENABLE_EDITOR_UI` | Dear ImGui 编辑器工作区及其依赖 |
| `VKL_ENABLE_RUNTIME_CONTROL` | VulkanLab 内的 Named Pipe 服务端 |
| `VKL_ENABLE_CAPTURE` | 截图服务和 swapchain `TRANSFER_SRC` usage |
| `VKL_ENABLE_ASSET_AUTHORING` | OnDemand import、资产工具进程监督和 Catalog 写操作 |
| `VKL_ENABLE_VALIDATION` | Validation profile 集成 |
| `VKL_ENABLE_GPU_DEBUG_UTILS` | Vulkan 对象命名和 GPU command labels |
| `VKL_ENABLE_GPU_PROFILING` | 每 Pass timestamp query |
| `VKL_ENABLE_TRACY` | Tracy CPU 与 Vulkan GPU 统一时间线；仅建议专用开发配置启用 |
| `VKL_ENABLE_CACAO` | 固定版本 FidelityFX CACAO comparison backend；只建议 AO 对比配置启用 |
| `VKL_BUILD_ASSET_TOOL` | `VulkanLabAssetTool.exe` |
| `VKL_BUILD_CONTROL_TOOL` | `VulkanLabCtl.exe` |
| `VKL_BUILD_RENDER_TEST` | `VulkanLabRenderTest.exe` |
| `BUILD_TESTING` | CTest 目标和 SPIRV-Reflect 测试依赖 |

依赖规则：

- RenderTest 要求 Runtime Control 和 Capture 同时编译，否则 CMake 配置失败。
- Runtime Control 服务端与 VulkanLabCtl 客户端可以独立构建。
- Asset Authoring 可使用外部 `--asset-tool`；启用 authoring 但不构建本地 AssetTool 时 CMake 给出 warning。
- 关闭 AssetTool 会同时关闭 DirectXTex、KTX CLI 与 KTX1，只保留渲染器读取 KTX2 所需的 `ktx_read`。
- `BUILD_TESTING=OFF` 时不构建 SPIRV-Reflect。
- Tracy 与现有 GPU timestamp profiler 相互独立；`VKL_ENABLE_TRACY=OFF` 时不配置 Tracy submodule，也不链接 TracyClient。
- `VKL_ENABLE_CACAO=OFF` 时不生成上游 shader、不编译或链接 CACAO SDK；内置 SSAO 与 Screen-Space ABI 保持可用。

CMake 通过 `configure_file()` 生成 `BuildFeatures.h`。宏仅用于程序入口、模块装配和真实/空实现选择，不用于控制 IBL、Shadow 或任何 GPU ABI。`VulkanLab.exe --help`、启动日志、BuildInfo 和 Runtime Control 的 `system.info.build.features` 都会报告实际编译能力。

未编译功能的启动参数不会被静默忽略。`--runtime-control`、`--runtime-control-pipe`、`--capture-root`、`--asset-mode ondemand`、`--asset-tool` 和非 `off` Validation profile 会在创建 Window/Vulkan 前返回明确错误。Editor 未编译时 `--no-gui` 仍被接受；Asset Authoring 未编译时默认模式为 `ReadOnly`。Runtime Control 已编译但某个子功能被裁剪时，对应协议方法返回 `feature_not_compiled`。

推荐使用仓库 presets 配置、构建和测试：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-test

cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --test-dir build/windows-msvc-release -C Release --output-on-failure
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

CMake 将 `shader/` 下由 Manifest 引用的 24 个 GLSL 源增量编译到 `build-*/generated/<Config>/shader/`，每个产物通过 `spirv-val` 后再 stage 到可执行文件旁的 `shader/`。共享 ABI include 会作为依赖触发相关 shader 重编译。源码树不保存 SPIR-V，也没有独立的 `compile.bat`；普通 C++ rebuild 不会重新调用 `glslc`，修改一个 Shader 只更新对应产物。需要单独构建 Shader 时使用：

```powershell
cmake --build build-debug --config Debug --target VulkanLabShaders
```

启用 `BUILD_TESTING` 时，`VulkanLabCpuTests` 会静态链接 SPIRV-Reflect 并读取上述实际 SPIR-V，校验 shader stage、descriptor、共享 UBO/push ABI、vertex input、varying 和 fragment output。SPIRV-Reflect 不链接进 `VulkanLab.exe`。

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

开发构建还会生成独立视觉测试程序，它不进入 Cook package：

```text
build/windows-msvc-debug/Debug/VulkanLabRenderTest.exe
build/windows-msvc-release/Release/VulkanLabRenderTest.exe
```

需要 GPU 和可呈现窗口的 smoke/golden 测试使用 `visual` 标签；纯 CPU、资产和 package 测试使用 `unit` 标签：

```powershell
ctest --preset windows-msvc-test -L unit --output-on-failure
ctest --preset windows-msvc-test -L visual --output-on-failure
```

规格、结果目录、错误码和基线审核流程见[自动视觉回归](visual_regression.md)。

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

Vulkan Validation 默认使用 `core`，也可以显式选择 `off/core/sync/gpu`：

```powershell
.\VulkanLab.exe --validation sync
```

`off` 不会关闭非 Cooked 构建中的 RenderDoc 标签。各 profile、回退规则和抓帧方式见 [RenderDoc 与 Vulkan Validation](renderdoc_validation.md)。

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

## glTF Validator

新 glTF/GLB 导入使用 Khronos glTF Validator `2.0.0-dev.3.10` 作为规范门禁。固定版本安装到 Git 忽略目录：

```powershell
.\tools\setup\Install-GltfValidator.ps1
```

脚本下载官方 Win64 zip，校验 SHA-256 `c5068f51205deedc28acc3529ee7e11ee60e853454f673093398eba80142202c`，并安装到 `external/tools/gltf-validator/2.0.0-dev.3.10/`。CMake 发现后会把 executable、LICENSE 和 NOTICES stage 到 `VulkanLabAssetTool.exe` 旁。也可以在启动或资产命令中显式覆盖：

```powershell
.\VulkanLab.exe --gltf-validator D:\Tools\gltf_validator.exe
.\VulkanLabAssetTool.exe validate scene `
  --project C:\Project\vulkan_learn `
  --model-id sheen-chair `
  --gltf-validator D:\Tools\gltf_validator.exe
```

工具发现顺序为显式参数、AssetTool 同目录、`PATH`，只接受精确版本。报告默认写入 `%LOCALAPPDATA%/VulkanLab/DerivedAssets/<projectId>/validation/`；`--cache-root` 可覆盖。`Valid` 和 `Warnings` 允许导入，`Invalid` 不能绕过；工具不可用时开发 UI 只有在显式勾选 `Import without validation` 后才允许继续。Cook 必须能发现正确版本，并要求全部选中 glTF scene 没有 Error。

```powershell
.\VulkanLabAssetTool.exe validate scene `
  --project C:\Project\vulkan_learn `
  --source D:\Assets\Example\scene.glb

.\VulkanLabAssetTool.exe validate scene `
  --project C:\Project\vulkan_learn `
  --model-id main-sponza `
  --force
```

## 模型预览资产

模型由源码项目的 `assets/catalog.json` 注册，新增可选 glTF 不需要修改或重新编译 `main.cpp`。`Viking Room` 使用内建 factory；其余 model 条目由项目相对 `source` 创建单模型预览 factory。可选源文件缺失时仍显示为 `Unavailable`，但不阻止启动。

`VulkanLab -> Scene -> Scenes` 显示 `Model Previews`，提供搜索、单击选择、双击/`Load`、`Reimport`、显式 source fallback、保存预览相机和从 Catalog 移除 model。`Import Model...` 选择 `.glb/.gltf` 后先执行本地依赖安全检查和 Validator，再显示名称、稳定 model ID、profile、Copy/Reference、验证 issues/扩展兼容性和是否自动加载。`.gltf` 的本地 `.bin` 与图片依赖会一起复制到 `models/imported/<model-id>/`；远程、缺失、逃逸依赖或 Validator Error 不会写入 Catalog。Catalog 注册成功后会自动提交 Native BC7 import，勾选自动加载时再连续执行 CPU prepare 和 GPU upload。已有 Catalog glTF 可在模型详情中按需 `Validate/Revalidate` 和打开完整报告。

`VulkanLab -> Scene -> Assets` 显示当前项目、Catalog、cache root、运行模式、索引 Ready 记录数、cache/unreferenced blob 用量、选中 model/profile 的 `Ready/Missing/Stale/Invalid/Importing` 状态、最近失败，以及资产任务的真实纹理进度、encoded/reused/failed、worker、耗时、日志和最近历史。长任务不会停留在 modal 中，页面不会逐帧扫描全部 manifest。

同一导入事务也可通过 CLI 自动执行：

```powershell
.\VulkanLabAssetTool.exe catalog add `
  --project C:\Project\vulkan_learn `
  --source D:\Assets\Example\scene.gltf `
  --display-name "Example Model" `
  --model-id example-model `
  --profile desktop_1024
```

Catalog 当前包含以下初始 glTF model 条目：

- `A Beautiful Game`
- `Anisotropy Barn Lamp`
- `Car Concept`
- `Chronograph Watch`
- `Diffuse Transmission Teacup`
- `Pot of Coals`
- `Main Sponza`

`Main Sponza` 的入口是 `models/main_sponza/NewSponza_Main_glTF_003.gltf`。该路径从 `projectRoot` 解析；大型本地资产不会在普通构建后被复制到 Debug/Release 输出目录。

## KTX2 派生纹理缓存

原始 glTF、GLB、PNG 和 JPEG 不会被修改。`VulkanLabAssetTool` 生成独立的 KTX2 缓存；既可显式调用，也可由 OnDemand admission 监督调用。当前 `desktop_512/1024/2048/full` profiles 使用 Native BC7：KTX-Software 负责缩放、Lanczos4 mipmap、wrap 和 normal normalize，DirectXTex 负责逐 mip BC7 压缩，libktx 负责写入 KTX2。默认共享根目录为 `%LOCALAPPDATA%/VulkanLab/DerivedAssets/<projectId>`，由 ProjectContext 和 Catalog 统一解析，因此 Debug、Release 和资产工具不再各自生成工作目录缓存。

构建完成后，从 Debug 运行目录为 Main Sponza 生成 1024 和 2048 两个 profile：

```powershell
.\VulkanLabAssetTool.exe texture-cache build `
  --project C:\Project\vulkan_learn `
  --model-id main-sponza `
  --profile desktop_1024 `
  --workers 4 `
  --memory-budget-mib 2048

.\VulkanLabAssetTool.exe texture-cache build `
  --project C:\Project\vulkan_learn `
  --model-id main-sponza `
  --profile desktop_2048
```

与运行时自动导入等价的稳定入口是：

```powershell
.\VulkanLabAssetTool.exe import scene `
  --project C:\Project\vulkan_learn `
  --model-id main-sponza `
  --profile desktop_1024 `
  --progress ndjson
```

`--project` 可省略，此时资产工具与渲染器一样查找 executable locator 或当前目录祖先中的 Catalog。需要把缓存放到其他位置时可显式使用 `--cache-root`。`--scene models/... --texture-limit 1024` 旧参数仍可按 Catalog source/profile 匹配，但新脚本应使用稳定 ID。

缓存按 model ID 和 profile ID 精确匹配。现有 manifest 内仍使用兼容字段 `sceneId`，字符串值不变，因此 Catalog v3 不会使缓存失效。`desktop_1024` 不会复用 `desktop_2048` manifest。重新执行命令会复用有效 blob，`--force` 用于强制重新编码。工具必须完整成功后才发布新 manifest，失败不会修改源资产。

并行导入参数：

- `--workers N`：编码子进程上限；默认取逻辑 CPU 一半并限制到 4。
- `--memory-budget-mib N`：估算编码工作集预算，默认 2048 MiB；预算优先于 worker 上限。
- `--preset development|production`：显式覆盖 profile preset。Native BC7 的 development 使用 quick 模式，production 使用完整搜索；v1 都不使用 Zstd，并生成不同 cache key。
- `--progress ndjson` 或 `--progress-json`：stdout 只输出机器可读 NDJSON，普通诊断与 `ktx` 输出写 stderr。

按 Ctrl+C 会取消整个 Job Object 中的 `ktx.exe`。已完成 blob 可在下次命令中复用，但取消时不会发布 scene manifest；命令正常取消返回 `130`。自动化脚本应以最终 `completed`、`failed` 或 `cancelled` 事件作为结果，不根据 artifact 完成数量推断 manifest 已发布。

首次 Native BC7 构建是离线 CPU 压缩任务，可能比旧 UASTC import 更慢；优先使用 `--workers 4`，并按可用内存设置 `--memory-budget-mib`。只有全部纹理成功后才发布 schema v3 manifest；已完成但未被 manifest 引用的 blob 可由 `cache prune` 清理。后续重复构建会复用匹配 encoder/version/quality/source/semantic/size/wrap 的 blob。

旧 schema v1/v2 UASTC 缓存仍可读取，但当前 BC7 profile 会将它标记为 `Stale` 并要求重建，不能伪装成 Ready。需要保留 portable UASTC 时应使用显式 `textureEncoder: "uastc"` 的自定义 profile。

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

`cache prune` 默认是 dry-run，只列出候选。确认列表后才使用 `--execute`；执行时会与 import/migration/validation 互斥，并在锁内重新计算保护闭包。任何已发布 scene/environment manifest 引用的 blob、Catalog validation index 引用的报告、正在导入的 blob 和保留期内的孤立文件都不会删除。Catalog 删除只解除 validation report 绑定，报告经过保留期后由这里清理。若任一 manifest 损坏或包含非法 blob 路径，命令会 fail closed 并保持 cache 不变。测试隔离 cache 可以用 `--older-than-days 0 --execute` 立即删除全部无引用 blob 和 validation report，正常共享 cache 不建议这样使用。

使用输出目录启动渲染器时，locator 仍会选择相同的用户级共享缓存：

```powershell
cd build-debug\Debug
.\VulkanLab.exe --runtime-control
```

Native BC7 cache hit 时，worker 读取完整 mip chain 后直接交给 staging/upload，不执行 Basis 转码、stb decode、CPU resize 或 GPU mip blit。旧 UASTC cache 仍使用 BC7/RGBA32 转码兼容路径。场景加载前的 admission 会检查 manifest schema/encoder、source stamp、blob 大小和 KTX2 header。OnDemand 下的 Missing/Stale/Invalid 会先异步重建，失败时保留当前 Scene；只有用户显式选择 source fallback 才走 RGBA8 路径。ReadOnly/CookedOnly 不会静默启动编码器。

可在日志、`VulkanLab -> Diagnostics -> Load Stats` 或 Runtime Control 的加载统计中检查 `nativeBc7CacheHits`、`basisUastcCacheHits`、`basisTranscodeCount`、Native/KTX2 读取字节与耗时、转码耗时、prebuilt mip 数量和实际上传字节。Native BC7 正常命中时 `basisTranscodeCount=0` 且 `derivedTextureTranscodeMs` 接近 0。

## HDR 环境与 IBL 派生缓存

环境光照不在运行时卷积源 HDR。Catalog schema v3 使用稳定 environment ID 和 `EnvironmentProfile` 管理本地 `.hdr`；schema v1 Catalog 仍可读取，并获得内建的 `ibl_desktop_v1` profile。当前 profile 固定输出：

- 512 cubemap Radiance，`RGBA16F`，完整普通 mip chain。
- 32 cubemap Irradiance，`RGBA16F`，单 mip，已包含 diffuse convolution 并除以 π。
- 256 cubemap Prefiltered Specular，`RGBA16F`，完整 roughness mip chain。
- 256x256 BRDF LUT，`RG16F`。

通过 UI 导入时，打开 `VulkanLab -> Scene -> Assets`，点击 `Import HDR`，选择项目本地或外部的 2:1 equirectangular `.hdr`。导入器把文件复制到 `assets/environments/<environment-id>/` 并写入 Catalog；同名 ID 或目标文件已存在时会拒绝覆盖。随后在同一页面点击 `Build` 或 `Rebuild` 生成派生资源。

等价的 CLI 流程为：

```powershell
.\VulkanLabAssetTool.exe catalog add-environment `
  --project C:\Project\vulkan_learn `
  --source D:\Assets\HDR\studio.hdr `
  --display-name "Studio" `
  --environment-id studio `
  --profile ibl_desktop_v1

.\VulkanLabAssetTool.exe environment-cache build `
  --project C:\Project\vulkan_learn `
  --environment-id studio `
  --profile ibl_desktop_v1 `
  --workers 4
```

首次构建是确定性的 CPU bake，固定使用 Hammersley 序列生成 diffuse irradiance、GGX prefilter 和 split-sum BRDF LUT，计算量明显高于普通纹理导入。有效 manifest 与四个 blob 已存在时会直接复用，不再解码 HDR 或重复积分；`--force` 才会强制重建。Ctrl+C 可取消构建，只有四个输出全部验证成功后才原子发布新 manifest。

环境缓存与 scene 纹理共享 cache root 和内容寻址 blob：

```text
DerivedAssets/<projectId>/
  manifests/environments/<environment-id>/<profile-id>.json
  blobs/<content-cache-key>.ktx2
  artifact_index.json
```

在 `VulkanLab -> Render -> Lighting` 选择环境，再分别开启 `Image-Based Lighting` 和 `Skybox`。二者默认关闭，选择环境本身不会隐式打开开关。环境在 worker 读取 KTX2，并通过增量上传队列分帧发布；切换失败或取消时旧环境保持有效。设备不支持 float cubemap/LUT 线性采样时只禁用 IBL，原有 constant ambient 路径仍可使用。

## 基础几何体

打开一个可写 Native Scene 后，可以通过 Outliner 的 `Create -> 3D Object`
创建 Plane、Cube、Sphere、Cylinder、Cone 和 Capsule，也可以从 Models 页的
`Engine Primitives` 区域拖入 Viewport。它们使用普通 ModelInstance，因此支持
现有的 Picking、Gizmo、父子层级、Undo/Redo、阴影和场景保存。

基础几何体由运行时生成，不需要下载或导入模型，也不会出现在项目 Catalog
中。默认尺寸为一米量级并使用 Z-up；通过 Entity Transform 调整尺寸。它们使用
固定的 `engine-primitive-v1` Repository profile 和中性 PBR 材质，同类型实例
共享 GPU 资源。Cook 时这些模型是零文件依赖，不要求 Validator、Native BC7
缓存或源资源。

## Cook 与独立运行包

Stage 7 的 Cook 只接受 Native SceneDocument 作为发布根，不再接受模型预览。先构建精简 Release runtime 和开发 AssetTool，并确保场景引用模型的 Validator 报告、Native BC7 纹理和环境 KTX2 都处于 Ready：

```powershell
cmake --preset windows-msvc-runtime
cmake --build build/windows-msvc-runtime --config Release --target VulkanLab

cmake --preset windows-msvc-dev-fast
cmake --build build/windows-msvc-dev-fast --config Debug `
  --target VulkanLabAssetTool

.\build\windows-msvc-dev-fast\Debug\VulkanLabAssetTool.exe cook `
  --project . `
  --runtime-dir .\build\windows-msvc-runtime\Release `
  --output .\dist\my-scene `
  --scene-id my-scene `
  --startup-scene my-scene `
  --build-missing
```

可以重复传入 `--scene-id`，一个包可包含多个 Native Scene。`--startup-scene` 必须属于已选场景；省略时使用 Catalog 顺序中的第一个已选场景。完全省略 `--scene-id` 时选择所有 `optional=false` 的 Native Scene。模型、import profile 和环境均从 SceneDocument/Catalog 闭包推导；Cook 中使用 `--model-id`、`--environment-id` 或 `--profile` 会返回迁移错误。

默认要求全部 artifact 已经 Ready。`--build-missing` 会按唯一 `(modelId, profileId)` 和 environment/profile 补建；`--workers` 与 `--memory-budget-mib` 控制离线构建。每个模型使用自身 Catalog `importProfile`，Windows 包只接受实际使用的 Native BC7 profile。Cook 还要求精确版本 glTF Validator 的结果为 Valid 或 Warnings；规范 Error 不可绕过。

Cook 会调用目标 `VulkanLab.exe --build-info-json`。目标必须是 `windows-msvc-runtime` 的 Release 产物，Editor、Runtime Control、Capture、Asset Authoring、Validation、Debug Utils、GPU Profiler 和 Tracy 必须全部未编译。全功能 Debug/Release 不能作为发布 runtime。

交付前使用同一套 hash 校验：

```powershell
.\build\windows-msvc-dev-fast\Debug\VulkanLabAssetTool.exe package verify `
  --path .\dist\my-scene
```

典型输出布局为：

```text
dist/my-scene/
  VulkanLab.exe
  package_manifest.json
  assets/catalog.json
  assets/scenes/*.vkscene.json
  shader/manifest.json               # Shader program/variant 权威清单
  shader/...                         # Manifest 实际引用的唯一 SPIR-V
  models/...                         # glTF/GLB 与必要 buffer，不含源图片
  runtime_assets/artifact_index.json
  runtime_assets/manifests/...       # model 和 environment manifests
  runtime_assets/blobs/*.ktx2
```

Cooked Catalog 只保留选中的 SceneDocument、其引用的唯一 Models/Environments 和必要 profiles，不注册 Model Preview。重复 ModelInstance 不复制模型或 GPU 资源；内容寻址 blob 跨模型去重。环境闭包只包含派生 KTX2 与 manifest，不包含源 HDR。包内也不包含 PNG/JPEG、AssetTool、VulkanLabCtl、编辑器、文档或 shader source。

Cook 先在输出目录旁构建 staging。发布前会复核 Catalog 与 SceneDocument 未在 Cook 期间变化，并验证 schema v3 package manifest、最小 Catalog、SceneDocument 引用、Artifact Index、KTX2 metadata、shader closure、所有文件大小和 SHA-256；成功后才原子替换旧包。失败会删除 staging 并保留旧包。

包内运行不需要源码 locator 或用户级 derived cache：

```powershell
cd .\dist\my-scene
.\VulkanLab.exe
```

程序从可执行文件旁发现 `package_manifest.json`，在创建窗口/Vulkan 前完成相同验证，然后只注册 Native Scene，并异步加载 `startupSceneId`。此时自动强制 `CookedOnly`；每个模型使用包内 Catalog profile，环境使用 SceneDocument 引用。`--project`、`--cache-root`、`--asset-tool` 和非 cooked asset mode 都会被拒绝。`windows-msvc-runtime` 不包含 Runtime Control，因此 `--runtime-control` 也会在 Vulkan 初始化前明确报错。

目标设备不支持 BC7 sampled、linear filtering 和 transfer destination 时返回 `bc7_required`。KTX2 manifest、entry、blob、format、payload 或 mip 数据出错会使加载失败，不会读取 PNG/JPEG fallback。目标机器仍需 Vulkan 显卡驱动和 MSVC Release Runtime。旧 schema v1/v2 单模型包仍可读取，并继续走 Model Preview 兼容路径。

`package_manifest.json` 包含 `VulkanLab.exe` 本身的 hash。若发布流程还要进行代码签名，应先完成签名，再重新生成 package manifest；当前 `cook` 命令假定输入 executable 已经是最终字节。

Native BC7 的引入基线是 Main Sponza 2048 Debug 总加载约 `26.11 s`，其中 KTX2 read `3.52 s`、UASTC 到 BC7 转码 `21.64 s`、GPU build/upload 约 `0.31 s`。新路径把这段压缩成本移到首次 import；重复加载应只有 KTX2 I/O 与 GPU 上传。streaming/residency 的量化重开条件见[资源加载](../architecture/resource_loading.md#platform-artifact-与-residency-决策)。

2026-07-31 的实际 Native BC7 结果：Main Sponza 2048 首次 Release import（72 张、4 workers）约 `776.84 s`，第二次全复用约 `3.15 s`；Debug runtime 连续两次加载约 `1.06 s`/`1.02 s`，Native read 约 `146 ms`，72/72 命中且 `basisTranscodeCount=0`。首次 import 是离线成本，不应拿它与场景切换时间相加。

## 运行注意事项

- 开发模式的 glTF 纹理尺寸默认限制为 `2048`，可在 `VulkanLab -> Render -> Pipeline` 切换为 `Full`、`2048`、`1024` 或 `512`。切换会创建新的场景加载任务；CookedOnly 控件禁用。
- 开发模式下，KTX2 编码由 `AssetImportManager` 监督的独立 `VulkanLabAssetTool` 进程执行；glTF prepare 由 `AssetRepository` 的单个 FIFO worker 执行，`ModelGpuBuilder` 在主线程按帧创建和上传共享 `ModelAsset`。CookedOnly 不创建 AssetImportManager。资产导入显示在 `Scene -> Assets`，模型预览加载显示在窗口顶部及 `Scene -> Scenes`。
- Native BC7 的首次 Build/Rebuild 可能持续数分钟；这是离线导入成本。正常重复加载不应出现 UASTC transcode，日志应显示 `cache=native-bc7 upload=direct`。
- HDR 环境的首次 CPU bake 可能持续较长时间，应在资产导入阶段完成，而不是在场景切换时触发。运行时只读取已经发布的环境 KTX2；IBL 与 Skybox 默认关闭。
- glTF 模型加载期间继续渲染当前 Scene；新的 `ModelAsset` 完全 Ready 后才原子发布预览 Scene。旧 Scene 和不再使用的共享资源按 frame submission serial 延迟释放，因此两个大型模型切换时可能短暂同时占用显存。活跃任务的紧凑进度位于窗口顶部，详细进度位于 `Scene -> Scenes`。
- `Full` 对 Main Sponza 仍是高风险选项。开发模式的 KTX2 缓存只覆盖已经显式生成且精确匹配的 profile；未命中时仍可能回退 RGBA8。
- 日志写入运行目录下的 `logs/VulkanLab.log`。加载统计也显示在 `VulkanLab -> Diagnostics -> Load Stats`。
