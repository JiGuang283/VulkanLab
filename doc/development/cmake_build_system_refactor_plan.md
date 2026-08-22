# CMake 与 Build 体系收口实施计划

> Status: Active
> Last verified: 2026-08-22
> Verified against: `98aba18`

## Review Summary

本计划基于当前实际 CMake target、Preset、Shader 构建规则、第三方依赖和 Cook
流程重新整理，而不是延续早期工程化文档中的假设。

当前工程已经具备以下正确基础，不需要推倒重来：

- C++ 源文件已经按 Foundation、Asset、GPU Runtime、Renderer、Scene、Editor、
  Control 等职责拆成真实 static library target。
- 编译期开关主要用于模块装配和真实/空实现选择，没有用宏切换 Shadow、IBL、
  RenderGraph 或 Shader ABI。
- 开发运行不再复制完整 `models/`，而是通过 `ProjectContext` 定位源码项目。
- 当前未提交改动已经将可运行目录、静态库、PDB、测试输出和运行时可写 Workspace
  分开。
- Cook 和 `package verify` 已经负责正式交付闭包，不需要再引入一套平行的 CPack
  产品系统。

代码 Review 后确认需要调整的重点是：

1. Visual Studio generator 与 `compile_commands.json` 配置不匹配；当前两个 build tree
   都没有生成该文件。
2. Debug/Release 全功能 configure tree重复；Visual Studio 是 Multi-Config generator，
   相同 feature set 不应配置两次。
3. `VulkanLab` 在启用 Asset Authoring 时硬依赖 AssetTool，导致日常 App build被离线
   工具链拖入。当前 dev-fast solution共 102 个 VS project，其中 57 个来自 KTX、
   4 个来自 DirectXTex。
4. AssetTool又硬依赖全部 Shader，混淆了“工具 executable”和“Cook 输入已准备完成”
   两种职责。
5. 每个 Shader variant依赖全部 GLSL include，且普通、Bindless、Lighting MRT、
   Surface MRT 的 custom command 大量重复，增量构建粒度过粗。
6. 字体、License、Validator、Shader 和 project locator 通过分散的 `POST_BUILD`
   拼装，feature关闭后可能在 `run/` 中留下旧文件。
7. `BuildFeatures.h` 同时包含运行时 feature 和 companion tool 是否构建，改变工具
   target 选择不应改变 `VulkanLab.exe` 的编译身份。
8. `src/CMakeLists.txt` 已超过单一编排文件合理职责，但不应因此把 Renderer 的每个
   feature 拆成独立 static library。

因此，本计划采取“小步收口”而不是 superbuild 重写：先固化现有输出基线，再收敛
Preset 和依赖图，然后修复 Shader 增量构建，最后统一运行镜像与 Cook 入口。KTX
预构建依赖、Ninja 和编译加速只在测量证明有收益后引入。

## Goals

完成后，构建体系应满足：

```text
Source Project
  -> Configure Profile
  -> CMake Target Graph
  -> Compile / Link / Shader Build
  -> Developer Runtime Image
  -> Optional Cook + Package Verify
```

- 日常渲染器修改只构建 VulkanLab、必要运行库和发生变化的 Shader。
- AssetTool、Ctl、RenderTest 和 Tests只在对应工作流中构建。
- 相同 CMake feature set只拥有一个 configure tree；Debug/Release由 build config区分。
- `run/<Config>` 能准确表示当前构建，不保留已经关闭功能的陈旧文件。
- Shader include变更只重编译真实依赖它的 variant。
- VS Code 的代码模型与 CMake target保持一致，不引用不存在的 compilation database。
- 正式出包始终经过 Runtime Release build、Cook、Package Verify 和仓库外启动验证。
- 现有命令迁移有明确兼容窗口，脚本和文档不会长期同时维护多套名称。

## Non-Goals

本计划不包含：

- 不重构 Renderer、RenderGraph、Scene 或 Asset 的运行时代码职责。
- 不因为 CMake 文件较长而把每个 Render feature做成独立 library。
- 不引入 vcpkg、Conan 或 FetchContent 替代当前固定版本依赖。
- 不立即把 KTX、DirectXTex 改成 ExternalProject 或预编译二进制。
- 不默认启用 Unity Build、PCH、LTO、`/MP` 或编译缓存。
- 不用 CPack 替代现有 Native Scene Cook/package closure。
- 不改变 Shader ABI、Catalog、SceneDocument、Derived Asset Cache 或 Cook schema。
- 不修改用户场景、环境、模型和 LocalAppData 中的派生缓存。

## Current State

### Configure Trees

当前存在六个 configure preset：

```text
windows-msvc-debug
windows-msvc-release
windows-msvc-dev-fast
windows-msvc-ao-compare
windows-msvc-tracy
windows-msvc-runtime
```

`windows-msvc-debug` 与 `windows-msvc-release` 的 cache variables相同，差异只在
build preset的 `configuration`。因为生成器是 Visual Studio 2022，这两个 configure
tree可以安全合并。

以下 profile必须保持独立 binary tree：

- Dev：Editor/Validation/Authoring启用，Tests和多数辅助 executable关闭。
- Full：完整工具和 Tests target可生成，Debug/Release共享。
- Runtime：Editor、Control、Capture、Authoring和开发诊断均未编译。
- Tracy：编译定义、链接依赖和客户端线程与普通 Dev不同。
- CACAO：增加第三方源码、DXC Shader 生成和额外运行文件。

### Target Graph

当前核心 target边界总体合理：

```text
vkl_foundation
vkl_shader_catalog
vkl_scene_data
vkl_asset_core
vkl_gpu_runtime
vkl_capture
vkl_renderer_runtime
vkl_asset_runtime
vkl_scene_runtime
vkl_scene_workflow
vkl_platform_runtime
vkl_editor                  optional
vkl_runtime_control_adapter optional
```

主要问题不是 target数量，而是少数错误的跨工作流依赖：

```text
VulkanLab -> VulkanLabAssetTool       # 当前由 tools/CMakeLists.txt 注入
VulkanLabAssetTool -> VulkanLabShaders
VulkanLabCpuTests -> vkl_engine       # transitional aggregate
```

`vkl_engine` 当前没有源码，只用于兼容测试依赖。它会隐藏测试真正需要的模块，应在
本计划中移除。

### Shader Pipeline

CMake从 `shader/manifest.json` 解析 program，生成普通、Bindless 和 MRT 变体。规范
SPIR-V 位于：

```text
build/<profile>/generated/<Config>/shader/
```

随后再 stage 到：

```text
build/<profile>/run/<Config>/shader/
```

当前所有 Shader command都把全部 `shader/include/*.glsl` 放入 `DEPENDS`。这保证了
正确性，但使任何共享 include修改都触发近似全量 Shader rebuild。该问题应通过
compiler depfile解决，而不是维护手写 include映射。

### Runtime Image And Package

当前未提交的输出布局为：

```text
build/<profile>/
  generated/<Config>/
  lib/<Config>/
  symbols/<Config>/
  run/<Config>/
  test-bin/<Config>/
  test-work/
  test-results/
```

该布局作为本计划基线保留。正式 package继续由 AssetTool Cook生成；`run/` 是开发
运行镜像，不是可发布 package。

## Target Architecture

### Configure Profile 与 Build Config 分离

明确区分：

- Configure Profile：决定编译能力、依赖和 target集合，必须使用独立 binary tree。
- Build Config：决定 Debug/Release 优化和符号，同一 Visual Studio tree中可以切换。
- Build Target：决定本次实际构建哪些 executable、Shader和运行文件。

目标目录：

```text
build/
  dev/
  full/
  runtime/
  tracy/
  cacao/
```

目标 preset关系：

```text
windows-msvc-base              hidden
  +-- windows-msvc-editor-base hidden
  |     +-- windows-msvc-dev-fast
  |     +-- windows-msvc-full
  |     +-- windows-msvc-tracy
  |     `-- windows-msvc-ao-compare
  `-- windows-msvc-runtime
```

用户可见 configure preset：

| Configure preset | Binary tree | Feature set |
|---|---|---|
| `windows-msvc-dev-fast` | `build/dev` | 日常 Editor，全运行功能，Tests/RenderTest/Ctl关闭 |
| `windows-msvc-full` | `build/full` | 全运行功能、全部工具和 Tests target |
| `windows-msvc-runtime` | `build/runtime` | 精简 Release Runtime |
| `windows-msvc-tracy` | `build/tracy` | Dev + Tracy + Ctl |
| `windows-msvc-ao-compare` | `build/cacao` | Dev + CACAO + Ctl |

Build preset：

| Build preset | Configure preset | Configuration | Aggregate target |
|---|---|---|---|
| `windows-msvc-dev-fast` | dev-fast | Debug | `VulkanLabDeveloper` |
| `windows-msvc-dev-runtime` | dev-fast | Debug | `VulkanLabRuntimeImage` |
| `windows-msvc-full-debug` | full | Debug | `VulkanLabFull` |
| `windows-msvc-full-release` | full | Release | `VulkanLabFull` |
| `windows-msvc-runtime` | runtime | Release | `VulkanLabRuntimeImage` |
| `windows-msvc-tracy` | tracy | Debug | `VulkanLabDeveloper` |
| `windows-msvc-ao-compare` | cacao | Debug | `VulkanLabDeveloper` |

迁移期保留 `windows-msvc-debug` 和 `windows-msvc-release` build preset作为 full tree的
别名；所有仓库脚本迁移后再删除。不得继续创建独立 `build/windows-msvc-debug` 和
`build/windows-msvc-release`。

### Aggregate Targets

新增不拥有源码的 custom aggregate target：

```text
VulkanLabRuntimeImage
  -> VulkanLab
  -> VulkanLabShaders
  -> runtime image staging

VulkanLabDeveloper
  -> VulkanLabRuntimeImage
  -> VulkanLabAssetTool        when enabled
  -> VulkanLabCtl              when enabled
  -> editor/tool payload

VulkanLabFull
  -> VulkanLabDeveloper
  -> VulkanLabRenderTest       when enabled
  -> VulkanLabCpuTests         when BUILD_TESTING

VulkanLabCookInput
  -> Release VulkanLabRuntimeImage
  -> VulkanLabAssetTool
```

规则：

- `VulkanLab` executable只链接运行时模块，不依赖 companion executable。
- AssetTool不依赖 Shader target；只有 `VulkanLabCookInput` 同时要求两者。
- Dev preset仍默认生成可以 OnDemand authoring 的完整开发镜像。
- 修改 Renderer 后可显式构建 `windows-msvc-dev-runtime`，避免重建工具。
- `VulkanLabRenderTest` 可以依赖 runtime image，而不是裸 executable，以保证 Shader
  和运行资源已经 stage。

### CMake File Ownership

保留当前 static library粒度，但把 600 行 `src/CMakeLists.txt` 拆到主要所有者目录：

```text
src/CMakeLists.txt                 orchestration only
src/core/CMakeLists.txt            foundation + GPU runtime
src/diagnostics/CMakeLists.txt     capture and diagnostics support
src/render/CMakeLists.txt          shader catalog + renderer runtime
src/assets/CMakeLists.txt          asset core + asset runtime
src/scene_data/CMakeLists.txt      scene data
src/scene/CMakeLists.txt           scene runtime
src/workflows/CMakeLists.txt       workflow controllers
src/platform/CMakeLists.txt        platform runtime
src/control/CMakeLists.txt         protocol/server/adapter
src/editor/CMakeLists.txt          optional editor
src/app/CMakeLists.txt             VulkanLab executable and assembly
```

不移动 C++ 文件作为该阶段的必要条件。某个 target少量引用相邻目录源码是可接受的，
但 target定义必须位于其主要 owner目录。

所有项目内公开 target继续提供 `VulkanLab::<Name>` alias。第三方 target不得获得项目
的 `/W4`、BuildFeatures include或项目级 feature definitions。

## Implementation Stages

## Stage 0: 固化当前 Workspace 与 Output 基线

> Status: Completed in `17cd0b7`

### Goal

先把当前未提交的运行目录/Workspace分离作为独立提交固定下来，防止后续 CMake重构
同时改变路径语义和依赖图。

### Work

- Review并提交当前 `run/lib/symbols`、Workspace、test-work/test-results和清理脚本改动。
- 保留用户当前 Scene、Environment、Derived Assets和第三方工作树不进入提交。
- 修正 `.vscode/settings.json` 的归属：如果只为临时查看 `build/` 而修改，则在本阶段
  明确决定是否保留，不把来源不明的设置混入 CMake提交。
- 记录以下 baseline：
  - dev-fast configure时间。
  - 无改动增量 build时间。
  - 修改一个 Renderer `.cpp` 后 build时间。
  - 修改一个局部 Shader include 后重新编译的 SPIR-V数量。
  - dev-fast和runtime生成的 VS project数量。

### Completion Criteria

- `windows-msvc-dev-fast` 与 `windows-msvc-runtime` 可以构建和启动。
- 可执行路径保持 `build/<profile>/run/<Config>/VulkanLab.exe`。
- 源码根没有运行日志、Capture、临时导出或派生缓存。
- 后续阶段拥有可比较的构建数据。

### Stage 0 Baseline Record

2026-08-22 在 Windows、Visual Studio 17 2022 generator、MSBuild 17.14.40、
CMake 4.1.0-rc1 环境下记录，并由 `17cd0b7` 固化。所有时间均为单次本机 wall time，
仅用于同一台机器上的前后对比：

| Measurement | Baseline |
|---|---:|
| clean dev-fast configure/generate | 5.296 s |
| no-op dev-fast build | 3.698 s |
| touch `RendererFeatureGraph.cpp` 后 build | 5.920 s |
| touch `atmosphere_scattering.glsl` 后 Shader build | 42.955 s |
| 上述局部 GLSL include变更触发 compile/stage | 127 / 127 |
| dev-fast Visual Studio projects | 102 |
| runtime Visual Studio projects | 51 |
| dev-fast项目中的 KTX / DirectXTex projects | 57 / 4 |
| dev-fast/runtime `compile_commands.json` | absent / absent |

该基线确认两项后续验收重点：普通 Renderer单文件增量已经较短，Stage 2不得使它
回退；Shader局部 include 当前等价于全量 rebuild，Stage 4必须显著缩小其依赖集合。

## Stage 1: Preset 与 VS Code 配置收口

> Status: Completed in `597d038`

### Problem And Root Cause

Preset重复声明相同 feature变量；Debug/Release使用两个相同 configure tree。工程同时
选择 Visual Studio generator和 `compile_commands.json`，但实际 build tree中不存在
该文件。

### Solution

- 使用 hidden base preset和 `inherits` 消除 feature重复。
- full Debug/Release共享 `build/full`。
- dev/runtime/tracy/cacao因编译能力不同继续隔离。
- 保持 Visual Studio 2022为 canonical generator。
- VS Code改用：
  ```json
  "cmake.useCMakePresets": "always",
  "C_Cpp.default.configurationProvider": "ms-vscode.cmake-tools"
  ```
- 删除失效的 `C_Cpp.default.compileCommands`。
- `CMakeUserPresets.json` 加入 `.gitignore`，供本机覆盖 SDK、generator或缓存路径。
- configure摘要打印 profile名、binary tree、runtime output和 runtime feature列表。

### Why Not Switch To Ninja Immediately

Ninja可以提供 `compile_commands.json` 并降低 VS solution生成开销，但当前普通终端没有
`cl.exe` 环境。直接替换 generator会使仓库 CLI 命令依赖用户先进入 Developer Shell。

本阶段只允许新增实验性的 `windows-ninja-dev` preset，并且必须配套稳定的 MSVC环境
bootstrap脚本；在 clean/incremental数据证明有收益前，不替换 canonical preset。

### Compatibility

- 旧 Debug/Release build preset保留一个迁移周期。
- 仓库内脚本和文档必须在删除旧 alias前全部迁移。
- 不允许不同 feature profile共享同一 binary directory。

### Completion Criteria

- VS Code正确获得 CMake target和 IntelliSense配置。
- `build/` 只需要 dev、full、runtime以及按需 profile。
- full tree可以先后构建 Debug和Release而无需重新 Configure。
- 删除任意一个 profile目录不会影响其他 profile。

### Stage 1 Verification Record

2026-08-22 完成以下验证：

- `cmake --list-presets` 只暴露 dev、full、runtime、tracy 和 cacao 五个
  configure profile；旧 Debug/Release 仅保留为 build preset兼容别名。
- `build/dev` Debug、`build/runtime` Release、`build/full` Debug 与 Release均构建
  成功；full的两个配置在同一生成树中连续构建，没有重新 Configure。
- dev与runtime executable分别使用隔离 Workspace持续运行8秒，日志中没有
  `error` 或 `critical` 记录。
- 清理旧生成树后，`build/`只包含 `dev`、`full` 和 `runtime`。
- VS Code JSON、CMake preset和迁移后的 PowerShell脚本均通过语法解析；
  `git diff --check`通过。
- 按项目默认策略没有运行CTest、Golden或Validation smoke。

## Stage 2: Target 图与 Build Feature 语义收口

### Problem And Root Cause

当前 executable和工作流 aggregate混在一起，导致 App build拉入 AssetTool。工具是否
生成也被写进 `BuildFeatures.h`，把 build product选择误当成 runtime capability。

### Solution

- 删除 `add_dependencies(VulkanLab VulkanLabAssetTool)`。
- 删除 `add_dependencies(VulkanLabAssetTool VulkanLabShaders)`。
- 新增 RuntimeImage、Developer、Full、CookInput aggregate target。
- 将 `BuildFeatures.h` 拆为：
  - `RuntimeFeatures.h`：只包含 `VKL_ENABLE_*`，可进入 VulkanLab。
  - CMake-only product options：`VKL_BUILD_*`，不进入 Runtime feature ABI。
- `system.info` 中 companion tool状态改为运行时发现或独立 `build.products` 字段，不再
  宣称它是 VulkanLab编译能力。
- 测试显式链接需要的模块，移除 `vkl_engine` transitional aggregate。
- 为项目 target设置 IDE folder：Runtime、Editor、Tools、Tests、Shaders、ThirdParty。

### Risks

- OnDemand import要求 AssetTool可执行文件存在。默认 Developer aggregate仍包含工具，
  只构建 RuntimeImage时启动应明确提示 AssetTool缺失，或使用 `--asset-mode readonly`。
- Cook脚本不得假设“构建 AssetTool”会顺便构建 Shader。
- 移除 `vkl_engine` 后可能暴露测试遗漏的直接依赖，这是预期的边界修复。

### Completion Criteria

- 构建 `VulkanLabRuntimeImage` 不会配置外的工具源码重编译或链接 AssetTool。
- 构建 `VulkanLabDeveloper` 后 OnDemand authoring可用。
- AssetTool源码修改不会导致 VulkanLab relink。
- Shader源码修改不会无条件要求 AssetTool relink。
- Runtime feature日志不因 `VKL_BUILD_CONTROL_TOOL` 等 product option改变。

### Stage 2 Verification Record

2026-08-22 完成以下验证：

- `VulkanLabRuntimeImage` 从清理后的 dev产物状态构建成功；输出目录没有
  `VulkanLabAssetTool.exe`，构建日志没有 AssetTool或DirectXTex编译/链接。
- `VulkanLabDeveloper` 随后只补齐当前profile启用的AssetTool与KTX CLI；
  `VulkanLabFull` 成功生成Renderer、Ctl、AssetTool、RenderTest和CPU test executable。
- 移除 `vkl_engine` 后，CPU tests改为显式链接实际模块。完整 `ktx` 与
  `ktx_read` 在同一测试进程中的MSVC静态链接顺序已显式约束；测试 executable
  链接成功，但按项目策略没有执行测试。
- dev与runtime分别构建Debug和Release成功；dev通过Runtime Control正常启动、查询
  并退出，runtime持续运行5秒。运行日志没有新增error或critical。
- `RuntimeFeatures.h` 只包含 `VKL_ENABLE_*`；`--build-info-json`不再报告产品选择。
  `system.info.build.products`按运行目录实际发现AssetTool=true、Ctl=false、
  RenderTest=false，与dev产物一致。
- 单独修改AssetTool源文件后只有AssetTool重新链接；单独修改一个Shader后只重新
  编译和stage对应SPIR-V，VulkanLab与AssetTool均未重新链接。
- RuntimeImage仍会编译 `ktx_read` 当前传递引入的ASTC实现；该第三方配置问题记录为
  Stage 3 Dependency拆分输入，不属于companion executable耦合。
- `cmake --list-presets`、三套configure profile和`git diff --check`通过。

## Stage 3: CMake 文件和 Dependency 配置拆分

### Goal

降低单文件职责和第三方全局 cache副作用，同时保持现有 C++ target粒度。

### Work

- 将 `src/CMakeLists.txt` 按 owner目录拆分，根文件只控制 add_subdirectory顺序。
- 将 `cmake/Dependencies.cmake` 拆为：
  ```text
  cmake/dependencies/Core.cmake
  cmake/dependencies/Rendering.cmake
  cmake/dependencies/Editor.cmake
  cmake/dependencies/AssetTools.cmake
  cmake/dependencies/Diagnostics.cmake
  ```
- 每个 dependency文件只在对应 feature/target需要时配置依赖。
- 将 KTX、DirectXTex、Tracy、CACAO 的 FORCE cache设置集中在各自 helper中，并在注释
  中记录为什么必须覆盖 upstream默认值。
- 拆分 `vkl_build_options`：
  - language/ABI options。
  - project warnings。
  - runtime feature include path。
- 第三方 target不继承项目 warnings和 feature header。
- 保持所有 C++ source显式列出；不对 C++ 使用 `GLOB_RECURSE`。

### Why Not Split Renderer Further

`vkl_renderer_runtime` 虽然包含约 65 个 implementation file，但它们共享 RenderGraph、
Pipeline、Shader ABI和大量内部类型。拆成大量 static library不会减少单 TU增量编译，
反而会暴露循环依赖和增加链接管理。只有后续 include graph或编译时间数据显示某个
feature可独立编译和链接时再拆。

### Completion Criteria

- 根 `src/CMakeLists.txt` 只表达模块顺序和 Application assembly。
- 每个 target在一个明确 owner文件中定义。
- 关闭 Editor、AssetTool、Tracy或CACAO时不配置其源码依赖。
- Dev和Runtime的 target数量不因重构无意义增加。

### Stage 3 Verification Record

Stage 3 已由 `98aba18` 完成并验证：

- 根 `src/CMakeLists.txt` 缩减为模块 `add_subdirectory()` 顺序和 `VulkanLab`
  executable assembly；Foundation、GPU Runtime、Platform、Renderer、Assets、
  Diagnostics、Scene Data、Scene、Workflow、Control 和 Editor 均由各自 owner
  `CMakeLists.txt` 定义。
- `cmake/Dependencies.cmake` 只负责编排 Core、Diagnostics、AssetTools、Rendering
  和 Editor 五个 dependency 文件。DirectXTex、Tracy、CACAO、ImGui/ImGuizmo 和
  SPIR-V Reflect 只在对应 product/feature 打开时配置。
- KTX 与 DirectXTex 使用的通用 cache 变量在添加上游子目录后恢复；fresh 配置的
  dev/full/runtime `CMakeCache.txt` 均未泄漏 `BUILD_SHARED_LIBS`、`BUILD_TOOLS`、
  `BUILD_SAMPLE`、`BUILD_DX11`、`BUILD_DX12` 或 `BC_USE_OPENMP`。
- `VulkanLab::ProjectOptions`、`VulkanLab::ProjectWarnings` 和
  `VulkanLab::RuntimeFeatures` 已拆开；运行时通过 `BuildOptions` 聚合，host tools
  和 tests 不再继承 runtime feature include path，第三方 target 不继承项目
  warning policy。
- 三套 profile 均从 fresh 生成树配置成功。实际 Visual Studio solution project
  数为 dev 51、full 63、runtime 33，与 Stage 2 的有效 target 集合一致，没有因文件
  拆分新增 C++ target。runtime solution 只包含 `VulkanLab` 产品，不包含 Editor、
  ImGui、ImGuizmo、DirectXTex、Tracy、CACAO、SPIR-V Reflect 或开发工具。
- dev Debug、full Debug 和 runtime Release 全部构建成功；full 同时链接了 AssetTool、
  Ctl、RenderTest 和 CPU test executable。按项目策略只确认测试 executable 可构建，
  未执行 CTest 或其他测试套件。
- dev 通过唯一 Named Pipe 完成启动、默认模型加载、`ping/info/quit`；runtime Release
  在无 Editor、无 GUI 路径持续渲染 5 秒。两者均未出现新的配置、链接或启动错误。
- C++ source glob 审计为空，`git diff --check` 通过。构建仍显示既有的 MSVC
  `getenv`、结构对齐、未使用参数和 CRT/PDB warning；它们不是本阶段引入的错误。
- runtime 的 `ktx_read` 仍会编译 KTX 当前传递依赖的 ASTC implementation。该依赖
  属于运行时 KTX2 reader 的上游实现细节，不会引入 KTX CLI、KTX1 或 AssetTool；
  是否进一步拆成预构建依赖留到 Stage 7 测量后决定。

## Stage 4: Shader 构建管线重构

### Problem And Root Cause

当前 Shader CMake正确但重复，并把所有 include作为所有 Shader的依赖。随着 Bindless、
Forward/Deferred和MRT组合增加，增量构建成本会持续放大。

### Solution

- 新增 `cmake/ShaderCompilation.cmake`。
- 用统一函数描述一个 Shader job：
  ```cmake
  vkl_add_shader_variant(
      SOURCE <path>
      OUTPUT_SUFFIX <suffix>
      TARGET_ENV <env>
      DEFINES <...>
      GENERATED_OUTPUT <path>
      RUNTIME_OUTPUT <path>)
  ```
- Manifest解析只负责生成 job，不复制 custom command模板。
- `glslc` 增加 depfile输出，并由 `add_custom_command(DEPFILE ...)` 消费，使每个 SPIR-V
  只依赖实际 include闭包。
- 继续对每个输出执行 `spirv-val`。
- 保持规范 SPIR-V和 runtime-staged SPIR-V两层，不在源码树生成二进制。
- 把 compile和stage目标分开：
  ```text
  VulkanLabShaderCompile
  VulkanLabShaders
  ```
- Manifest变更触发重新 Configure；单个 GLSL或 include变更只触发必要 job。
- 失败时不把未通过 `spirv-val` 的产物 stage到 runtime image。

### Edge Cases

- 同一源文件以不同 target-env编译时必须生成独立 job和 depfile。
- Bindless、MRT define组合必须进入输出名，防止两个 command声明同一输出。
- 删除 Manifest program后，staging阶段必须移除陈旧 SPIR-V。
- Windows路径中的空格必须通过 `VERBATIM` 和独立参数处理。
- DEPFILE行为需在 canonical Visual Studio generator和可选 Ninja generator上分别验证。

### Completion Criteria

- Shader variant数量与 Manifest预期一致。
- 修改局部 include只重编译其真实 consumer。
- 修改共享 ABI include仍正确重编译全部受影响 variant。
- 删除 program后 `run/<Config>/shader` 不保留对应旧 SPIR-V。
- Shader compile失败不会污染可运行镜像。

## Stage 5: 声明式 Developer Runtime Image

### Problem And Root Cause

多个 target通过分散 `POST_BUILD` 复制文件，无法统一判断当前 feature下哪些文件应该
存在，也不会删除旧配置遗留物。

### Solution

- 新增 `cmake/RuntimeImage.cmake` 和 `StageRuntimeImage.cmake`。
- 每个 subsystem只向 runtime manifest注册 payload：
  ```text
  VulkanLab.exe
  shader/manifest.json
  shader/**/*.spv
  vulkanlab_project.json
  editor/lucide.ttf             Editor only
  licenses/Lucide/*             Editor only
  VulkanLabAssetTool.exe        Developer aggregate only
  ktx.exe                       AssetTool only
  gltf_validator.exe            installed and discovered only
  CACAO license/notices         CACAO only
  ```
- staging脚本只管理 manifest声明的相对路径，禁止删除用户 Workspace或 package目录。
- 上一次 manifest存在、当前 manifest不再包含的 owned file应被删除。
- runtime image staging使用 stamp和真实输入依赖，不在无改动 build中重复复制所有文件。
- 保留当前用户路径：
  `build/<profile>/run/<Config>/VulkanLab.exe`。
- `VulkanLab.exe` 可继续直接输出到 `run/`；本计划不强制增加一层 raw binary copy，除非
  staging实现证明无法可靠管理 executable。

### Why Not Use CPack

Developer Runtime Image只服务本地运行，正式发布由 AssetTool解析 Native Scene闭包、
BC7、Environment、Catalog和 Shader hash。CPack不了解这些语义，接入后会形成第二套
不完整 package定义。因此只允许使用 `install()` helper复用文件规则，不把 CPack作为
产品入口。

### Completion Criteria

- feature关闭后重新构建，旧字体、工具、CACAO或Validator payload不会残留。
- 无改动增量 build不重复 stage整个 Shader目录。
- Developer、Runtime、Tracy和CACAO运行镜像只包含各自所需文件。
- Developer runtime image仍可从任意工作目录启动并定位源码项目。

## Stage 6: Build、Cook 与 Package 的统一入口

### Goal

让开发构建和出包验证使用稳定命令，不要求用户记住多个 executable路径和参数组合。

### Work

- 新增或收口以下 PowerShell入口：
  ```text
  tools/dev/Configure-Project.ps1
  tools/dev/Build-Developer.ps1
  tools/dev/Build-Runtime.ps1
  tools/dev/Cook-Package.ps1
  tools/dev/Verify-Package.ps1
  tools/dev/Clean-LocalOutputs.ps1
  ```
- 脚本只编排 CMake和现有 AssetTool，不复制 Cook业务逻辑。
- `Cook-Package.ps1` 固定执行：
  ```text
  configure/build dev AssetTool
  configure/build runtime Release image
  AssetTool cook
  AssetTool package verify
  optional repository-external launch
  ```
- Cook默认使用 `build/runtime/run/Release`，并先检查 `--build-info-json`。
- package输出默认进入 `dist/<package-name>`，临时 staging进入 Workspace或 `out/package`，
  不进入源码资产目录。
- `--build-missing`、Scene ID、Startup Scene和Output保持显式参数。
- 失败不得覆盖之前已发布 package；沿用 Cook现有原子发布语义。

### Completion Criteria

- 新用户只需要一个 configure/build命令即可启动 Editor。
- 一个命令可以生成、验证并可选启动 Cooked package。
- package从仓库外启动时不读取源码 Catalog、models、DerivedAssets或 Workspace。
- CMake build和AssetTool Cook的职责边界清晰，没有重复复制资产闭包。

## Stage 7: 构建性能测量与可选优化

### Goal

在依赖图和增量正确性收口后，再根据数据决定是否引入更激进的编译优化。

### Measurements

每个 profile记录：

- clean configure/generate时间。
- clean build时间。
- no-op build时间。
- 单 Renderer `.cpp` 修改后的 build时间。
- 高频公共 header修改后的 build时间。
- 单 Shader和共享 Shader include修改后的 build时间。
- solution target数量和 build tree大小。

### Optional Improvements

按测量结果依次评估：

1. 实验性 Ninja + MSVC dev preset和可靠的 `VsDevCmd` bootstrap。
2. MSBuild项目级并行和 `/MP`，避免与 CI或编译机器过度订阅。
3. Renderer或Editor PCH，仅在公共稳定 header解析占主导时启用。
4. `sccache`/`clcache`，仅在本地和 CI部署成本可接受时启用。
5. Runtime Release可选 IPO/LTCG，作为交付优化而不是日常 build默认值。
6. KTX/DirectXTex预构建 dependency cache或独立 tools superbuild。

### Explicitly Rejected Defaults

- Dev默认 Unity Build：会恶化单文件迭代、宏隔离和错误定位。
- 全局 PCH：会加强不合理 include耦合。
- 固定并行 job数量：不同开发机和 CI资源差异过大。
- 立即 ExternalProject化全部依赖：会增加跨配置、调试符号和安装路径复杂度。

### Completion Criteria

- no-op和单文件增量时间不差于 Stage 0 baseline。
- Shader局部变更明显少于当前近全量变体 rebuild。
- 若启用 Ninja/PCH/编译缓存，必须有记录数据证明收益并保留稳定回退路径。

## Verification Matrix

遵循项目当前策略，只构建和实际启动，不默认运行 CTest、Golden、视觉回归或
Validation smoke。

每阶段至少执行受影响配置：

| Change | Required build/start |
|---|---|
| Preset/layout | dev-fast + runtime |
| Editor dependency | dev-fast + runtime |
| AssetTool graph | dev-fast Developer + dev RuntimeImage |
| Shader pipeline | dev-fast Shader + VulkanLab start |
| Tracy dependency | tracy build + start |
| CACAO dependency | ao-compare build + start |
| Cook workflow | runtime Release + one small Native Scene package |

通用检查：

- `git diff --check`。
- Configure摘要与 BuildInfo feature一致。
- `run/<Config>` 不存在已关闭 feature的陈旧 payload。
- `VulkanLab.exe --build-info-json` 在创建 Window/Vulkan前成功。
- dev-fast和runtime都能启动并持续渲染。
- package verify成功，并能从仓库外启动。

测试 target仍必须保持可配置、可构建，但只有用户明确要求时才运行测试套件。

## Delivery Strategy

建议提交顺序：

1. `build: separate runtime workspaces and generated outputs`
2. `build: consolidate CMake presets and editor integration`
3. `build: separate runtime and tool aggregate targets`
4. `refactor: organize CMake targets by module ownership`
5. `build: track precise shader dependencies`
6. `build: stage declarative developer runtime images`
7. `build: unify cook and package workflows`
8. `docs: document the consolidated build system`

每个提交必须保持至少一个常用 preset可构建，不允许在多个提交之间留下无法 Configure
的中间状态。

## Risks And Mitigations

| Risk | Mitigation |
|---|---|
| Preset重命名破坏脚本 | 保留一个迁移周期的 alias，删除前用 `rg` 扫描仓库调用点 |
| 共享 full tree的 Debug/Release输出互相覆盖 | 所有 runtime/lib/symbols/generated路径继续包含 `$<CONFIG>` |
| App不再依赖 AssetTool后 OnDemand失效 | 默认 Developer aggregate仍构建工具；RuntimeImage单独模式明确要求 readonly或外部工具 |
| Shader DEPFILE在 VS generator表现不同 | 先建立 fixture验证单 include重编译集合，再启用到全量 Manifest |
| Staging清理误删文件 | 只删除上一份 owned manifest记录的相对路径并验证目标位于 `run/<Config>` |
| KTX upstream cache污染 | 集中 helper和注释；预构建模式留到测量阶段 |
| 拆分 CMake暴露循环依赖 | 不新增细粒度 library；必要时先提取 data-only interface而不是互相链接 |
| Full tree过大 | 日常使用 dev tree；Full只用于完整工具和显式测试构建 |

## Future Improvements

以下内容有价值，但不纳入本轮实施：

- CI build matrix与远程编译缓存。
- Linux或Clang toolchain及跨平台 package profile。
- KTX、DirectXTex、Tracy的二进制 dependency cache。
- Shader permutation离线数据库和并行 Shader worker。
- 按 Render Path裁剪 Cooked Shader closure。
- C++ Modules。
- 自动 ABI兼容报告和构建可复现性签名。

## Final Completion Criteria

本计划完成的判定标准是：

- 只有五类有真实 feature差异的 configure tree；Full Debug/Release共享。
- Preset通过 inheritance维护，仓库脚本不包含废弃 build目录。
- VS Code不再引用不存在的 `compile_commands.json`。
- VulkanLab、AssetTool、Shader和测试之间不存在工作流级错误硬依赖。
- 日常 Runtime build不会被 DirectXTex/KTX Tool链无条件拖入。
- Shader使用真实 include依赖，variant构建逻辑无重复 custom command模板。
- Runtime image由统一 manifest装配并清理陈旧 payload。
- 出包流程统一为 Runtime Release -> Cook -> Verify -> 仓库外启动。
- dev-fast与runtime均可构建和运行，且增量构建数据不低于基线。
