# 编辑器 UI 工作区

> Status: Current
> Last verified: 2026-07-30
> Verified against: KHR_lights_punctual working tree

VulkanLab 的开发 UI 使用一个可移动、缩放和折叠的 `VulkanLab` 工具窗口。
它不依赖 ImGui docking branch，也不创建独立的平台窗口。

## 首次布局

首次运行时，工具窗口位于主 viewport 右侧：

- 宽度为工作区宽度的 32%，并限制在 `320..420px`。
- 顶部和底部各保留 `8px`。
- 窗口没有关闭按钮，避免关闭后缺少恢复入口。
- 后续位置、尺寸和折叠状态由现有 `imgui.ini` 保存。

使用全新的工作目录可以清除已有布局影响。旧 `imgui.ini` 中的
`Renderer`、`Scenes`、`Assets`、`Loading`、`Lighting`、`Materials`、
`Camera`、`Stats` 和 `Capture` 记录不会再被读取，因为这些窗口不再创建。

顶部状态区始终显示当前 Scene 和 FPS。Scene 名称受单元格裁剪，悬停可查看
完整名称。仅当场景加载任务活跃时，状态区下方才显示紧凑进度条。

## Scene

`Scene` 页包含两个子页：

- `Scenes`：搜索、选择和加载 Catalog scene；导入、重导入、source fallback、
  保存相机和移除操作也在这里。活跃场景任务的详细进度和 Cancel 位于页面
  底部；没有任务时不显示空 Loading 区域。
- `Assets`：项目、Catalog、cache、artifact 状态、当前资产导入进度、取消、
  日志和最近任务历史。该页还管理 HDR environment：`Import HDR` 将本地
  2:1 `.hdr` 复制进项目并注册到 Catalog；环境列表显示 profile 与
  `Ready/Missing/Stale/Invalid`，并提供 Build、Rebuild、Cancel 和 Remove。
  环境 bake 完成前不会替换当前已发布环境。

Import 和 Remove 仍使用 modal；文件导入和场景加载的数据流没有因布局调整
而改变。

## Render

`Render` 页使用四个默认展开的分区：

- `Pipeline`：Shader variant、Texture Limit、Exposure EV 和 PBR Tone Mapper。
  Shader 下拉按 Manifest 的 `legacy`、`pbr`、`debug` category 分组。
- `Post Processing`：Compute Bloom 开关、Threshold、Soft Knee 和 Intensity，
  并显示设备 Available 与当前 Shader 下的 Active 状态。Bloom 默认关闭，只在
  `PBR-lite Forward` 和 `PBR-lite NormalMapped` 下生效；切换到 Legacy 或
  Debug variant 不会丢失设置，但 Active 会变为 No。
- `Lighting`：方向光阴影、bias、ambient、灯光统计和无场景灯光时的 fallback
  Sun 参数；包含 `KHR_lights_punctual` 的场景还可展开只读灯光列表，检查类型、
  颜色、物理强度单位、位置、方向、范围和 Spot 锥角。Environment combo、
  Image-Based Lighting、Skybox、Intensity 和 Rotation 也在这里。`None`
  始终可选，选择环境与启用 IBL/Skybox 是三个独立操作，默认两个开关均关闭。
- `Camera`：位置、移动速度、near/far clip plane 和 Scene bounds。

这些控件直接操作 Application 已有状态，与 Runtime Control 使用同一份渲染
设置；UI 没有复制一套 ViewModel。

## Materials

`Materials` 是只读诊断页：

1. 使用名称或 Scene material index 筛选材质。
2. 在上方列表选择一个材质。
3. 在下方查看 `Surface`、`PBR`、`Textures` 和
   `Derived Render State` 两列表格。

Scene generation 变化时，选择会重置到首个有效材质，避免保留上一个 Scene
的失效索引。当前版本不提供材质编辑、贴图缩略图或对象高亮。

## Diagnostics

`Diagnostics` 页包含：

- `Performance`：FPS、对象数量、输入模式和 GPU Pass Timings。
- `Load Stats`：最近一次场景加载的阶段耗时、资源、上传、cache 和 VMA 数据。
- `Capture`：带/不带 GUI 截图、取消、任务历史、输出路径和错误。

`--no-gui` 会继续跳过整个 ImGui 初始化和绘制流程；统一工作区不会改变该
启动参数、截图语义或 Runtime Control 行为。
