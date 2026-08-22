# Test Resources And Outputs

测试源码、规范和确定性基线属于仓库内容；测试进程产生的项目副本、日志、截图和报告不得写回源码树。

## Repository Inputs

- `tests/render/` 保存自动渲染规范。
- 小型共享 fixture 可以位于项目 `assets/` 或 `models/` 中，但必须由 Catalog 稳定引用，并同时可供编辑器手动诊断。
- `assets/scenes/renderer-smoke.vkscene.json` 是当前渲染与 IBL smoke 共用的场景 fixture，因此保留在项目资产中。
- Algorithm Playground、GI Calibration 和 Sponza GI Stress 是开发基准场景，不属于临时测试输出。

## Generated Outputs

CMake 测试输出统一位于当前 preset 的 build tree：

```text
build/<preset>/
  test-bin/<Config>/  # 单元测试 executable
  test-work/       # 临时项目、缓存和测试工作目录
  test-results/    # 截图、报告和比较结果
```

`VulkanLabRenderTest` 会为每次运行创建独立 Workspace，并以 `--asset-mode readonly` 启动渲染器。测试不得修改源 Catalog、SceneDocument、模型或环境资产。

大型下载资源和派生缓存不提交到 `tests/`。需要它们的测试应在 `test-work/` 中生成最小 fixture，或通过已有 AssetTool 流程构建临时项目。
