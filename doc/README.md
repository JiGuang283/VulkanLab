# VulkanLab 文档

> Status: Current
> Last verified: 2026-08-01
> Verified against: `86b809d`

本目录按文档有效性分为 Current、Active 和 Archived 三种状态。只有 `guides/` 与 `architecture/` 中标记为 Current 的文档可以作为当前实现依据；源码始终是最终事实来源。

## 当前文档

- [构建与运行](guides/build_and_run.md)：环境、构建、开发运行、Cook/package 和启动参数。
- [Runtime Control](guides/runtime_control.md)：通过 `VulkanLabCtl.exe` 在运行时控制渲染器。
- [诊断与自动化启动配置](guides/diagnostics.md)：CMake Presets、BuildInfo 和确定性运行参数。
- [RenderDoc 与 Vulkan Validation](guides/renderdoc_validation.md)：抓帧标签、对象命名、Validation Profiles 和 smoke workflow。
- [Tracy 性能分析](guides/tracy_profiling.md)：专用构建、Profiler 安装、CPU/GPU 时间线、命令行 capture 和状态诊断。
- [自动视觉回归](guides/visual_regression.md)：RenderTest、smoke/golden、结果报告和基线审核流程。
- [Shader Registry](guides/shader_registry.md)：Manifest 驱动的 Shader 注册、构建、运行时选择与扩展流程。
- [编辑器 UI 工作区](guides/editor_ui.md)：统一工具窗口、页面布局和各诊断入口。
- [系统概览](architecture/overview.md)：模块边界、初始化顺序、线程所有权和每帧流程。
- [渲染流程](architecture/rendering.md)：RenderQueue、Forward Pass、Pipeline、材质、Shader 和光源。
- [资源加载](architecture/resource_loading.md)：SceneFactory、glTF、KTX2、Artifact Index、Cook/package、批量上传和加载统计。

## 文档状态

- **Current**：已按标注的代码提交核对，可以用于理解和操作当前版本。
- **Active**：正在讨论或执行的开发计划，只描述目标，不代表功能已经存在。
- **Archived**：历史计划、分析或变更记录，可能包含旧 API、旧路径和过时结论。

新开发计划放在 [development/](development/README.md)。计划完成、废弃或被替代后，应移动到 [archive/](archive/README.md)，不要继续留在当前文档入口中。
