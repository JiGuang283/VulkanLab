# 自动视觉回归

> Status: Current
> Last verified: 2026-08-15
> Verified against: Renderer Smoke Scene migration

`VulkanLabRenderTest.exe` 是独立的开发测试程序。它通过 Runtime Control 启动并控制 `VulkanLab.exe`，固定场景、Shader、相机、窗口尺寸和时间步，等待画面稳定后截图，再执行 smoke 或 golden 比较。测试程序不链接渲染器或 Vulkan；实际 GPU 能力仍由被测 `VulkanLab.exe` 提供。

## 构建与运行

```powershell
cmake --preset windows-msvc-full
cmake --build --preset windows-msvc-full-debug

.\build\full\run\Debug\VulkanLabRenderTest.exe run `
  --project . `
  --runtime .\build\full\run\Debug\VulkanLab.exe `
  --spec .\tests\render\renderer_smoke_legacy.json `
  --output .\out\test-results\render
```

Runner 会为每次运行创建唯一 Named Pipe、capture root 和结果目录。工作目录由 Runner 显式设置，不依赖调用命令时的当前目录；进程树由 Windows Job Object 管理，正常结束发送 `app.quit`，异常或超时则终止整个 Job。

Runner 会从 `scene.list` 核对 spec 的 scene。Model Preview spec 必须提供 `profileId`，Runner 会在加载前应用对应纹理限制；Native Scene spec 不提供 `profileId`，其模型分别使用 Catalog 中的 profile。

## CTest 测试集

```powershell
# 纯 CPU、资产和 package 测试
ctest --preset windows-msvc-test -L unit --output-on-failure

# 需要 Vulkan GPU 和可呈现窗口
ctest --preset windows-msvc-test -L visual --output-on-failure

# Debug 全部测试
ctest --preset windows-msvc-test --output-on-failure

# Release 全部测试
ctest --test-dir build/full -C Release --output-on-failure
```

当前快速视觉集包含：

- Renderer Smoke Scene + Legacy Forward smoke；
- Sheen Chair + PBR-lite NormalMapped smoke；
- Sheen Chair + Debug BaseColor smoke；
- Renderer Smoke Scene + PBR-lite NormalMapped shadow smoke；
- Renderer Smoke Scene + Debug Shadow smoke；
- 运行时生成 tiny HDR/KTX2 后执行 Skybox rotation、Debug IBL Diffuse 和 Debug IBL Specular smoke；
- 当前不提交 Renderer Smoke Scene golden；人工确认候选后再建立新 baseline。

Main Sponza 只作为本地扩展 smoke 和 LoadStats 场景，不进入快速默认测试集。

## Spec v1-v4

规格文件位于 `tests/render/`，使用稳定 scene/profile/environment ID，而不是 UI index。解析器继续接受 schema v1；schema v2 增加 `renderSettings`，schema v3 增加环境，schema v4 增加 Bloom。Model Preview 需要 `profileId`，Native Scene 省略该字段。解析器拒绝未知字段和非法范围。

```json
{
  "schemaVersion": 2,
  "name": "renderer-smoke-pbr-shadow",
  "sceneId": "renderer-smoke",
  "shader": "PBR-lite NormalMapped",
  "camera": {
    "position": [0.0, -7.0, 3.1],
    "yaw": 90.0,
    "pitch": -24.0
  },
  "viewport": [800, 600],
  "fixedDelta": 0.000001,
  "stableFrames": 8,
  "includeGui": false,
  "renderSettings": {
    "shadowsEnabled": true,
    "shadowReceiverBias": 0.0015,
    "shadowConstantBias": 1.25,
    "shadowSlopeBias": 1.75,
    "exposureEv": 0.0,
    "toneMapper": "aces"
  },
  "mode": "smoke",
  "thresholds": {
    "minimumNonBlackRatio": 0.05,
    "maximumSolidColorRatio": 0.98
  }
}
```

IBL 测试使用 schema v3，例如：

```json
{
  "schemaVersion": 3,
  "name": "renderer-smoke-ibl",
  "sceneId": "renderer-smoke",
  "environmentId": "studio",
  "shader": "PBR-lite NormalMapped",
  "camera": {
    "position": [0.0, -7.0, 3.1],
    "yaw": 90.0,
    "pitch": -24.0
  },
  "viewport": [800, 600],
  "fixedDelta": 0.000001,
  "stableFrames": 8,
  "includeGui": false,
  "renderSettings": {
    "iblEnabled": true,
    "skyboxEnabled": true,
    "environmentIntensity": 1.0,
    "environmentRotationRadians": 0.0
  },
  "mode": "smoke",
  "thresholds": {
    "minimumNonBlackRatio": 0.05,
    "maximumSolidColorRatio": 0.98
  }
}
```

Runner 会先查询 `environment.list`，提交异步 environment load 并等待发布，再应用渲染设置。仓库的 IBL CTest 不提交大型 HDR/KTX2：测试脚本在临时项目中生成非均匀 tiny HDR、离线 bake、运行四个 schema v3 smoke，并确认旋转 0°/90° 的 PNG SHA-256 不同。

Smoke 检查尺寸、非黑像素比例和主导纯色比例，用于发现黑屏、空帧或完全错误的输出。Golden 在此基础上比较 RGBA8 的逐通道绝对差、MAE、RMSE 和坏像素比例。

## 结果与错误

每次运行输出到独立目录：

```text
<output>/<spec>-<utc>-<pid>-<run-id>/
  report.json
  actual.png
  diff.png                 # golden 比较失败时
  renderer.log
  renderer-stdio.log
  capture/...
```

`report.json` 记录 spec、BuildInfo、GPU、Shader SPIR-V hash、Runtime Control 步骤、加载统计、截图耗时、比较指标、清理结果和首个根因。常见错误码包括 `renderer_not_found`、`renderer_start_failed`、`load_failed`、`capture_timeout`、`smoke_compare_failed`、`golden_compare_failed`、`renderer_crash` 和 `quit_timeout`。

| 退出码 | 含义 |
|---:|---|
| `0` | 测试通过或 baseline 已显式接受。 |
| `1` | 渲染、协议、超时或图像比较失败。 |
| `2` | Runner 参数、spec 或本地配置错误。 |
| `125` | Smoke 通过，但 reference GPU family 不匹配，golden 跳过。 |

失败后优先查看 `report.json` 的 `error`、`steps` 和 `cleanup`，再检查 `renderer.log` 与 `actual.png`。Golden 比较失败会保留 `diff.png`，默认运行绝不会覆盖 baseline。

## Golden 更新

基线包含 PNG 和同名 metadata JSON。Metadata 固定 scene/profile、Shader、viewport、fixed delta、BuildInfo、GPU vendor/device、Shader hash、尺寸和 PNG SHA-256。Golden 只在当前 GPU 的 vendor/device 与 reference 相同时作为阻塞测试；其他 GPU 返回 `125`，但仍必须先通过 smoke。

仓库目前没有已接受的 Renderer Smoke Scene golden。建立新基线时必须人工审核：

1. 先正常运行 golden spec，查看失败目录中的 `actual.png` 和 `diff.png`。
2. 确认画面变化符合预期后，显式执行一次 `--accept`。
3. 检查新 baseline 和 metadata，再提交这两个文件。
4. 重新运行 Debug/Release golden，至少连续通过两次。

```powershell
.\build\full\run\Debug\VulkanLabRenderTest.exe run `
  --project . `
  --runtime .\build\full\run\Debug\VulkanLab.exe `
  --spec .\tests\render\renderer_smoke_legacy_golden.json `
  --output .\out\test-results\golden-review `
  --accept
```

`--accept` 只允许 golden spec。Smoke spec 使用该参数会以 `accept_requires_golden` 失败，并且不会启动渲染器。

## 限制

- v1 需要 Windows、Vulkan GPU 和可呈现窗口，不是无头渲染。
- Runner 保持窗口可见；最小化、远程桌面断开或驱动停止 present 会导致稳定帧等待失败。
- 同 GPU family 的驱动变化和 Shader hash 变化会写入报告，但图像阈值仍是最终判定。
- `VulkanLabRenderTest.exe`、spec、golden、capture 和报告均为开发资产，不进入 Cook package。
- 并行运行必须使用 Runner；它会自动分配 endpoint。手工脚本需要自行指定唯一 `--runtime-control-pipe`。

底层命令与截图协议见 [Runtime Control](runtime_control.md)，确定性启动参数和截图约束见[诊断与自动化启动配置](diagnostics.md)。
