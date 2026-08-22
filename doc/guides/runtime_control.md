# Runtime Control 开发自动化接口

> Status: Current
> Last verified: 2026-08-21
> Verified against: Forward / Deferred Stage 7 working tree

Runtime Control 是面向本机开发、诊断和自动化的可选接口，不是普通用户运行时功能。它通过 Windows Named Pipe 控制已经运行的 VulkanLab，可以查询状态、加载模型预览或 Native Scene、设置环境、相机、Shader 与渲染参数、等待渲染稳定、异步截图并安全退出程序。`scene.list.entries[]` 同时返回 `kind: "modelPreview"` 和 `kind: "nativeScene"`；Native Scene 条目使用稳定 SceneDocument ID，模型预览继续返回兼容 `sceneId`、`modelId`、Catalog profile ID 和纹理限制。

Runtime Control 默认关闭。启用时必须显式传入 `--runtime-control`；Named Pipe 拒绝远程客户端，不开放网络端口。

该服务还必须以 `VKL_ENABLE_RUNTIME_CONTROL=ON` 编译。全功能 Debug/Release 和 `windows-msvc-dev-fast` 包含服务端；`windows-msvc-runtime` 不包含服务端或 VulkanLabCtl，传入 `--runtime-control` 会在 Vulkan 初始化前报错。服务端存在但 Capture 或 Asset Authoring 被独立裁剪时，相关协议方法保留并返回 `feature_not_compiled`。

## 构建与启动

使用 presets 构建：

```powershell
cmake --preset windows-msvc-full
cmake --build --preset windows-msvc-full-debug

cmake --build --preset windows-msvc-full-release
```

每个配置都会生成渲染器和控制客户端：

```text
build/full/run/Debug/VulkanLab.exe
build/full/run/Debug/VulkanLabCtl.exe
build/full/run/Release/VulkanLab.exe
build/full/run/Release/VulkanLabCtl.exe
```

启动默认 endpoint：

```powershell
cd build\full\run\Debug
.\VulkanLab.exe --runtime-control
```

在另一个终端连接：

```powershell
.\VulkanLabCtl.exe ping
```

输出 `pong` 表示控制通道可用。没有传入 `--runtime-control` 时不会创建命令队列、管道或控制线程，客户端连接失败并返回退出码 `2`。

## 多实例隔离

默认 endpoint 为 `\\.\pipe\VulkanLab`。自动化和并行测试应为每个实例指定不同 suffix：

```powershell
.\VulkanLab.exe --runtime-control --runtime-control-pipe suite_a
.\VulkanLabCtl.exe --pipe suite_a ping
```

这组命令使用 `\\.\pipe\VulkanLab.suite_a`。suffix 最长 64 个字符，只允许 ASCII 字母、数字、`-` 和 `_`。渲染器和客户端必须使用同一个 suffix；省略 suffix 时保持默认 endpoint 的兼容行为。

## 命令参考

`--pipe <suffix>` 和 `--json` 是控制工具的全局选项，可放在命令前后。默认输出适合人工阅读，`--json` 输出完整协议响应。

### 程序与场景

```powershell
.\VulkanLabCtl.exe ping
.\VulkanLabCtl.exe info
.\VulkanLabCtl.exe scene list
.\VulkanLabCtl.exe scene current
.\VulkanLabCtl.exe scene load "Main Sponza"
.\VulkanLabCtl.exe scene reload
.\VulkanLabCtl.exe --no-wait scene load "Main Sponza"
.\VulkanLabCtl.exe load status
.\VulkanLabCtl.exe load status 12
.\VulkanLabCtl.exe load cancel 12
.\VulkanLabCtl.exe quit
```

场景名称使用 Catalog 的完整 display name，不区分 ASCII 大小写。`scene list --json` 同时返回兼容的 `scenes` 名称数组和带稳定 `id`、`kind`、`available`、`source` 的 `entries`。Model Preview 还返回 `modelId/profileId/textureLimit`；Native Scene 的这些字段为 null。`scene.current` 对 RuntimeWorld 增加 SceneDocument ID、entity/model/light 数量和 active camera。

`scene.load` 和 `scene.reload` 立即返回 task ID。`VulkanLabCtl` 默认每 100 ms 轮询 `load.status`，直到整个 parse/resolve/prepare/upload/publish operation 完成；`--no-wait` 可关闭客户端等待。Model Preview 的普通 load 使用 Repository Ready cache，reload 创建新的 ModelAsset generation。Native Scene load 解析 SceneDocument、解析唯一 Models/Environment 并事务性发布 RuntimeWorld；reload 重新读取文档但对未失效模型使用 Repository cache。任务响应保留模型预览的 generation/hit/coalesced 字段，并对 Native Scene 增加 document phase、唯一模型数量、Ready 数量和失败 asset ID。

`quit` 的成功响应会先写回并 flush，随后 Application 才退出主循环、停止管道和截图 worker。

### 相机

```powershell
.\VulkanLabCtl.exe camera get
.\VulkanLabCtl.exe camera set `
  --position 2,2,2 `
  --yaw -135 `
  --pitch -30
```

`camera.set` 要求同时提供 position、yaw 和 pitch。所有值必须是有限浮点数；`NaN` 和无穷值会在客户端或服务端被拒绝。设置和 ImGui/输入共用同一个 `Camera` 实例，查询还会返回当前 near/far clip plane。

### 自动化窗口尺寸

```powershell
.\VulkanLabCtl.exe window resize 1024 720
```

`window.resize` 只在渲染器以 `--automation` 启动时可用，合法范围为每个维度 `1..16384`。成功后 GLFW resize callback 会触发现有 swapchain 重建流程；普通交互实例返回 `automation_required`。启用时 `system.info.capabilities` 包含 `window_resize`，协议版本保持 `3`。

### 渲染状态与稳定等待

```powershell
.\VulkanLabCtl.exe render status
.\VulkanLabCtl.exe render wait
.\VulkanLabCtl.exe render wait --stable-frames 8 --timeout-ms 30000
```

`render.status --json` 返回：

- 当前 scene、scene generation、最新 load operation，以及 Native RuntimeWorld 的 SceneDocument/entity/model/light/active camera 摘要；
- package schema、是否为 Native Scene package 和 startup Scene；开发项目中该项显示未打包状态；
- submitted/completed frame serial 与累计 presented frame 数；
- 最近一个已完成 frame 的 `gpuTimings`，包含 available、frameSerial、Atmosphere LUTs/DirectionalShadow/PointShadow/SpotShadow/SkyBackground/MainForward、可选 Bloom、ToneMap、Present + UI 分项与 totalMs；
- 待上传 texture/mesh、in-flight upload batch；
- `modelAssetRepository` 的 Ready/Loading/Failed/Retiring 数量、prepare/build/hit/coalesced 计数和各 generation 的 consumer/资源摘要；
- 当前选择和已发布的 environment，以及环境加载任务；
- 当前 Scene 的 Directional/Point/Spot 数量、按类型与总计的实际上传数量、256 灯上限、每个 frame slot 的 SSBO capacity/总字节、超限数量与最多 32 个 ignored Entity ID，以及 Directional caster、Point/Spot shadow slots、far plane、caster draws和共享 Shadow Map 显存估算；
- `culling` 的 source/visible 数量、frustum/distance/small-object 计数、shadow candidates/culled、depth draws、GPU occluded、Hi-Z mip 数和 indirect capacity；
- `ddgi` 的设备支持、Probe Volume Entity、active 状态、probe/update/ray 数量、更新 cursor、TLAS instance、generation/reset 和显存估算；
- `atmosphere` 的设备 support、active 状态、component/Sun Entity、Sun buffer index、相机高度、静态 LUT ready/dirty、generation、更新时间和不可用原因；
- capture queue 计数，以及 Workspace/Viewport 各自的 capture capability；
- `viewport` 的模式、可见/hover 状态、display extent、render extent 和 resize pending；
- `renderPath` 的 requested/active、View Mode、capability、fallback reason、标准 opaque products、
  GBuffer/Deferred Lighting状态，以及 Cluster grid、reference、overflow和显存；
- GUI 可见性、窗口最小化、swapchain recreate 和 rendering 状态。

`render.wait` 不是服务端阻塞命令。控制工具反复请求 `render.status` 和必要的 `load.status`，要求同一 generation 已完成加载、pending upload 为 0、窗口可渲染，并观察指定数量的新 presented frames。默认等待 8 帧、超时 30 秒；超时返回 `render_wait_timeout`，持续最小化时返回 `window_not_rendering`。

GPU timing 在对应 frame slot 的正常 fence 已完成后读取，不使用 query `WAIT_BIT`，也不增加 queue/device idle。不支持 graphics timestamp 的设备返回 `available=false`；启动后的最初两个 frame 也可能暂时没有已完成结果。

### 异步截图

```powershell
.\VulkanLabCtl.exe capture screenshot suite\renderer-smoke.png --no-gui
.\VulkanLabCtl.exe capture screenshot suite\renderer-smoke-ui.png --include-gui
.\VulkanLabCtl.exe capture status 1
.\VulkanLabCtl.exe capture status 1 --json
.\VulkanLabCtl.exe capture cancel 1
```

`capture screenshot` 校验请求后立即返回 task ID，不等待未来帧、GPU 完成或 PNG 编码。调用者应轮询 `capture status`，直到 `terminal=true`。状态包括 `Queued`、`Recording`、`WaitingForGpu`、`Encoding`、`Cancelling`、`Completed`、`Cancelled` 和 `Failed`。

`--include-gui` 从最终 Swapchain 截取完整 Workspace；`--no-gui` 从 per-frame
Viewport Color 截取纯场景，输出尺寸是实际 Viewport render extent。后者不会丢弃
当前 ImGui frame，因此交互窗口不会闪烁。

完成结果包含：

- source（`Workspace` 或 `Viewport`）、width、height、实际 image format 和 frame serial；
- capture root 下的最终绝对 output path；
- PNG SHA-256；
- recording、GPU wait、CPU copy、encode 和 total timing。

截图路径必须是 capture root 下的非空相对 `.png` 路径。绝对路径、`..` 逃逸、其他扩展名和解析后落在根目录外的路径都会以 `invalid_capture_path` 拒绝。PNG 先写临时文件再原子发布；取消或失败不会留下最终文件。

开发运行默认 capture root 位于 `%LOCALAPPDATA%/VulkanLab/Workspaces/<projectId>/captures/`，可通过 `--workspace <path>` 隔离整套运行数据，或使用 `--capture-root <path>` 只覆盖截图目录。标准 `windows-msvc-runtime` 和 Stage 7 Cooked package均未编译 Runtime Control 或 Capture，因此没有 Named Pipe endpoint；传入 `--runtime-control` 会在 Vulkan 初始化前报错。自定义构建若保留 Runtime Control 但裁剪 Capture，截图协议返回 `feature_not_compiled`。Viewport Color 或 Swapchain source 不支持 8-bit RGBA/BGRA transfer-source 时返回 `capture_unsupported`；另一个来源仍可独立保持可用。

常见截图错误码还有 `capture_queue_full`、`capture_not_found`、`capture_not_cancellable` 和 `capture_failed`。截图路径不会调用 `vkQueueWaitIdle()` 或 `vkDeviceWaitIdle()`；完成状态由正常 frame fence 的 submission serial 推进。

### Render Path、View Mode 与纹理限制

```powershell
.\VulkanLabCtl.exe render-path get
.\VulkanLabCtl.exe render-path set auto
.\VulkanLabCtl.exe render-path set forward
.\VulkanLabCtl.exe render-path set deferred
```

`Auto` 在 Deferred capability可用且当前 View Mode兼容时选择 Deferred，否则回退 Forward并
返回 `fallbackReason`。显式 Forward始终使用 Forward；显式 Deferred不满足设备或 View Mode
契约时返回 `render_path_unsupported`，不会静默改变请求。

切换路径只更新 RenderGraph topology并使相关 temporal history失效，不重建 Device、Swapchain、
MaterialSystem或 PipelineCache。`render-path get` 返回 requested/active、Forward/Deferred支持、
View Mode兼容性、topology hash和 TAA/GTAO/SSR/SSGI history摘要。

```powershell
.\VulkanLabCtl.exe shader list
.\VulkanLabCtl.exe shader current
.\VulkanLabCtl.exe shader set "PBR-lite NormalMapped"

.\VulkanLabCtl.exe texture-limit get
.\VulkanLabCtl.exe texture-limit set 1024
.\VulkanLabCtl.exe texture-limit set full
```

`shader.*` 是兼容保留的命令名，实际操作全局 View Mode，不会替换每个材质的 Shader Family。
名称使用完整 display name，不区分 ASCII大小写；响应以 `viewMode` 返回当前模式。开发模式修改
纹理限制只影响 Model Preview，并触发当前预览的新
加载任务；Native Scene始终使用各 Catalog Model的 import profile。允许值为 `full`、`512`、
`1024` 和 `2048`。CookedOnly profile固定，修改返回 `texture_limit_locked`。

### Environment

```powershell
.\VulkanLabCtl.exe environment list
.\VulkanLabCtl.exe environment current
.\VulkanLabCtl.exe environment set "Studio"
.\VulkanLabCtl.exe environment set None
.\VulkanLabCtl.exe environment reload
.\VulkanLabCtl.exe --no-wait environment set "Studio"
```

`environment list` 返回 `None` 和 Catalog environments，并报告 profile、派生 artifact 状态及设备是否支持 float IBL resources。`environment set` 接受不区分 ASCII 大小写的完整 display name 或稳定 ID；`None` 取消选择并立即回到 fallback resources。选择环境不会自动打开 IBL 或 Skybox。

加载是异步操作。默认客户端拿到 task ID 后通过现有 `load status` 等待 worker KTX2 读取、增量 GPU 上传和 descriptor generation 发布完成；`--no-wait` 只返回初始任务。`load status <task-id>` 与 `load cancel <task-id>` 同时识别 Scene 和 Environment 命名空间。加载失败或取消会保留旧的已发布环境。`environment reload` 要求当前已经选择非 None 环境。

### 阴影、剔除、Screen-Space、IBL、Bloom、曝光与 Tone Mapping

```powershell
.\VulkanLabCtl.exe render-settings get
.\VulkanLabCtl.exe render-settings set --shadows on
.\VulkanLabCtl.exe render-settings set `
  --receiver-bias 0.0015 `
  --constant-bias 1.25 `
  --slope-bias 1.75
.\VulkanLabCtl.exe render-settings set `
  --max-point-shadows 4 `
  --point-shadow-distance 50 `
  --max-spot-shadows 4 `
  --spot-shadow-distance 80
.\VulkanLabCtl.exe render-settings set `
  --exposure 1.0 `
  --tone-mapper aces
.\VulkanLabCtl.exe render-settings set `
  --ibl on `
  --skybox on `
  --environment-intensity 1.25 `
  --environment-rotation-deg 90
.\VulkanLabCtl.exe render-settings set `
  --bloom on `
  --bloom-threshold 1.0 `
  --bloom-soft-knee 0.5 `
  --bloom-intensity 0.1
.\VulkanLabCtl.exe render-settings set `
  --frustum on `
  --shadow-culling on `
  --shadow-distance 200 `
  --distance-culling off `
  --max-draw-distance 1000 `
  --small-object-culling off `
  --min-projected-pixels 1 `
  --occlusion on `
  --occlusion-bias 0.0005
.\VulkanLabCtl.exe render-settings set `
  --surface-debug motion `
  --surface-motion-scale 32
.\VulkanLabCtl.exe render-settings set `
  --ao ssao `
  --ssao-quality high `
  --ssao-radius 0.5 `
  --ssao-bias 0.025 `
  --ssao-intensity 1.0 `
  --ssao-power 1.5
.\VulkanLabCtl.exe render-settings set `
  --ao cacao `
  --cacao-quality high `
  --cacao-resolution half `
  --cacao-radius 1.2 `
  --cacao-intensity 1.0 `
  --cacao-power 1.5
.\VulkanLabCtl.exe render-settings set `
  --screen-space-debug cacao-output `
  --screen-space-debug-mip 0
.\VulkanLabCtl.exe render-settings set `
  --ao gtao `
  --gtao-quality medium `
  --gtao-radius 1.0 `
  --gtao-falloff 0.85 `
  --gtao-intensity 1.0 `
  --gtao-power 1.5 `
  --gtao-temporal-weight 0.9
.\VulkanLabCtl.exe render-settings set `
  --taa taa `
  --taa-history-weight 0.9 `
  --taa-sharpness 0.1
.\VulkanLabCtl.exe render-settings set `
  --screen-space-debug taa-rejection
.\VulkanLabCtl.exe render-settings set `
  --reflection ssr `
  --ssr-quality medium `
  --ssr-max-distance 50 `
  --ssr-thickness 0.15 `
  --ssr-max-roughness 0.8 `
  --ssr-intensity 1.0 `
  --ssr-history-weight 0.9
.\VulkanLabCtl.exe render-settings set `
  --screen-space-debug ssr-confidence
.\VulkanLabCtl.exe render-settings set `
  --gi ssgi `
  --ssgi-quality medium `
  --ssgi-max-distance 12 `
  --ssgi-thickness 0.25 `
  --ssgi-intensity 1.0 `
  --ssgi-radiance-clamp 10 `
  --ssgi-history-weight 0.9
.\VulkanLabCtl.exe render-settings set `
  --gi ddgi `
  --ddgi-radiance-clamp 10 `
  --ddgi-debug none
.\VulkanLabCtl.exe render-settings set `
  --gi ssgi-ddgi
.\VulkanLabCtl.exe render-settings set `
  --screen-space-debug ssgi-variance
```

`render-settings set` 支持部分更新，并要求至少提供一个选项。`--shadows`、`--ibl`、`--skybox` 和 `--bloom` 接受 `on/off`，`--tone-mapper` 接受 `aces`、`reinhard` 或 `passthrough`。Receiver bias 范围为 `[0, 0.05]`，`--point-receiver-bias`/`pointShadowReceiverBiasWorld` 使用世界单位且范围为 `[0,1]`，constant/slope bias 为 `[0, 10]`；Point/Spot shadow 数量范围均为 `[0,4]`，距离范围均为 `[0.1,1000]`。exposure 为 `[-10, 10]` EV，environment intensity 为 `[0, 100]`；Bloom threshold、soft knee 和 intensity 分别为 `[0,20]`、`[0,1]` 和 `[0,5]`。CLI 用 degree 表示 rotation，协议字段 `environmentRotationRadians` 使用弧度；服务端将其规范化到一个完整旋转。

剔除开关同样只修改当前会话。Frustum、Shadow Culling 和受支持设备上的 Hi-Z Occlusion 默认开启；Distance 与 Small Object 默认关闭。`shadowDistance`、`maxDrawDistance`、`minProjectedSizePixels` 和 `occlusionDepthBias` 的协议范围分别为 `[0.1,1000]`、`[0.1,1000000]`、`[0,256]` 和 `[0,0.05]`。`render-settings get` 额外返回 `occlusionAvailable/Active/UnavailableReason`；不支持 compute、sampled depth 或 `R32_SFLOAT` storage 的设备尝试开启时返回 `occlusion_unsupported`。

`--surface-debug` 接受 `none`、`normal`、`roughness`、`motion` 和 `history-validity`。Motion 显示比例由 `--surface-motion-scale` 控制，范围为 `[0.1,1024]`。Surface Data 不可用时启用非 `none` 调试视图会返回 `surface_data_unsupported`；`render-settings get` 和 `render status` 同时报告支持状态、激活状态、history generation、有效 item 数和最近失效原因。

`--ao` 接受 `off/ssao/cacao/gtao`。SSAO quality 接受 `low/medium/high`，分别对应 8、16 和 32 个样本；radius、bias、intensity 和 power 的范围分别为 `[0.05,10]`、`[0,0.2]`、`[0,4]` 和 `[0.25,4]`。CACAO 只在 `windows-msvc-ao-compare` 中可用，quality 接受 `lowest/low/medium/high/highest`，resolution 接受 `native/half`，radius、intensity 和 power 分别使用 `[0.05,10]`、`[0,4]` 和 `[0.25,4]`。切换 resolution 会等待现有 frame fences 后事务重建 CACAO contexts，不调用 device idle。GTAO quality 接受 `low/medium/high`，radius、falloff、intensity、power 和 temporal weight 范围分别为 `[0.05,10]`、`[0,0.99]`、`[0,4]`、`[0.25,4]` 和 `[0,0.99]`。

只有支持 Screen-Space 的 Default Lit材质路径会把当前 active AO乘入间接光；Legacy、Debug、透明与 transmission材质保持原行为。`--taa` 接受 `off/taa`，history weight 与 sharpness范围分别为 `[0,0.99]` 和 `[0,1]`。TAA默认关闭，发生 camera cut、resize、scene/View Mode/Render Path/camera mode切换或执行序列不连续时自动重置 history。

`--reflection` 接受 `ibl-only/ssr`，`--ssr-quality` 接受 `low/medium/high`；max distance、thickness、max roughness、intensity 和 history weight 的范围分别为 `[0.1,1000]`、`[0.001,5]`、`[0,1]`、`[0,4]` 和 `[0,0.99]`。SSR 默认关闭，只对支持 Screen-Space 的 Lit材质路径生效；miss 和低 confidence 区域回退当前 IBL/constant ambient baseline。SSR history 与 TAA/GTAO 独立，并响应相同的 camera cut、resize、scene/View Mode/Render Path/camera mode 失效事件。

`--gi` 接受 `ambient-or-ibl/ssgi/ddgi/ssgi-ddgi`，`--ssgi-quality` 接受 `low/medium/high`；max distance、thickness、intensity、radiance clamp 和 history weight 的范围分别为 `[0.05,1000]`、`[0.001,10]`、`[0,4]`、`[0.1,100]` 和 `[0,0.99]`。SSGI 默认关闭，只对支持 Screen-Space 的 Lit材质路径生效；miss 与低 confidence 区域回退 DDGI 或当前 ambient/IBL baseline。SSGI history 与 TAA、GTAO、SSR 分离，并响应相同的 camera cut、resize、scene/View Mode/Render Path/camera mode 失效事件。

`ddgi` 和 `ssgi-ddgi` 还要求当前 Native Scene 存在一个有效的 DDGI Probe Volume、当前设备支持 Vulkan Ray Query/Acceleration Structure 与所需 storage image format，并使用支持 DDGI 的 Lit材质路径。`--ddgi-radiance-clamp` 范围为 `[0.1,100]`；`--ddgi-debug` 接受 `none/irradiance/distance/classification`。组合模式把 DDGI 作为屏幕外低频 diffuse baseline，并用 SSGI confidence 覆盖近场屏幕空间结果。`render-settings get` 与 `render.status.ddgi` 会报告实际 active 状态、不可用原因、TLAS instance 和 probe update 进度。

`--screen-space-debug` 接受 `none`、`nearest-depth`、`scene-color`、`ssao-raw`、`ssao-filtered`、`cacao-output`、`gtao-raw`、`gtao-temporal`、`gtao-filtered`、`gtao-rejection`、`gtao-history-weight`、`taa-history`、`taa-rejection`、`taa-history-weight`、`ssr-raw`、`ssr-temporal`、`ssr-filtered`、`ssr-confidence`、`ssr-rejection`、`ssgi-raw`、`ssgi-temporal`、`ssgi-filtered`、`ssgi-confidence`、`ssgi-variance` 和 `ssgi-rejection`，mip 范围为 `[0,31]` 并在 shader 中限制到实际 mip。Surface 与 Screen-Space Debug 互斥；同一 patch 同时请求两个非 `none` 模式时返回 `conflicting_debug_views`，分开切换时新模式会关闭旧模式。

`render.status.screenSpace` 返回 depth/color pyramid、SSAO、CACAO、GTAO、TAA、SSR 与 SSGI 的支持状态、AO/GI requested/active 状态、Debug View、资源 extent、mip 数、各 temporal effect 的 history generation/validity、最近 reset 原因、CACAO generation/precision 和估算显存。SSAO/CACAO/GTAO/TAA/SSR/SSGI 不支持时尝试启用分别返回 `ssao_unsupported`、`cacao_unsupported`、`gtao_unsupported`、`taa_unsupported`、`ssr_unsupported` 或 `ssgi_unsupported`；请求不可用的调试资源时返回 `screen_space_unsupported`。

Tone Mapping policy 由 Shader Manifest 的 View Mode决定：Lit和 `Debug IBL Diffuse/Specular` 可配置，Legacy与其他 Debug View Mode强制 PassThrough。Bloom compatibility也由 View Mode决定；设置会保留，但不兼容模式下 `bloomActive=false`。`render-settings get` 返回 `bloomAvailable`、`bloomActive`、`bloomUnavailableReason` 和四个 Bloom设置。设备不满足 compute/`RGBA16F` storage image要求时，尝试开启会返回 `bloom_unsupported`。

阴影会影响被选择的 Directional CSM caster，以及最多四盏 Point 和四盏 Spot。`render.status.lighting` 返回 ShadowSystem revision/reactive 状态，以及每盏 punctual shadow light 的 policy、稳定 key、Entity、slot、score、slot age、retained/selected、实际 far plane、caster draw 数和 Point 六个 face 分项；`render.status.culling` 返回四个 cascade、24 个 point face 和四个 spot slot 的 draw 统计。IBL 只在环境已发布且开关开启时替代 PBR 的 constant ambient；Skybox 开关独立。Native Scene 的 Atmosphere 与 Atmosphere Sun 来自只读 SceneDocument/RuntimeWorld，Runtime Control v3 只报告状态，不提供大气参数 mutation。UI 的 `Render -> Common/Post Processing/Lighting/Culling` 与 Runtime Control 修改同一个 `RenderSettings` 对象。

### 派生资产

```powershell
.\VulkanLabCtl.exe asset catalog
.\VulkanLabCtl.exe asset status "Main Sponza"
.\VulkanLabCtl.exe asset import "Main Sponza"
.\VulkanLabCtl.exe --force asset import "Main Sponza"
.\VulkanLabCtl.exe --load-after asset import "Main Sponza"
.\VulkanLabCtl.exe --no-wait asset import "Main Sponza"
.\VulkanLabCtl.exe asset cancel 9223372036854775808
.\VulkanLabCtl.exe asset cache-info
```

这些 scene 写操作只在 `--asset-mode ondemand` 下可用。ReadOnly/CookedOnly 返回 `asset_import_disabled`；查询 Catalog、状态和 cache 仍可用。Runtime Control 不接收任意本地模型/HDR 路径，注册新源文件仍使用 ImGui 导入器或 `VulkanLabAssetTool catalog add`/`catalog add-environment`。Environment 派生缓存的 Build/Rebuild 目前由 Assets UI 或 AssetTool 完成，不由 Runtime Control 自动 bake。

Catalog glTF 的最近验证报告可只读查询；响应最多包含 32 条 issue，完整报告仍从 Assets/Scenes UI 打开：

```powershell
.\VulkanLabCtl.exe asset validation main-sponza
.\VulkanLabCtl.exe --json asset validation sheen-chair
```

响应包含 `Valid/Warnings/Invalid/Stale/Unavailable/Failed/NotChecked/NotApplicable` 状态、Validator 版本、四级计数、renderer extension 诊断和 report path。该方法只接受 Catalog model ID 或完整显示名，不接受原生 SceneDocument ID 或任意外部文件路径；响应同时保留 `sceneId` 并增加 `modelId` 与 `assetKind: "model"`。

### 加载统计

```powershell
.\VulkanLabCtl.exe stats
.\VulkanLabCtl.exe stats --json
```

完整 JSON 包含阶段耗时、资源数量、decode/transcode/upload 字节、KTX2 cache 命中、增量上传同步数据和 VMA 前后快照。没有完成过场景加载时返回 `no_load_stats`。

## 自动化示例

下面的 PowerShell 示例启动隔离实例、等待场景和渲染稳定、截图并退出：

```powershell
$runtime = Resolve-Path .\build\full\run\Release
$suffix = "smoke_$PID"
$captureRoot = Join-Path $PWD "out\runtime-control\smoke-$PID"
$app = Start-Process `
  -FilePath "$runtime\VulkanLab.exe" `
  -WorkingDirectory $runtime `
  -ArgumentList @(
    '--runtime-control',
    '--runtime-control-pipe', $suffix,
    '--automation',
    '--window-size', '800x600',
    '--fixed-delta', '0.016666667',
    '--no-gui',
    '--capture-root', $captureRoot
  ) `
  -PassThru

try {
  & "$runtime\VulkanLabCtl.exe" --pipe $suffix ping
  & "$runtime\VulkanLabCtl.exe" --pipe $suffix scene load "Renderer Smoke Scene"
  & "$runtime\VulkanLabCtl.exe" --pipe $suffix camera set `
    --position 2,2,2 --yaw -135 --pitch -30
  & "$runtime\VulkanLabCtl.exe" --pipe $suffix render wait `
    --stable-frames 8 --timeout-ms 30000

  $capture = & "$runtime\VulkanLabCtl.exe" --pipe $suffix --json `
    capture screenshot "suite\renderer-smoke.png" --no-gui | ConvertFrom-Json
  $taskId = $capture.result.taskId

  do {
    Start-Sleep -Milliseconds 50
    $status = & "$runtime\VulkanLabCtl.exe" --pipe $suffix --json `
      capture status $taskId | ConvertFrom-Json
  } until ($status.result.terminal)

  if (-not $status.ok -or $status.result.state -ne 'Completed') {
    throw "Capture failed: $($status | ConvertTo-Json -Depth 8)"
  }
} finally {
  & "$runtime\VulkanLabCtl.exe" --pipe $suffix quit
  $app.WaitForExit()
}
```

日常 smoke/golden 测试应使用已经固化这段编排的 `VulkanLabRenderTest`；手写脚本仍需自行检查每条命令的 `$LASTEXITCODE`。规格和报告说明见[自动视觉回归](visual_regression.md)。

## 返回码

| 返回码 | 含义 |
|---:|---|
| `0` | 命令成功。 |
| `1` | 已连接渲染器，但命令被拒绝、任务失败或超时。 |
| `2` | 参数、连接或消息协议错误。 |

## 协议摘要

Runtime Control 当前协议版本为 `3`。客户端连接目标 Named Pipe 后，先发送 little-endian `uint32` JSON 字节长度，再发送 UTF-8 JSON；响应使用同样 framing。单条消息最大 64 KiB，每次连接处理一条请求。

```json
{
  "id": 1,
  "method": "capture.screenshot",
  "params": {
    "path": "suite/renderer-smoke.png",
    "includeGui": false
  }
}
```

成功响应统一为：

```json
{
  "id": 1,
  "ok": true,
  "result": {
    "taskId": 1,
    "state": "Queued",
    "terminal": false
  }
}
```

失败响应统一为：

```json
{
  "id": 1,
  "ok": false,
  "error": {
    "code": "invalid_capture_path",
    "message": "Capture output must be a relative PNG path."
  }
}
```

管道线程只负责 framing、JSON 解析、排队和回写。所有 Scene、Camera、Shader、统计和 Vulkan 相关操作都由 Application 主线程执行；服务端不会用控制命令阻塞等待未来帧。
