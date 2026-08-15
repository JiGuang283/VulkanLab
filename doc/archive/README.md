# 历史文档归档

> Status: Archived
> Last organized: 2026-08-15

本目录保存 VulkanLab 过去的开发计划、架构分析和变更记录，用于追溯设计背景与实现过程。

归档文档可能引用已经删除的 API、旧目录、旧构建命令或已废弃的设计结论，不能作为当前实现依据。当前使用方法和系统行为应以 `doc/guides/` 与 `doc/architecture/` 为准。

## 目录

- `plans/`：已完成、废弃或被后续方案取代的开发计划；`plans/engineering/` 保存工程化执行记录。
- `analyses/`：特定阶段的问题分析、架构评审和调查记录。
- `change_logs/`：历史架构或功能变更记录。

近期完成的路线图：

- [场景与资源资产管线完整计划](plans/optimization/asset_management_pipeline_plan.md)：Scene Catalog、共享缓存、并行/自动导入、Artifact Index、Cooked runtime，以及 Stage F 条件决策。
- [工程基础到自动视觉回归执行记录](plans/engineering/engineering_to_visual_regression_execution_plan.md)：M0-M7 的工程重构、异步截图、Runtime Control v3 和 RenderTest/golden 闭环。
- [工程结构与构建系统重构计划](plans/engineering/engineering_refactor_plan.md)：target-based CMake、build-tree Shader、ProjectContext 和早期 Application 拆分设计。
- [渲染器收口与 Application 重构计划](plans/engineering/renderer_consolidation_and_app_refactor_plan.md)：Viking/OBJ 退役、Graph 资源驻留、Application/Editor/Runtime Control 边界与 Renderer 功能目录收口。
- [渲染器收口 Stage 0 基线](plans/engineering/renderer_consolidation_baseline.md)：上述收口计划实施前的资源与源码基线。
- [可编辑场景与多模型世界实施计划](plans/scene_authoring/scene_authoring_plan.md)：Stage 0-7 的共享模型资产、RuntimeWorld、编辑器交互、可扩展灯光与 Native Scene 发布闭环。

除修复因归档移动而失效的相对链接外，归档文件保持原始内容。新计划应先放入 `doc/development/`，完成或废弃后再移入本目录。
