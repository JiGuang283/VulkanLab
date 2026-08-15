# VulkanLab 工程结构与构建系统重构计划

> Status: Archived (superseded and completed by later execution plans)
> Last verified: 2026-08-15
> Verified against: `62f6cc4`

> Final note: target-based CMake、build-tree Shader、ProjectContext、RuntimeCommandDispatcher、Presets 和 BuildInfo 由 M0-M7 完成；剩余 Application、Editor 和 Workflow 收口随后由 `renderer_consolidation_and_app_refactor_plan.md` 完成。本文仅保留历史设计背景。

## Summary

VulkanLab 当前的源码目录已经形成 `app/assets/control/core/render/scene/window` 等基本领域边界，不需要重新设计整个渲染器。但构建系统仍接近项目早期形态：根 `CMakeLists.txt` 同时管理依赖、应用、工具、测试、Shader 和运行资源；全局 include/link path 泄漏到所有 target；应用通过递归 glob 收集整个 `src/`；资产源码在 VulkanLab、AssetTool 和 CPU tests 中重复列举并重复编译；Shader 每次构建都写回源码树；开发构建继续扫描和复制完整 `models/`。

与此同时，[Application.cpp](../../../../src/app/Application.cpp) 已承担主循环、场景工作流、资产导入、Runtime Control、所有 ImGui 面板和统计编排。继续直接加入截图、RenderDoc、Validator 和自动化任务会进一步放大耦合。

本计划进行一次有边界、行为保持的工程化重构：

```text
全局、单文件 CMake
  -> target-based CMake + 分目录构建

重复编译的源码集合
  -> 可复用内部静态库

源码树内 SPIR-V + 全量 PRE_BUILD
  -> build tree 增量 Shader target

复制全部 models
  -> ProjectContext 直接定位开发资产

Application 单体
  -> composition root + controller/dispatcher/panel 服务
```

本计划是[开发诊断与自动化工具链计划](../../../development/development_toolchain_plan.md) Stage 0/1 的前置工作。完成 target、Shader 和开发资源布局后，再实施异步截图与视觉回归，避免在即将废弃的构建和 Application 边界上继续叠加功能。

已完成的 Stage 0 到工具链 Stage 1 实施切片及验收记录见[工程基础到自动视觉回归执行记录](engineering_to_visual_regression_execution_plan.md)。

## Current Baseline

### Source Layout

当前主要规模：

| 区域 | 当前情况 |
|---|---|
| `src/app/Application.cpp` | 约 2836 行，包含应用编排、Runtime Control 和多数 ImGui。 |
| `src/assets/` | 15 个实现文件，已形成较清晰的 CPU 资产领域。 |
| `src/core/` | 14 个实现文件，管理 Vulkan/VMA、上传、同步和 pipeline。 |
| `src/render/` | 14 个实现文件，管理材质、纹理、mesh、queue、pass 和 glTF prepare。 |
| `src/scene/` | 7 个实现文件，管理 Scene、加载任务和 GPU builder。 |
| AssetTool | `TextureCacheBuilder.cpp` 约 982 行，属于独立工具领域。 |

现有目录名称和大多数文件归属合理。本计划不会为了统一外观移动全部文件。

### Build Layout

当前根 `CMakeLists.txt` 约 336 行，存在以下具体问题：

- 使用 `include_directories()` 和 `link_directories()` 修改全局状态。
- GLFW 直接依赖 `external/glfw/lib-vc2022` 和裸库名 `glfw3`。
- 使用 `file(GLOB_RECURSE ... src/*.cpp)`，任何新实现文件都会自动进入主程序，包括本应属于其他 target 的文件。
- `VulkanLabCpuTests` 与 `VulkanLabAssetTool` 手工重复列出多份 `src/assets/*.cpp` 和工具源码。
- `stb_image.cpp`、`tiny_gltf.cpp`、`tiny_obj_loader.cpp`、`vk_mem_alloc.cpp` 依赖“每个 executable 恰好编译一次”的隐式约定。
- 15 个 Shader 通过 `PRE_BUILD` 每次全量编译，输出到 `shader/**/*.spv`，这些生成文件当前被 Git 跟踪。
- POST_BUILD 每次扫描 `shader/`、`textures/` 和完整 `models/`，Main Sponza 放大了开发构建成本。
- 所有 CPU tests 由一个自定义 `main()` 串行执行，CTest 只能报告一个聚合结果。

### Dependency Boundary Issue

`SceneCatalog.h` 当前为使用 `CameraPose` 包含 `scene/Scene.h`，导致纯 Catalog 数据依赖完整运行时 Scene 类型。类似轻量类型依赖会妨碍资产库独立构建，需要先提取稳定 data-only header，而不是用更多 include path 掩盖。

## Goals

- 根 `CMakeLists.txt` 只负责 project options、依赖入口和 `add_subdirectory()`。
- 所有项目 target 只通过 `target_*` API 传播 include、definition、option 和 link dependency。
- 应用、资产工具和测试链接共享内部库，不重复维护同一份实现文件列表。
- 每个第三方 implementation macro 在明确 target 中只定义一次。
- Shader 只在自身或依赖变化时重新编译，所有 SPIR-V 生成到 build tree。
- Git 不再跟踪生成的 SPIR-V，Cook 仍能取得经过构建的准确 Shader 集合。
- 开发运行不再要求复制完整 `models/`；Catalog 场景直接从 ProjectContext 指向源码项目。
- Application 保留初始化和主循环所有权，但不再包含大型命令 switch、全部 panel 实现和新截图服务实现。
- Debug、Release、CTest、Runtime Control、KTX2 import、Main Sponza 和 Cook/package 行为保持不变。
- 为后续 RenderTest、Tracy、SPIR-V contract 和 Windows CI 提供稳定 target 和 preset。

## Non-Goals

- 不重构 RenderPipeline、MainForwardPass、descriptor layout 或材质系统。
- 不引入 ECS、RenderGraph、C++ modules、PCH 或 Unity Build。
- 不在本阶段把所有依赖迁移到 vcpkg、Conan 或 FetchContent。
- 不把内部静态库设计成稳定公共 SDK，不承诺 ABI 兼容。
- 不进行全仓库文件改名、namespace 改名或机械格式化。
- 不把 `TextureCacheBuilder.cpp`、`GltfPreparer.cpp` 等较大但职责单一的文件仅按行数拆分。
- 不改变 Catalog schema、KTX2 cache key、package schema 或 Runtime Control 协议语义。
- 不借重构机会修改渲染画面、资源格式或线程模型。

## Design Principles

1. **Target 是依赖边界**：include path、compile definition 和 link library 必须体现真实依赖。
2. **先建 target，再移动职责**：Stage 1 不大规模移动源码，避免同时修改路径、链接和行为。
3. **生成文件离开源码树**：Shader、locator、contract 和测试产物全部进入 build tree。
4. **项目资源与运行产物分离**：开发模式通过 ProjectContext 访问源资产，Cook package 使用闭包资源。
5. **Application 是 composition root**：可以拥有服务，但不实现每个服务的细节。
6. **每个提交可运行**：任何阶段都不能要求后续提交才能恢复 Debug/Release 构建。
7. **不隐藏循环依赖**：遇到 asset/scene/render 循环时提取 data-only type 或缩小接口，不使用全局 include 规避。

## Target Architecture

目标 target graph 第一版保持适度粒度：

```text
vkl_build_options                 INTERFACE

Third-party wrappers/impl:
  vkl_glm                         INTERFACE
  vkl_json                        INTERFACE
  vkl_spdlog                      INTERFACE
  vkl_stb_headers                 INTERFACE
  vkl_tinygltf_headers            INTERFACE
  vkl_tinyobj_headers             INTERFACE
  vkl_vma_headers                 INTERFACE
  vkl_glfw                        IMPORTED/INTERFACE
  vkl_image_codecs                STATIC
  vkl_gltf_parser                 STATIC
  vkl_obj_parser                  STATIC
  vkl_vma_impl                    STATIC
  vkl_imgui                       STATIC

Project libraries:
  vkl_foundation                  STATIC
  vkl_shader_catalog              INTERFACE
  vkl_asset_core                  STATIC
  vkl_asset_runtime               STATIC
  vkl_control                     STATIC
  vkl_engine                      STATIC
  vkl_asset_tool_core             STATIC

Executables:
  VulkanLab
  VulkanLabAssetTool
  VulkanLabCtl
  VulkanLab*Tests

Generated target:
  VulkanLabShaders
```

依赖方向：

```text
vkl_foundation
  <- vkl_asset_core / vkl_asset_runtime / vkl_control / vkl_engine

vkl_shader_catalog
  <- vkl_engine / vkl_asset_tool_core

vkl_asset_core
  <- vkl_asset_runtime
  <- vkl_asset_tool_core
  <- vkl_engine

vkl_control <- VulkanLab / VulkanLabCtl
vkl_engine + vkl_control + vkl_imgui <- VulkanLab
vkl_asset_core + vkl_asset_tool_core <- VulkanLabAssetTool
project libraries <- test executables
```

`vkl_engine` 第一版可以包含 `core/render/scene/window/platform`，避免过早为每个目录创建相互循环的小库。只有 profiler 或编译数据证明需要更细拆分时，才继续分割。

`vkl_foundation` 只包含跨进程需要的轻量基础能力，例如 Log 和生成的 BuildInfo，不得反向依赖 Vulkan runtime。`vkl_shader_catalog` 解析 Manifest 并暴露稳定 program/variant ID、显示名、策略和运行时路径；CookPackageBuilder 与 Renderer 共同使用它，AssetTool 不应为了枚举 Shader 而链接 `vkl_engine`。

### Data-Only Types

先新增轻量 data header，例如：

```text
src/scene/SceneTypes.h
  CameraPose
  Bounds
```

`SceneCatalog`、`PreparedSceneData` 和 `Scene` 只依赖该 data header。data header 不得包含 `Scene`、Vulkan handle、MaterialInstance 或 Device。若后续发现更多跨层描述类型，按同样规则提取，但不创建泛化的 `Common.h`。

### Third-Party Implementation Ownership

- `vkl_image_codecs` 唯一编译 `stb_image.cpp` 和 `stb_image_write.cpp`。
- `vkl_gltf_parser` 唯一编译 `tiny_gltf.cpp`。
- `vkl_obj_parser` 唯一编译 `tiny_obj_loader.cpp`。
- `vkl_vma_impl` 唯一编译 `vk_mem_alloc.cpp`。
- 不在 public header 定义 implementation macro。
- 每个最终 executable 通过静态库链接需要的实现；未使用的 OBJ/VMA 不强制进入 AssetTool。

### CMake File Layout

```text
CMakeLists.txt
cmake/
  Dependencies.cmake
  ProjectOptions.cmake
  CopyDirectoryPreserveTimestamps.cmake
  VulkanLabProjectLocator.json.in
src/
  CMakeLists.txt
shader/
  CMakeLists.txt
tools/
  CMakeLists.txt
  vulkan_lab_asset_tool/CMakeLists.txt
  vulkan_lab_ctl/CMakeLists.txt
tests/
  CMakeLists.txt
```

根 CMake 保留 KTX 的必要 feature 配置入口，但具体 workaround 可放入 `Dependencies.cmake`，并附带上游版本原因。项目 target 不直接读取其他目录的内部 source list 变量。

## Stage 0: Baseline And Change Isolation

### Scope

- 先提交当前两份未提交的工具链文档，确保工程重构提交只包含相关代码和文档。
- 记录当前 Debug/Release：
  - configure/build 命令和耗时
  - no-op rebuild 耗时
  - VulkanLab、AssetTool、Ctl 输出位置和文件集合
  - 4 个 CTest 结果
  - 15 个 SPIR-V SHA-256
  - Release package verify 结果
- 使用 Runtime Control 对 Viking Room、Sheen Chair 和 Main Sponza 1024 各加载一次，保存 LoadStats。
- 记录当前 `models/` 复制后的文件数、字节和 POST_BUILD 时间。

### Guardrails

- 重构期间禁止顺带修改 Shader 源码、材质、Vulkan state 或模型资产。
- 每阶段比较 Shader hash、Cook package protected file 集合和代表场景统计。
- 如果重构提交同时产生画面变化，先回退并缩小变更，不在结构提交中调容差。

### Acceptance

- 工作树在基线提交后干净。
- Debug、Release 和 4/4 CTest 通过。
- 基线报告放在本计划 Implementation Notes 或独立 change log，不提交 build/cache/model 输出。

## Stage 1: Target-Based CMake Foundation

### Stage 1A: Project Options And Dependency Targets

- 新增 `vkl_build_options` INTERFACE target，统一：
  - C++17
  - MSVC `/utf-8`
  - Debug/Release spdlog level definition
  - 项目 warning policy，第一版不对 `external/` 启用
- 删除全局 `include_directories()` 和 `link_directories()`。
- 为 GLM、JSON、spdlog、stb、tinygltf、tinyobj、VMA 建立命名 target，include scope 使用 `INTERFACE`。
- 将当前预编译 GLFW 包装成配置明确的 imported target，例如 `vkl::glfw`：
  - Windows x64/MSVC 使用 `external/glfw/lib-vc2022/glfw3.lib`
  - include path 由 target 传播
  - Win32 系统库通过该 target 或最终 window target 传播
- 第一版不更换 GLFW 二进制来源。替换为源码 submodule 是独立后续决策，不能与 target 化混在一起。
- KTX 4.4.2 继续使用现有 submodule 和 feature flags；ASTC MSVC runtime workaround 保留并迁移到依赖文件。

### Stage 1B: Internal Libraries

- 创建 `vkl_asset_core`，首先收纳无 Vulkan 的 Catalog、ProjectContext、Manifest、ArtifactIndex、Cache、Package 和 import transaction 逻辑。
- 创建 `vkl_foundation`，收纳 Log 和生成的 BuildInfo 等不依赖 Vulkan 的基础实现。
- 创建 `vkl_shader_catalog` INTERFACE target，使 Renderer、Cook 和后续 Shader contract tests 共享同一份 variant 元数据。
- 创建 `vkl_asset_runtime`，收纳 AssetImportManager、AssetLoadCoordinator、DerivedTextureCache 等应用/worker/runtime cache 协调逻辑。
- 创建 `vkl_control`，收纳 RuntimeCommand、protocol 和 NamedPipeServerWin32，并只依赖 foundation/JSON/Win32。
- 创建 `vkl_asset_tool_core`，收纳 TextureCachePipeline、ProcessRunner、CookPackageBuilder 和可测试的工具实现；AssetTool `main.cpp` 只负责参数解析和命令分派。
- 创建 `vkl_engine`，收纳非 Application 的 Vulkan runtime、render、scene、window 和 platform 实现。
- 创建 `vkl_imgui`，独立编译 ImGui core/backends。
- `VulkanLab` source list 第一版只包含 `src/main.cpp`、`src/app/Application.cpp` 和后续 app 专用实现。
- `VulkanLabAssetTool`、`VulkanLabCtl` 和 tests 通过链接库复用代码，不重新列出库的 `.cpp`。

### Stage 1C: Explicit Sources And Directory CMake

- 移除 `file(GLOB_RECURSE SOURCES ...)`。
- 每个目录在本地 `CMakeLists.txt` 显式列出自身 source/header。
- 根 CMake 使用 `add_subdirectory(src)`、`add_subdirectory(shader)`、`add_subdirectory(tools)`、`add_subdirectory(tests)`。
- target 名使用 `vkl_*` 内部名和可选 `VulkanLab::*` alias；不要与 executable 同名。
- public/private include scope 按 header 是否暴露依赖决定，不为解决一个编译错误把 `${CMAKE_SOURCE_DIR}/src` 设为全局。

### Tests

- 从空 build directory 配置 Debug 和 Release。
- 通过 CMake graph 或 MSBuild detailed log 确认共享资产实现每个 config 只编译到所属静态库一次。
- 删除任一必要 link dependency 时应在对应 target 失败，而不是由全局 include/link 意外补齐。
- VulkanLab、AssetTool、Ctl 和 CPU tests 全部链接成功。

### Acceptance

- 根 CMake 不再出现 `include_directories`、`link_directories` 或 `GLOB_RECURSE src`。
- tests/AssetTool 不再手工列出 `src/assets/*.cpp`。
- `tiny_gltf`、stb、tinyobj、VMA implementation translation unit 的所有权唯一且可搜索。
- Debug、Release、4/4 CTest、默认场景启动和 package verify 与基线一致。

## Stage 2: Out-Of-Source Incremental Shader Build

### Generated Layout

Shader 输出改为 config 隔离的 build tree：

```text
<build>/generated/<config>/shader/
  legacy/forward.vert.spv
  legacy/forward.frag.spv
  pbr_lite/...
  material_debug/...
```

运行目录仍保持：

```text
<runtime>/shader/<variant path>.spv
```

因此 `ShaderVariant` 的运行时相对路径和 Cook package 布局不变。

### Build Rules

- `shader/CMakeLists.txt` 显式声明 Shader source 和 output。
- 每个源文件使用独立 `add_custom_command(OUTPUT ...)`，`DEPENDS` 至少包含自身；未来共享 include 加入 depfile 或显式 dependency。
- `VulkanLabShaders` custom target 依赖全部 SPIR-V。
- `VulkanLab` 和 Cook 所需 runtime staging 显式依赖 `VulkanLabShaders`，不使用 `PRE_BUILD`。
- 只有发生变化的 Shader 重新编译；普通 C++ rebuild 不调用全部 glslc。
- 复制到 runtime output 使用生成目录，不再复制整个源码 `shader/`。
- `compile.bat` 不再作为第二套权威列表。Stage 2 完成后删除，或改成只调用 CMake target 的兼容 wrapper；不能继续单独维护 15 条命令。

### Source Tree Cleanup

- 在新链路验证后，从 Git 删除当前 15 个 `shader/**/*.spv`。
- `.gitignore` 阻止源码 shader 目录重新产生 SPIR-V。
- 删除操作单独提交，确保 review 能区分生成资产移除和构建规则变化。
- 不删除任何 `.vert/.frag` 源文件。

### Verification

- clean build 生成 Manifest 引用并去重后的全部 SPIR-V，并复制到 Debug/Release runtime 对应路径。
- no-op build 不调用 glslc。
- 修改一个 fragment shader 只重建对应 SPIR-V 和必要 staging。
- Shader Manifest 引用的每个文件存在；不存在未引用的 runtime SPIR-V。
- Cook package 只包含 Manifest 实际引用的 SPIR-V，package verify 通过。
- Shader SHA-256 与 Stage 0 基线一致；如果 glslc 版本不同，记录工具版本并先比较反射接口和画面。

### Acceptance

- 源码树不包含生成 SPIR-V。
- Shader 编译具有增量 dependency，不再依赖 target `PRE_BUILD`。
- Runtime、AssetTool Cook 和 package 使用同一份 build output，不存在手工/自动两套产物来源。

## Stage 3: Developer Asset Layout Without Full Model Copy

### Runtime Path Model

明确三类路径：

| 路径 | 开发模式 | Cooked package |
|---|---|---|
| Project root | 源码项目根目录 | package 根目录 |
| Runtime root | executable 所在目录 | package 根目录 |
| Derived cache root | 用户 cache 或显式 override | package `runtime_assets` |

新增轻量 `RuntimePaths` 或扩展 ProjectContext，使路径解析只发生一次。不要继续让各 subsystem 根据当前工作目录拼接：

- Catalog glTF/GLB 和外部依赖从 project root 解析。
- Viking Room 等 builtin 源资产从 project root 解析。
- SPIR-V 从 runtime root 解析。
- AssetTool、cache 和 package 路径继续使用现有安全检查。

### Migration Steps

1. 先让所有开发场景使用绝对解析后的路径，同时暂时保留旧复制行为。
2. 增加从任意 working directory 启动 Debug/Release 的测试。
3. 修正 Cook 对 builtin scene/texture 的闭包测试，确保不依赖开发输出中的额外目录。
4. 删除 POST_BUILD 对完整 `models/` 的复制。
5. 删除不再需要的完整 `textures/` 复制；如果 builtin runtime 仍需要 staging，只复制 Catalog 明确引用的最小集合。
6. 可保留 `VKL_COPY_DEV_ASSETS=ON` 作为短期迁移选项，但默认必须关闭，并在一个 release 周期后删除。

### Verification

- 从仓库根、Debug 输出目录和另一个任意 working directory 启动，ProjectContext 解析结果一致。
- Viking Room、Sheen Chair、Main Sponza 均可加载。
- 删除 runtime output 下的 `models/` 后开发模式仍能加载 Catalog 场景。
- Cooked package 在移除源码项目和共享 cache 后仍能独立运行。
- no-op build 不扫描/复制 Main Sponza 目录，POST_BUILD 时间明显下降。

### Acceptance

- 默认开发构建不再创建完整 runtime `models/` 副本。
- CWD 不再是源资产解析的隐式依赖。
- 开发 locator、显式 `--project` 和 Cooked package 三种模式都有测试。

## Stage 4: Application Composition Refactor

Stage 4 只拆编排职责，不改变 Scene/GPU state machine。

### Stage 4A: Runtime Command Dispatcher

- 将 `Application.cpp` 中 Runtime Control method 分派、参数校验、候选列表和 JSON response 组装移到 `control/RuntimeCommandDispatcher`。
- Dispatcher 不访问 Named Pipe thread，不拥有 Vulkan 对象，也不跨线程读取 Application state。
- 定义窄的 `RuntimeControlHost` 或等价 action interface，由 Application 在主线程实现：
  - scene query/load/reload
  - shader query/set
  - texture profile query/set
  - load/import status/cancel
  - stats/info/quit
- 长任务继续返回 taskId，不让 dispatcher 在主线程等待未来帧。
- 协议序列化测试不创建 Window/Vulkan。

### Stage 4B: Editor Panels

新增 `src/editor/`，按用户工作流拆分：

```text
ScenePanel
AssetPanel
RendererPanel
LightingPanel
MaterialsPanel
StatsPanel
```

- Panel 接收 view model 和 action interface，不直接拥有 Device、SceneLoadManager 或 AssetImportManager。
- Scene/Assets panel 可以共享同一 SceneWorkflow actions，避免 UI 和 Runtime Control 行为分叉。
- ImGui 调用留在 editor target/文件，业务状态转换留在 controller。
- 不为每个按钮创建抽象类；只有需要在 Runtime Control 和 UI 复用的操作才进入 action interface。

### Stage 4C: Scene Workflow Controller

在 Runtime dispatcher 和 panel 行为稳定后，提取 `SceneWorkflowController`：

- Catalog/registry 刷新
- artifact status 和 selected profile
- import admission、reimport 和 load chaining
- operation generation/task mapping
- scene load request/cancel/status

GPU build/publish 仍由主线程 Application/Renderer 协调。Controller 通过明确回调提交“开始 GPU load”“释放当前 Scene”等请求，不持有 GLFW/ImGui，也不让 worker 访问 Vulkan。

### Application End State

Application 主要负责：

- 构造并连接 Window、Vulkan、Renderer、controllers、panels 和 control server
- 主循环顺序
- 主线程 Vulkan ownership
- 每帧 update/render/present
- shutdown 顺序

不以行数作为唯一目标，但以下内容不得继续直接实现在 Application.cpp：

- 完整 Runtime method switch
- 每个大型 ImGui panel body
- validator/RenderTest/capture 的文件处理细节
- Catalog JSON transaction

### Verification

- Runtime Control 所有现有方法 JSON 响应与基线兼容。
- ImGui 与 Runtime Control 对同一 scene/shader/profile 操作走同一 action。
- scene load/import cancellation、快速连续切换、退出和 resize 通过现有测试与手动验证。
- 新 controller/dispatcher CPU tests 不创建 Vulkan。
- Application 析构前 server、worker、upload 和 renderer 的停止顺序不变。

### Acceptance

- 新截图服务可以作为独立 subsystem 接入，不需要把 capture task state 再写入 Application.cpp 大型 switch/panel。
- Application 是组合和主循环边界，不再是 UI、协议和资产事务的唯一实现文件。
- 没有新增双向 target dependency 或 controller 之间的隐藏 singleton。

## Stage 5: Test Target Restructure

### Scope

将当前单一 `VulkanLabCpuTests` 按责任拆成至少：

```text
VulkanLabAssetTests
VulkanLabSceneDataTests
VulkanLabAssetToolTests
```

- 第一版可以保留现有 `require()` 风格和简单 main，避免同时引入测试框架迁移。
- 每个 test executable 链接生产库，不重新编译生产 `.cpp`。
- 现有 CMake integration tests 保留独立名称：CatalogImport、TextureCache、Cook。
- 使用 CTest label：`unit`、`asset`、`tool`、`package`、后续 `gpu`/`visual`。
- 测试临时目录全部位于 build tree，并支持并行运行时唯一目录。

### Optional Test Framework Gate

只有出现以下需求时才评估 Catch2/doctest：

- 需要按 case 过滤和并行；
- CI 需要 JUnit 级别 case 报告；
- fixture/setup 重复显著增加。

测试框架迁移必须独立提交，不能成为 target 重构完成条件。

### Acceptance

- 单个资产测试失败不会只显示“CPU tests failed”聚合结果。
- `ctest -L unit` 和 `ctest -L package` 可独立执行。
- test target 不复制生产 source list。
- 原有断言数量和覆盖行为没有因拆分减少。

## Stage 6: Presets, Build Metadata And Developer Workflow

该阶段与[开发工具链计划 Stage 0](../../../development/development_toolchain_plan.md#stage-0-baseline-and-configuration-foundation)共享交付物；由本计划完成后，工具链 Stage 0 直接消费，不重复实现。

### CMake Presets

新增稳定 preset：

```text
windows-msvc-debug
windows-msvc-release
windows-msvc-test
windows-msvc-tracy        # Tracy 专用诊断配置
```

- configure/build/test preset 使用固定 build 目录命名。
- MSVC multi-config 的 `--config` 由 build/test preset 封装。
- 如 clangd 需要真实 `compile_commands.json`，增加可选 `windows-ninja-debug` preset；不宣称 Visual Studio generator 会生成该文件。
- 不把本机 Vulkan SDK 绝对路径写入 preset，继续使用 `VULKAN_SDK` 和 `find_package(Vulkan)`。

### Generated Build Info

- CMake 生成 build metadata header/JSON：
  - project version
  - Git revision/dirty marker
  - build config/compiler
  - Vulkan SDK/glslc version
- 无 Git 环境时使用明确 `unknown`，不让 source archive 配置失败。
- Runtime `system.info` 和 bug report 使用同一份生成数据。

### Documentation

- 更新 build guide，只保留 preset 作为推荐路径，旧命令作为排错参考。
- 记录 target graph、生成目录和开发/包资源解析差异。
- 更新 IDE 指南：Visual Studio 和 clangd 分别使用正确 preset。

### Acceptance

- 从 clean recursive checkout 使用文档命令完成 Debug、Release 和 CTest。
- 不需要手工修改 CMakeLists 中的 GLFW/Vulkan/GLM 路径。
- Runtime 能报告准确 build revision 和 config。

## Stage 7: Cleanup And Enforcement

### Static Build Checks

增加配置期或 CTest 检查：

- 禁止项目 CMake 新增全局 `include_directories()`、`link_directories()`。
- 禁止 Shader `.spv` 出现在源码树或 Git tracked files。
- 检查每个 `ShaderVariant` 对应 runtime output。
- 检查 Cook closure 不包含源码模型副本、开发 locator、test output 或工具链文件。
- 检查 external target 不继承项目 warning-as-error/Tracy definitions。

### Remove Transitional Paths

- 删除 `VKL_COPY_DEV_ASSETS` 迁移选项。
- 删除旧 `compile.bat` wrapper，前提是文档和 IDE task 已切换到 CMake target。
- 删除不再使用的 CMake source-list variables、全局目录变量和重复 comments。
- 保留 `CopyDirectoryPreserveTimestamps.cmake` 仅用于仍需增量 staging 的最小资源；无调用后再删除。

### Acceptance

- clean/no-op/single-C++/single-Shader 四种构建行为符合预期。
- Git status 不因构建产生源码树变更。
- Debug/Release/CTest/package verify 和代表场景运行全部通过。
- 当前 architecture/build guide 已反映最终结构，本文移入 archive 前不存在未完成 transitional option。

## Cross-Stage Verification

### Automated

每个阶段至少执行：

```powershell
cmake --build build-debug --config Debug
ctest --test-dir build-debug -C Debug --output-on-failure

cmake --build build-release --config Release
ctest --test-dir build-release -C Release --output-on-failure
```

Presets 完成后切换为 preset 命令。附加检查：

- `git diff --check`
- local Markdown link check
- tracked/source-tree SPIR-V check
- CMake target graph inspection
- Runtime package verify
- no-op rebuild 不产生 glslc/模型复制
- build 后工作树保持干净

### Runtime Smoke

- Viking Room：builtin OBJ/texture 路径。
- Sheen Chair：小型 glTF、KTX2/cache fallback。
- CarConcept 或 ChronographWatch：多材质与透明。
- Main Sponza 1024：大场景、外部依赖、72 KTX2、405 meshes。
- Runtime Control：ping、scene list/load、shader set、stats、quit。
- Cooked package：复制到源码树外、删除 source/cache 后运行。

### Invariants

- 代表 Shader SPIR-V hash 不变。
- 迁移时 selectable variants 的数量、display name、顺序和路径不变。
- Catalog、package 和 cache schema version 不变。
- Main Sponza 资源数量、cache hit、上传量和画面不因结构重构变化。
- 没有新增 validation error、queue wait 或常驻 staging。
- Application/Controller 拆分不改变主线程 Vulkan ownership。

## Commit Strategy

建议提交顺序：

1. `docs: plan engineering and build system refactor`
2. `refactor: extract shared scene data types`
3. `build: add target-scoped third-party dependencies`
4. `build: create reusable VulkanLab libraries`
5. `build: split project CMake by target`
6. `build: generate shaders outside the source tree`
7. `chore: remove tracked generated SPIR-V`
8. `refactor: resolve developer assets through project context`
9. `build: stop copying the full model directory`
10. `refactor: extract runtime command dispatcher`
11. `refactor: extract editor panels and scene workflow`
12. `test: split CPU tests by domain`
13. `build: add CMake presets and build metadata`
14. `docs: document the target and developer asset layout`

Stage 1B 内部库可以按依赖风险拆成更多小提交。不要把 tracked SPIR-V 删除、模型复制删除和 Application 拆分放在同一提交。

## Manual Work Required

代码重构、构建、CTest、package verify 和命令行 smoke 可以自动完成。以下验证需要用户或目标机器：

- Visual Studio/clangd 在新 preset 和 source grouping 下的 IDE 体验。
- Main Sponza 最终画面与窗口响应性。
- 从不同 working directory 双击/启动 executable 的实际行为。
- Release package 在独立目录和目标 GPU 上的最终运行。
- 对比重构前后 Windows 构建时间、磁盘写入和输出目录大小。

## Risks And Mitigations

| 风险 | 缓解措施 |
|---|---|
| 静态库拆分暴露循环依赖 | 提取 data-only types；第一版使用较粗 `vkl_engine`，不建立大量相互链接的小库。 |
| implementation macro 重复导致 ODR/link 错误 | 为 stb/tinygltf/tinyobj/VMA 建立唯一 implementation target 和 symbol smoke tests。 |
| Multi-config Shader 输出互相覆盖 | 输出包含 config 维度，runtime staging 从对应 config 目录复制。 |
| 删除 runtime models 后路径失败 | 先引入绝对 RuntimePaths 并保留复制，再在下一提交删除复制。 |
| Cook 意外依赖开发目录 | package closure tests 在源码/cache 删除后运行。 |
| Application 拆分改变线程所有权 | Dispatcher/controller 只在主线程执行 action；worker 规则沿用当前架构。 |
| 过度 target 化增加链接和理解成本 | 第一版只保留少量按复用边界定义的项目库；用实际循环和复用需求决定后续细分。 |
| 结构提交混入画面变化 | 对比 SPIR-V hash、LoadStats 和后续截图基准；结构提交禁止 Shader 修改。 |
| GLFW 预编译库限制编译器版本 | 本阶段先包装 imported target；源码化/包管理器迁移独立评估。 |

## Assumptions

- 第一阶段继续以 Windows x64、MSVC 2022 和现有 Vulkan SDK 为支持环境。
- KTX 4.4.2 submodule 和当前 GLFW 预编译包暂时保留。
- 内部 static libraries 不作为外部 SDK 发布，不需要安装/export rules。
- 项目源码目录继续保留现有领域命名，只有新增 app/editor/data 类型按职责放置。
- Catalog 和 ProjectContext 是开发资产定位的权威入口，当前工作目录不是长期 API。
- Cooked package 继续使用最小闭包和 package hash 验证，不依赖开发 locator。
- 当前未提交的工具链文档应在重构代码开始前单独提交。

## Completion Criteria

本计划满足以下条件后才可归档：

- 根 CMake 只承担顶层配置和子目录编排。
- 项目无全局 include/link directory，无递归收集整个 `src/` 的 glob。
- AssetTool、tests 和 app 通过内部库共享生产实现。
- 第三方 implementation translation units 有唯一 target ownership。
- SPIR-V 在 build tree 增量生成，源码树无 tracked/generated SPIR-V。
- 默认构建不复制完整 `models/`，所有开发和 Cook 路径测试通过。
- Runtime command、editor panels 和 scene workflow 不再全部实现在 Application.cpp。
- 测试按领域拆分并支持 CTest label。
- CMake Presets、build metadata 和文档可从 clean checkout 重复使用。
- Debug、Release、全部 CTest、Main Sponza、Runtime Control 和独立 Cook package 无行为回归。
- 构建结束后工作树干净，无生成文件污染源码目录。

完成后使用 `git mv` 将本文移入 `doc/archive/plans/engineering/`，并更新 Current architecture/build guide。后续自动截图工作按[开发诊断与自动化工具链计划](../../../development/development_toolchain_plan.md)继续。
