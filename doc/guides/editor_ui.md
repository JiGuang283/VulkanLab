# 编辑器 Docking 与 Scene Viewport

> Status: Current
> Last verified: 2026-08-01
> Verified against: Viewport v2 working tree

VulkanLab 的开发 UI 使用 Dear ImGui `v1.92.7-docking`，在主 GLFW
窗口内创建一个全屏 DockSpace。Viewport、Scenes、Assets、Render、Materials
和 Diagnostics 是可以停靠、合并为 Tab、浮动或隐藏的独立窗口。

当前没有启用 ImGui Multi-viewports，不会为工具窗口创建额外的操作系统窗口。

## 默认布局

首次运行时，或选择 `Layout -> Reset Layout` 后，桌面布局为：

```text
┌ View / Layout ─── Scene ─── FPS / GPU / Loading ┐
│ Scenes + Assets │      Viewport        │ Render  │
│                 │                      │ Material│
├─────────────────┴──────────────────────┴─────────┤
│                  Diagnostics                     │
└──────────────────────────────────────────────────┘
```

- Scenes 和 Assets 默认停靠在左侧同一节点。
- Render 和 Materials 默认停靠在右侧同一节点。
- Diagnostics 默认位于中央区域下方。
- Viewport 默认停靠在中央节点，显示 Renderer 生成的 per-frame Viewport Color。
- 窗口宽度小于 `1200px` 时使用紧凑布局，Diagnostics 与 Scenes/Assets
  合并到左侧节点，不再占用底部空间。

左右面板使用目标像素宽度和上下限计算，常见窗口下 Render 属性标签不会因
面板过窄而被默认裁切，超宽屏下也不会无上限占用场景区域。

用户拖动后的节点、Tab 和浮动窗口状态由 `imgui.ini` 保存。旧版单窗口
`VulkanLab` 记录不会被新窗口 ID 使用。删除现有 ini 或执行 Reset Layout
都可以恢复默认布局。

## 菜单与状态

DockSpace 顶部菜单栏提供：

- `View`：显示或隐藏 Viewport、Scenes、Assets、Render、Materials 和 Diagnostics。
- `Layout -> Reset Layout`：重新打开全部工具窗口并恢复默认节点结构。
- 状态区：当前 Scene、FPS、可用时的 GPU frame time，以及活跃加载任务的
  阶段和完成百分比。

关闭某个 dock Tab 后，可以始终通过 `View` 菜单恢复，不会出现无法重新打开
工具窗口的情况。

## 工具窗口

### Scenes

Scenes 保留搜索、选择和加载 Catalog scene，以及导入、重导入、source
fallback、保存相机和移除操作。活跃场景任务的详细阶段、资源进度和 Cancel
位于同一窗口底部；没有任务时不显示空加载区。

Import 和 Remove 继续使用 modal，Docking 不改变 Validator、Catalog
transaction、Native BC7 import 或异步 Scene load 数据流。

### Assets

Assets 显示项目、Catalog、cache、artifact 状态、资产导入进度、取消、日志和
任务历史。该窗口也管理 HDR environment 的导入、派生资源 Build/Rebuild、
Cancel 和 Remove。

### Render

Render 使用四个折叠区：

- `Pipeline`：Shader variant、Texture Limit、Exposure EV 和 Tone Mapper。
- `Post Processing`：Compute Bloom 参数与 Available/Active 状态。
- `Lighting`：阴影、ambient、场景灯光、fallback Sun 和 IBL/Skybox。
- `Camera`：位置、移动速度、near/far clip plane 和 Scene bounds。

这些控件继续直接修改 Application 当前渲染状态，与 Runtime Control 共用同一
设置入口。

### Materials

Materials 仍是只读诊断窗口，使用搜索、材质列表和单项详情显示 Surface、PBR、
Textures 与 Derived Render State。切换 Scene generation 后选择会重置到有效
材质。

### Diagnostics

Diagnostics 保留内部 Tabs：

- `Performance`：FPS、对象数量、输入模式、Tracy 状态和 GPU Pass Timings。
- `Load Stats`：最近加载的阶段耗时、资源、上传、cache 和 VMA 数据。
- `Capture`：截图请求、GUI inclusion、任务状态、输出路径和错误。

## Viewport 与相机输入

Viewport 是独立的场景渲染目标，不是 Swapchain 背景的透传区域。HDR、Depth、
Bloom 和 tone-mapped Viewport Color 按 Viewport 内容区的物理像素 1:1 创建；
相机 aspect ratio 每帧直接使用当前内容区比例。

拖动 Dock 分隔线时，ImGui 临时缩放上一张有效图像。尺寸稳定 120 ms 后，Renderer
等待全部 frame fence，重新创建 viewport-dependent image、framebuffer 和 descriptor；
该过程不重建 Swapchain、不清空 Pipeline Cache，也不调用 `vkDeviceWaitIdle()`。
首次出现有效尺寸会在下一帧立即应用。隐藏、折叠或零尺寸时继续使用最后一个
有效 render extent。

右键相机模式只能从实际 Viewport 图像区域启动。在标题栏、Tab、工具窗口、菜单、
modal 或 docking 拖动目标上按右键不会进入相机模式。进入 CameraDrag 后仍使用
现有 GLFW 鼠标捕获和 `W/S/A/D/Q/E` 移动逻辑。

## 截图与无 UI 构建

- `includeGui=true` 从最终 Swapchain 截图，包含菜单栏和当前 Docking 工作区。
- `includeGui=false` 从 Viewport Color 截图，输出纯场景和实际 Viewport 原生分辨率；
  当前 ImGui frame 不会被丢弃，用户窗口不会闪烁。
- `--no-gui` 不创建 ImGui Context 或 DockSpace，PresentPass 将 Viewport Color
  fullscreen 显示到 Swapchain。
- `VKL_ENABLE_EDITOR_UI=OFF` 不编译 EditorDockWorkspace，也不链接 ImGui，仍保留
  同一条离屏 Viewport 渲染和 fullscreen present 路径。

当前只支持一个 Scene Viewport。对象拾取、Gizmo、可调 render scale 和 ImGui
Multi-viewports 尚未实现。
