# 编辑器工作区与 Scene Viewport

> Status: Current
> Last verified: 2026-08-21
> Verified against: Forward / Deferred Stage 7 working tree

VulkanLab 的开发编辑器基于 Dear ImGui docking branch。当前界面按三类主要工作流组织：

```text
Scene    -> 场景层级、Viewport、Inspector、Content Browser
LookDev  -> 材质、灯光、环境和渲染效果
Debug    -> RenderGraph、性能、资源和任务诊断
```

编辑器仍使用单个操作系统窗口和单个 Scene Viewport，不启用 ImGui Multi-viewports。

## 工作区

`Workspace` 菜单提供以下预设：

- `Scene`：左侧 Outliner 和 Content Browser，中央 Viewport，右侧 Inspector。
- `LookDev`：左侧 Content Browser，中央 Viewport，右侧 Inspector、Render 和 Materials。
- `Debug`：Render、Viewport、Inspector 为主要区域，底部 Diagnostics 默认展开。
- `Compact`：工具页合并到单个侧栏，适合窄窗口。

窗口首次运行时会根据可用宽度选择 `Scene` 或 `Compact`。`Reset Current Layout` 可恢复当前预设。`Ctrl+Space` 临时最大化或恢复 Viewport。

Scene 和 LookDev 的底部区域默认只显示状态栏。状态栏包含当前场景、Dirty 标记、活动任务、FPS、GPU frame time 和错误数量。点击左侧抽屉按钮可展开 `Tasks / Performance / Load Stats / Capture`，高度可在 `180-360px` 之间拖动。

## 本机偏好

编辑器状态不写入项目目录。每个项目使用独立目录：

```text
%LOCALAPPDATA%/VulkanLab/Editor/<projectId>/
  preferences.json
  layout.ini
```

`preferences.json` 当前保存：

- 工作区预设和底部抽屉状态。
- Content Browser 列表/网格模式。
- Render Advanced 状态。
- Viewport Gizmo operation/space 和 overlay 状态。
- Editor Camera 移动速度。

偏好修改采用 1 秒 debounce 和同目录原子替换；退出前强制 flush。损坏的偏好文件会记录 warning 并回退默认值。旧 `Viewport`/`Debugging` 预设分别迁移为 `Scene`/`Debug`。

## Content Browser

独立的 Scenes 和 Assets 窗口已合并为 `Content Browser`。它提供：

- `All`、`Scenes`、`Models / Primitives`、`Environments` 分类。
- 列表和固定尺寸图标网格。
- 场景、模型、基础几何体和环境类型图标。
- 模型 artifact/validation 状态和环境派生状态。

双击 Native Scene 会打开场景；双击 Model 会进入 Model Preview。可实例化的 Model 和 Primitive 可拖入 Native Scene Viewport。模型导入、Validator、Reimport、派生缓存构建和 Environment 操作继续使用原有事务与异步任务系统。

Jobs/Cache 不再占用独立资产页面，统一显示在底部 `Tasks`。场景加载、模型验证/导入、BC7、环境构建和 Capture 的终态只产生一次 notification；错误通知保持显示，可直接展开 Tasks。

## Render 与诊断

Render 面板按用途分为：

- `Output`：Render Path、View Mode、Texture Limit、Exposure 和 Tone Mapper。
- `Lighting`：Shadow、场景灯、Sun、Environment、IBL 和 Atmosphere。
- `Effects`：AO、TAA、SSR、SSGI/DDGI、Bloom 等后处理和间接光效果。
- `Visibility`：Frustum、Distance、Small Object、Shadow 和 Hi-Z Occlusion culling。
- `Camera`：位置、移动速度、clip plane 和 Scene bounds。

`Advanced` 只改变控件可见性，不修改 `RenderSettings`。Surface Data 和算法内部状态放入 Advanced 或底部 Diagnostics；资源尺寸、generation、history reset、GPU pass time 和 RenderGraph 信息不占用日常调参区域。

不支持的渲染功能保留原有位置并显示不可用状态；切换工作区或隐藏 Advanced 不会重置效果参数。

Render Path使用 `Auto / Forward / Deferred`。`Auto`在设备和当前 View Mode兼容时选择 Deferred，
否则显示 Forward fallback原因；显式 Deferred不可用时控件禁用或拒绝设置。View Mode只改变全局
观察方式，Lit模式由每个材质自己的 Shader Family选择 Forward/GBuffer/Shadow Program。

Materials面板显示选中 ModelInstance的 Family、Shading Model、GPU material/texture index、
Forward/GBuffer支持和实际 fallback。编辑器不提供“用一个全局 Shader替换所有材质”的控件。

## Outliner 与 Inspector

Outliner 为每帧 snapshot 建立 parent/children 索引，避免递归绘制时反复扫描全实体数组。搜索结果保留匹配实体的祖先路径。实体行显示类型、enabled 状态、Active Camera、Atmosphere Sun、异步加载、错误和灯光未上传状态。

Inspector 顶部显示实体类型、名称和 UUID，并提供 UUID 复制。`Add Component` 统一创建 Model、Light、Camera、Sky Atmosphere、Reflection Probe 和 DDGI Probe Volume；现有组件继续使用一致的折叠属性区和 SceneEditorSession Undo/Redo。

Outliner drag/drop 继续使用 Keep World reparent，Inspector parent picker 使用 Keep Local。无法分解 shear 的 Keep World 操作原子失败，不修改原层级和 Transform。

## Viewport

Viewport 显示 Renderer 的 per-frame LDR Viewport Color。HDR、Depth、屏幕空间资源和 Bloom 按内容区物理像素创建；拖动 Dock 时临时缩放旧图像，尺寸稳定 120ms 后重建 viewport-dependent 资源，不重建 Swapchain 或清空 Pipeline Cache。

工具栏提供：

- Select/Translate/Rotate/Scale 图标，保留 `Q/W/E/R`。
- Local/World space。
- Editor Camera / Active Camera。
- Bounds、Lights、Probes overlay。

`F` 将 Editor Camera 聚焦到当前选中实体的 world bounds；Active Camera 模式下不执行。右键相机模式只允许从实际图像区域启动，Gizmo、drag/drop、Dock 和活动控件会阻止 picking/camera 冲突。

模型 picking 仍使用 CPU ray 与 ModelAsset bounds，不实现 primitive picking、重叠对象循环选择或 GPU Object-ID Pass。

## Command Palette 与反馈

`Ctrl+P` 打开 Command Palette。File/Edit/View 菜单、快捷键和 Palette 使用同一个 `EditorActionRegistry`，共享 command ID、enabled 状态、图标和 callback。

Info/Success notification 自动消失，Warning 显示更久，Error 保持到手动关闭。删除资源、未保存场景和磁盘冲突仍使用 modal，避免误执行破坏性操作。

## 快捷键

- `Ctrl+N/O/S`：New/Open/Save。
- `Ctrl+Shift+S`：Save As。
- `Ctrl+Z/Y`：Undo/Redo。
- `Ctrl+P`：Command Palette。
- `Ctrl+Space`：最大化/恢复 Viewport。
- `Ctrl+D`：复制选中 subtree。
- `Delete`：删除选中 subtree。
- `F2`：重命名选中实体。
- `F`：Frame Selected。
- `Q/W/E/R`：Select/Translate/Rotate/Scale。

文本输入、modal、Gizmo 操作或 drag/drop 活跃时不会触发实体快捷键。

## Editor-only 依赖

Editor 构建使用 `lucide-static 1.27.0` 的 icon font，并与 Segoe UI 合并到同一 ImGui atlas。字体位于 `external/lucide-static/lucide.ttf`，版本和 ISC License 同目录保存。运行时缺少字体时回退到文字标签和 `Q/W/E/R`，不阻止启动。

`windows-msvc-runtime` 的 `VKL_ENABLE_EDITOR_UI=OFF`，不会编译或链接 ImGui、ImGuizmo、Lucide、Editor preferences 或上述 panels；PresentPass 仍将 Viewport Color 全屏显示到 Swapchain。
