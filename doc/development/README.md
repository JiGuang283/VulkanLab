# 开发文档工作区

> Status: Current
> Last verified: 2026-08-01
> Verified against: `86b809d`

本目录只存放正在讨论或执行的开发计划。不要根据历史归档推断下一阶段目标。

## Active Plans

- [大型场景响应式加载路线图](async_scene_loading_plan.md)：从现有同步加载演进到后台准备、增量 GPU 上传、任务取消和压缩纹理资产管线。
- [工程结构与构建系统重构计划](engineering_refactor_plan.md)：用 target-based CMake、build-tree Shader、ProjectContext 资源路径和 Application 职责拆分支撑后续工具开发。
- [开发诊断与自动化工具链计划](development_toolchain_plan.md)：Stage 0-5 已完成，下一未完成阶段是 Windows CI 与质量门禁。
- [可编辑场景与多模型世界实施计划](scene_authoring_plan.md)：将导入模型、可编辑场景和运行时 World 分层，逐步实现多模型实例、实体编辑、独立灯光与 Cook 集成。

## Completed Records

- [工程基础到自动视觉回归执行记录](../archive/plans/engineering/engineering_to_visual_regression_execution_plan.md)：已完成 M0-M7，包含提交、自动测试和人工门禁证据。

## 新计划规则

1. 每个计划使用独立 Markdown 文件，文件名使用小写 snake_case。
2. 开头标记 `Status: Active`、创建或最近核对日期，以及计划所依据的代码提交。
3. 明确 Summary、范围、接口变化、实现步骤、测试计划、假设和非目标。
4. 计划描述目标状态，不得写成已经实现的当前能力；当前行为应链接到 `doc/guides/` 或 `doc/architecture/`。
5. 计划完成、废弃或被替代后，使用 `git mv` 移入 `doc/archive/plans/` 的合适分类，并在必要时记录最终状态。

只有仍需团队决策或仍有未完成工作时，计划才应留在本目录。问题调查完成但不形成实施计划时，归档到 `doc/archive/analyses/`；已完成的历史变化记录归档到 `doc/archive/change_logs/`。
