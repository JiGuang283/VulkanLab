# Shader Registry

> Status: Current
> Last verified: 2026-07-30
> Verified against: Compute Bloom v1 working tree

Shader 注册的唯一权威来源是
[`shader/manifest.json`](../../shader/manifest.json)。运行时不会扫描目录或根据
文件名猜测 stage 组合；CMake、VulkanLab、CookPackageBuilder 和 Shader
contract tests 都读取同一份 Manifest。

## 数据模型

`programs` 描述可创建 pipeline 的 stage 组合：

- `id`：稳定的小写机器 ID。
- `contract`：`main-forward`、`shadow-depth`、`fullscreen` 或 `compute`。
- `vertex` / `fragment`：graphics program 的 GLSL 源路径。
- `compute`：compute program 的 GLSL 源路径，不能与 graphics stage 混用。

`variants` 描述 `VulkanLab -> Render -> Pipeline` 可选择的 Main Forward program：

- `id`：Runtime Control 和内部状态使用的稳定 ID。
- `displayName`：ImGui 和兼容接口显示的名称。
- `program`：必须引用 `main-forward` program。
- `category`：当前使用 `legacy`、`pbr` 或 `debug`。
- `toneMapping`：`configurable` 或 `pass-through`。
- `bloom`：可选布尔值，声明 variant 是否允许 Compute Bloom；缺省为 `false`。
- `default`：全部 variants 中必须且只能有一个为 `true`。
- `order`：UI 排序键；相同时按 ID 排序。

当前启动默认 variant 是 `pbr-lite-normal-mapped`。`legacy-forward` 继续保留在
列表首位，用于显式视觉基线和兼容性检查。

所有 stage 路径相对于 `shader/`，使用 `/` 且不得包含 `..`。Manifest 记录
GLSL 路径，例如 `pbr_lite/forward.vert`；运行时 SPIR-V 路径自动成为
`shader/pbr_lite/forward.vert.spv`。

## 新增 Forward Variant

1. 在 `shader/` 的合适子目录新增 GLSL 文件。
2. 在 `programs` 中登记 vertex/fragment 组合和 `main-forward` contract。
3. 在 `variants` 中登记稳定 ID、显示名、类别、Tone Mapping/Bloom 策略和顺序。
4. 执行 Debug 构建。Manifest 变化会触发 CMake 重新配置，新增 Shader
   自动执行 `glslc`、`spirv-val` 和 runtime staging。
5. 启动 VulkanLab，切换到新 variant 并确认 pipeline 能创建和渲染。

兼容现有 Main Forward ABI 的 variant 不需要修改 C++、CMake source list、
Cook 打包列表或 Shader contract test 列表。新的 descriptor、push constant、
vertex layout 或 pass contract 仍需要先扩展对应 C++ 渲染接口和反射规则。

## 内部 Program

Shadow、ToneMap、Bloom Compute 和 Skybox 等内部 Shader 只登记在 `programs`，不放入
`variants`。Renderer 使用稳定 program ID 查询路径；新增 program 不会自动创建
或执行新的 Render Pass。`Debug IBL Diffuse/Specular` 是可选择的 Main Forward
variant，因此同时登记在 `programs` 和 `variants`。

## Runtime Control

`shader.list` 保留旧的 `shaders` 显示名数组，同时返回带
`id/name/category/toneMapping/bloom/default` 的 `entries`。`shader.set` 的现有字符串
先按稳定 ID、再按大小写不敏感的显示名匹配，因此已有 RenderTest spec 无需迁移。

Manifest 缺失、Schema 不支持、ID/显示名重复、默认项错误、路径逃逸、stage
组合非法或 SPIR-V 缺失时，VulkanLab 会在创建窗口和 Vulkan 对象前退出并报告
具体字段。Cook package 同样拒绝无效 Manifest，不使用静态 fallback 清单。

当前不支持运行时热重载、目录自动扫描或第三方 Shader 插件。
