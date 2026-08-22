# 开发文档工作区

> Status: Current
> Last verified: 2026-08-21
> Verified against: `62f6cc4`

本目录只存放正在讨论或执行的开发计划。不要根据历史归档推断下一阶段目标。

## Active Plans

- [Vulkan 底层性能优化路线与执行计划](vulkan_runtime_performance_optimization_plan.md)：收口 RenderGraph 与 descriptor CPU 开销，消除 Deferred 重复几何工作，缓存 Shadow，降低 GBuffer 带宽，并分阶段演进到 GPU-driven draw submission。

- [AO、反射与全局光照算法路线](ao_reflection_gi_plan.md)：基于 Surface Data、Hi-Z、HDR 和 temporal history，分阶段实现 SSAO、TAA、GTAO、SSR、SSGI、Reflection Probe、DDGI 与可选硬件光追路径。
- [大型场景响应式加载路线图](async_scene_loading_plan.md)：从现有同步加载演进到后台准备、增量 GPU 上传、任务取消和压缩纹理资产管线。
- [开发诊断与自动化工具链计划](development_toolchain_plan.md)：Stage 0-5 已完成，下一未完成阶段是 Windows CI 与质量门禁。

## Completed Records

- [Forward / Deferred Render Path 与 Material Shader Family](../archive/plans/render_architecture/deferred_render_paths_and_material_shaders_plan.md)：已完成共享 opaque path contract、材质 Shader Family、Deferred GBuffer、Clustered Lighting、Auto 路径策略及 Cooked Runtime 收口。
- [工程基础到自动视觉回归执行记录](../archive/plans/engineering/engineering_to_visual_regression_execution_plan.md)：已完成 M0-M7，包含提交、自动测试和人工门禁证据。
- [工程结构与构建系统重构计划](../archive/plans/engineering/engineering_refactor_plan.md)：target、Shader、ProjectContext 与早期 Application 边界设计的历史计划。
- [渲染器收口与 Application 重构计划](../archive/plans/engineering/renderer_consolidation_and_app_refactor_plan.md)：已完成 Viking/OBJ 退役、Graph 资源驻留、Controller 拆分、Renderer 功能目录和文档收口。
- [渲染器收口 Stage 0 基线](../archive/plans/engineering/renderer_consolidation_baseline.md)：收口前的 800x600 资源、代码规模和兼容迁移债务采样。
- [可编辑场景与多模型世界实施计划](../archive/plans/scene_authoring/scene_authoring_plan.md)：已完成 Stage 0-7，包含共享 ModelAsset、RuntimeWorld、场景编辑、可扩展灯光与 Native Scene Cook。

## 新计划规则

1. 每个计划使用独立 Markdown 文件，文件名使用小写 snake_case。
2. 开头标记 `Status: Active`、创建或最近核对日期，以及计划所依据的代码提交。
3. 明确 Summary、范围、接口变化、实现步骤、测试计划、假设和非目标。
4. 计划描述目标状态，不得写成已经实现的当前能力；当前行为应链接到 `doc/guides/` 或 `doc/architecture/`。
5. 计划完成、废弃或被替代后，使用 `git mv` 移入 `doc/archive/plans/` 的合适分类，并在必要时记录最终状态。

只有仍需团队决策或仍有未完成工作时，计划才应留在本目录。问题调查完成但不形成实施计划时，归档到 `doc/archive/analyses/`；已完成的历史变化记录归档到 `doc/archive/change_logs/`。
