# 编辑器 Docking 与 Scene Viewport

> Status: Current
> Last verified: 2026-08-07
> Verified against: Local Reflection Probes v1

VulkanLab 的开发 UI 使用 Dear ImGui `v1.92.7-docking`，在主 GLFW
窗口内创建一个全屏 DockSpace。Viewport、Outliner、Inspector、Scenes、Assets、
Render、Materials 和 Diagnostics 是可以停靠、合并为 Tab、浮动或隐藏的独立窗口。

当前没有启用 ImGui Multi-viewports，不会为工具窗口创建额外的操作系统窗口。

## 默认布局

首次运行时，宽度不低于 `1100px` 使用 Viewport preset：

```text
┌ View / Layout ─── Scene ─── FPS / GPU / Loading ┐
│ Outliner        │      Viewport        │Inspector│
│ Scenes / Assets │                      │Render/Mat│
└─────────────────┴──────────────────────┴─────────┘
```

- Outliner 位于左上，Scenes 与 Assets 位于左下同一节点。
- Inspector 位于右上，Render 与 Materials 位于右下同一节点。
- Diagnostics 默认隐藏，但已经停靠到左侧节点，可通过 View 菜单打开。
- Viewport 默认停靠在中央节点，显示 Renderer 生成的 per-frame Viewport Color。
- 窗口宽度小于 `1100px` 时首次使用 Compact preset，只显示 Scenes 和 Render
  两个侧栏 Tab；Assets、Materials 和 Diagnostics 可从 View 菜单按需打开。

左右面板使用目标像素宽度和上下限计算，常见窗口下 Render 属性标签不会因
面板过窄而被默认裁切，超宽屏下也不会无上限占用场景区域。

用户拖动后的节点、Tab 和浮动窗口状态由 `imgui.ini` 保存。程序不会在普通窗口
resize 时重排用户布局。DockSpace v3 会执行一次布局迁移；删除现有 ini 或选择
布局 preset 可以恢复预设结构。

`Layout` 提供三种 preset：

- `Viewport`：左右工具栏和最大化的中央 Viewport，Diagnostics 默认隐藏。
- `Debugging`：在 Viewport 下方增加约 28% 高度的 Diagnostics。
- `Compact`：所有工具都预先停靠在单个侧栏，默认只打开 Scenes 和 Render。

## 菜单与状态

DockSpace 顶部菜单栏提供：

- `File`：New/Open/Save/Save As/Close Native Scene，以及把当前模型预览转换为 SceneDocument。
- `Edit`：Undo/Redo。
- `View`：显示或隐藏 Viewport、Outliner、Inspector、Scenes、Assets、Render、Materials 和 Diagnostics。
- `Layout -> Viewport/Debugging/Compact`：应用指定工作区 preset。
- `Layout -> Reset Current Layout`：恢复当前 preset 的默认节点结构。
- 状态区：当前 Scene、FPS、可用时的 GPU frame time，以及活跃加载任务的
  阶段和完成百分比。

Dock Tab 不显示关闭按钮，减少窄节点中的标签占用。窗口显隐统一由 View 菜单
控制。

## 视觉与控件约定

Editor 使用中性深灰主题和 Windows Segoe UI，字体与间距按 GLFW content scale
初始化。蓝色只用于当前选择和交互焦点；绿色、黄色、红色和青色分别表示 Ready、
Stale/Warning、Error 和 Loading。

`EditorWidgets` 统一提供属性表、状态点、路径值、空状态和分段模式选择。长路径在
容器中裁剪，悬停显示完整值，右键复制。UI helper 不持有 Application、Scene 或
Vulkan 对象。

## 工具窗口

### Scenes

Scenes 只显示 Catalog v3 的 Native Scene Documents。双击或点击 Load 会启动事务式
Native Scene 加载；当前文档在列表中标记为 Open。加载期间旧 World 继续渲染，详细阶段、
模型解析进度和 Cancel 位于窗口底部。

### Assets

Assets 包含 `Models`、`Environments` 与 `Jobs / Cache` 三个 Tab。Models 管理导入、
Validator、预览、重导入和模型派生资源；Environments 管理 HDR 导入及 IBL artifact；
Jobs / Cache 显示项目 cache、当前任务、取消、日志和历史。

打开 Native Scene 后，可实例化的 glTF 模型行可以直接拖入 Viewport。拖动 payload
只保存稳定 `modelId`；释放后创建 root Model Entity，并复用 AssetRepository 的异步
绑定与共享 GPU 资源。builtin、缺失模型或没有 Native Scene 会话时不会提供拖放。

### Outliner 与 Inspector

Outliner 显示 Native Scene 的实体层级，支持搜索、单选、创建 Empty/Model/三类 Light/
Camera/Sky Atmosphere/Reflection Probe、重命名、enabled、复制和删除 subtree。一个 Scene 最多创建一个
Sky Atmosphere，且只能位于 Scene Root。Inspector 的 Entity 页编辑 parent、局部
Translation/Euler Rotation/Scale，以及 Model、Light、Camera、Atmosphere component；Scene 页编辑
ambient、environment 和 active camera。Parent picker 使用 Keep Local。

Outliner 的 Entity 可以拖到另一个 Entity，或拖到显式 `Scene Root` 行解除父子关系。
这条拖放路径使用 Keep World：系统先保存旧 world matrix，再计算并分解新 local TRS。
如果父级非均匀缩放与旋转形成无法用 TRS 表示的 shear，操作以
`transform_not_decomposable` 原子失败，原层级与 Transform 保持不变。

删除 active Camera 或移除其 Camera component 会被拒绝，必须先指定另一台 Camera。
Directional、Point 和 Spot 共享 256 盏有效灯光上限。超限灯仍保存在文档中，并按上一帧 RenderView 的精确结果标记为 Not uploaded。Directional Inspector 可设置 `Casts Shadow`，并显示 Active/Eligible/Disabled；Point/Spot shadow 显示 Unsupported。

Directional Light Inspector 还提供 `Use as Atmosphere Sun` 和太阳角半径。设置新 Sun 会在同一条 Undo 命令中清除旧 Sun。Atmosphere Inspector 提供 Earth Preset、基础行星/空气参数和 Advanced 散射参数；Atmosphere Entity 禁止 reparent、rotate、scale、duplicate，删除时会同步解除 Sun 绑定。

Reflection Probe Inspector 提供 Environment、Box/Sphere shape、extents/radius、blend distance、priority、intensity、box projection 和 capture offset。`Capture and Bake` 会临时以探针位置和六个固定 90° 方向依次捕获 256×256 线性 HDR，拼接为 2:1 RGBE HDR，并调用现有 EnvironmentAssetTool 离线生成 KTX2。只有全部 bake/upload 成功后才通过一条 Editor 命令绑定新 Environment；失败时保留旧探针资产和场景状态。该操作是显式的，不会因移动物体或灯光而自动重新捕获。

### Render

Render 使用六个折叠区：

- `Common`：Shader variant、Texture Limit、Exposure EV 和 Tone Mapper。
- `Post Processing`：Bloom 开关、Intensity 和状态；Threshold/Soft Knee 放入默认
  折叠的 Bloom Tuning。
- `Lighting`：阴影、ambient、场景灯光、fallback Sun、IBL/Skybox、Reflection Probe 数量/Repository/capture 状态，以及 Atmosphere support、active Sun、相机高度和 LUT generation/dirty/update 状态。
- `Culling`：Camera/Shadow/Distance/Small Object/Hi-Z Occlusion 开关、阈值、CPU/GPU 可见性统计和 indirect buffer 容量。
- `Surface Data`：Normal、Roughness、Motion、History Validity 调试视图，以及 history generation、有效 item 数、失效原因和 per-frame buffer 容量。
- `Camera & Clip`：位置、移动速度、near/far clip plane 和 Scene bounds，默认折叠。

Shadow bias 放入默认折叠的 Shadow Tuning；场景灯光数量和逐灯数据放入 Light
Diagnostics。不可用功能保留控件位置，并通过禁用状态或 tooltip 说明原因。

这些控件继续直接修改 Application 当前渲染状态，与 Runtime Control 共用同一
设置入口。

### Materials

Materials 仍是只读诊断窗口。模型预览时显示整个预览的材质；Native Scene 中优先
显示 Outliner 当前选中 ModelInstance 所共享的 ModelAsset 材质。

### Diagnostics

Diagnostics 保留内部 Tabs：

- `Performance`：摘要属性、最近 180 帧 FPS/GPU 时间曲线，以及 GPU Pass 占比条。
- `Load Stats`：最近加载的阶段耗时、资源、上传、cache 和 VMA 数据。
- `Capture`：通过 `Viewport | Workspace` 分段控件选择来源，并显示任务状态、
  输出路径和错误。

## Viewport 与相机输入

Viewport 是独立的场景渲染目标，不是 Swapchain 背景的透传区域。HDR、Depth、
Bloom 和 tone-mapped Viewport Color 按 Viewport 内容区的物理像素 1:1 创建；
相机 aspect ratio 每帧直接使用当前内容区比例。

Viewport 图像上方保留一行只读状态，显示当前 render extent；资源 debounce
期间显示 `Resizing...`。

拖动 Dock 分隔线时，ImGui 临时缩放上一张有效图像。尺寸稳定 120 ms 后，Renderer
等待全部 frame fence，重新创建 viewport-dependent image、framebuffer 和 descriptor；
该过程不重建 Swapchain、不清空 Pipeline Cache，也不调用 `vkDeviceWaitIdle()`。
首次出现有效尺寸会在下一帧立即应用。隐藏、折叠或零尺寸时继续使用最后一个
有效 render extent。

Native Scene 的 Viewport toolbar 可在 `Editor Camera` 与 `Active Camera` 间切换。
Active Camera 直接使用场景 Camera component，且不会响应 FPS 相机输入。

Toolbar 的 `Q/W/E/R` 分别切换 Select、Translate、Rotate 和 Scale；Translate/Rotate
支持 Local/World，Scale 固定使用 Local。左键点击使用 CPU ray 与 Ready ModelInstance
的 asset-space bounds 求交，选择最近命中的实体；点击空白清除选择。选中模型绘制 bounds
线框，Light、Camera 和 Empty Entity 显示 pivot 标记并继续通过 Outliner 选择。

Sky Atmosphere 只允许使用 Translate Gizmo移动地表原点；Rotate 和 Scale 被禁用，相关约束也由 RuntimeWorld 与 SceneDocument validator 强制执行。

Gizmo 拖动开始时捕获一次 SceneDocument before 状态，拖动期间直接更新 RuntimeWorld，
释放时只生成一条 Undo 命令。`Escape` 取消并恢复 before 状态；隐藏 Viewport、切换选择
或重载场景也会取消未完成操作。Active Camera 模式下不能直接操纵当前 active Camera，
需要先切回 Editor Camera。

右键相机模式只能从实际 Viewport 图像区域启动。在标题栏、Tab、工具窗口、菜单、
modal 或 docking 拖动目标上按右键不会进入相机模式。进入 CameraDrag 后仍使用
现有 GLFW 鼠标捕获和 `W/S/A/D/Q/E` 移动逻辑。

模型拖入 Viewport 时优先与 `Z=0` 平面求交；相机射线没有有效正向地面交点时，使用穿过
当前选择、Scene bounds center 或原点的相机朝向平面。拖动期间只显示落点与模型名称，
释放后创建实体，不绘制临时模型副本。

## 截图与无 UI 构建

- `includeGui=true` 从最终 Swapchain 截图，包含菜单栏和当前 Docking 工作区。
- `includeGui=false` 从 Viewport Color 截图，输出纯场景和实际 Viewport 原生分辨率；
  当前 ImGui frame 不会被丢弃，用户窗口不会闪烁。
- `--no-gui` 不创建 ImGui Context 或 DockSpace，PresentPass 将 Viewport Color
  fullscreen 显示到 Swapchain。
- `VKL_ENABLE_EDITOR_UI=OFF` 不编译 EditorDockWorkspace，也不链接 ImGui，仍保留
  同一条离屏 Viewport 渲染和 fullscreen present 路径。

当前只支持一个 Scene Viewport 和单选。Picking 使用 ModelAsset bounds，不提供 primitive
选择、重叠对象循环选择或 GPU Object-ID Pass；Gizmo 暂无 snapping。可调 render scale、
outline postprocess 和 ImGui Multi-viewports 尚未实现。

## 文件与快捷键

- `Ctrl+N/O/S`：New/Open/Save。
- `Ctrl+Shift+S`：Save As。
- `Ctrl+Z/Y`：Undo/Redo。
- `Ctrl+D`：复制选中 subtree。
- `Delete`：删除选中 subtree。
- `F2`：重命名选中实体。
- `Q/W/E/R`：Viewport Select/Translate/Rotate/Scale。

切换场景、关闭或退出时，Dirty 会话提供 Save/Discard/Cancel。保存使用加载时的 file
stamp；外部修改冲突只允许 Reload、Save As 或 Cancel，不提供强制覆盖。文本输入活跃
或 modal 打开时不会触发实体快捷键。
