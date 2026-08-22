# 开发资产包

> Status: Current
> Last verified: 2026-08-22
> Verified against: `4e495b9`

VulkanLab 的大型模型源资产不进入 Git。为了让另一台电脑在克隆源码后获得相同的场景开发环境，项目提供本地资产包生成器和安装器。资产包是 Git 之外的交付物，可以通过移动硬盘、局域网或网盘传输；脚本不会上传 GitHub，也不依赖固定下载地址。

## 包含范围

默认生成模式包含：

- 所有 Native SceneDocument 实际引用、但没有被 Git 跟踪的 glTF/GLB 模型。
- 当前机器上存在的其他 Catalog glTF/GLB 模型，用于材质、扩展和回归验证。
- `.gltf` 的本地 `buffers[].uri`、`images[].uri` 依赖闭包。
- 模型目录中的 `credits`、`license`、`copying`、`notice` 和 `readme` 文件。
- Khronos glTF Sample Assets 的模型来源、作者和许可证信息，写入压缩包内部 manifest。
- 每个文件的路径、大小和 SHA-256，用于安装前后的完整性验证。

以下内容不进入资产包：

- Git 已经跟踪的 Sheen Chair、Kenney Furniture、SceneDocument 和基础几何体数据。
- Main Sponza 源目录中的 FBX、USD、MAX、离线渲染图和其他 glTF 未引用文件。
- `build/`、运行日志、截图、RenderDoc/Tracy capture 和编辑器偏好。
- `%LOCALAPPDATA%/VulkanLab/DerivedAssets` 下可重新生成的 Native BC7、环境 KTX2、Validator 报告和 Artifact Index。
- 第三方工具二进制和 Git submodule 内容。

当前场景没有引用外部 HDR Environment；程序化 Atmosphere 参数保存在受 Git 跟踪的 SceneDocument 中，因此不需要额外打包。以后 SceneDocument 引用了本地 HDR 环境时，应先扩展生成器的环境依赖闭包，不能手工把文件塞入既有归档。

## 在源电脑生成

从仓库根目录执行：

```powershell
.\tools\dev\New-DevelopmentAssetBundle.ps1 -Force
```

默认输出：

```text
dist/development-assets/v1/
  VulkanLab-development-assets-v1.json
  VulkanLab-development-assets-v1.tar.zst
```

`.json` 是外部索引，记录归档总大小、SHA-256、源代码 revision 和分卷信息。`.tar.zst` 内部还有逐文件 manifest。`dist/` 被 Git 忽略，生成结果不会进入普通提交。

如果传输介质限制单文件大小，可以显式分卷：

```powershell
.\tools\dev\New-DevelopmentAssetBundle.ps1 `
  -PartSizeMiB 1024 -Force
```

这会生成 `.part001`、`.part002` 等文件。索引和所有 part 必须放在同一个目录中传输。只需要当前 SceneDocument 闭包、不需要额外可用 Catalog 模型时，可增加 `-SceneClosureOnly`。

生成器先在 `build/` 中创建临时 hard-link staging，再写临时输出；完整生成成功后才替换旧包。失败不会删除上一次有效归档。

## 在另一台电脑安装

先克隆代码并初始化 submodule：

```powershell
git clone --recursive https://github.com/JiGuang283/VulkanLab.git
cd VulkanLab
```

将资产索引和归档或全部分卷复制到任意本地目录，然后执行：

```powershell
.\tools\dev\Install-DevelopmentAssets.ps1 `
  -IndexPath D:\VulkanLabAssets\VulkanLab-development-assets-v1.json
```

安装器会：

1. 检查项目 ID 和代码 revision。
2. 检查每个 archive part 的大小与 SHA-256。
3. 拒绝绝对路径、`..` 和其他不安全 tar 条目。
4. 在 `build/` 临时目录中重组并解压归档。
5. 检查内部 manifest 以及每个 payload 文件。
6. 将文件安装到 Catalog 约定的项目相对路径。

代码 revision 不同时只产生 warning，因为仅文档或构建改动通常不影响资产；Catalog ID 不匹配则直接拒绝。目标路径已有不同内容时默认拒绝覆盖，只有确认本地文件可以被资产包版本替换时才使用：

```powershell
.\tools\dev\Install-DevelopmentAssets.ps1 `
  -IndexPath D:\VulkanLabAssets\VulkanLab-development-assets-v1.json `
  -Force
```

安装后按正常流程构建和运行：

```powershell
.\tools\dev\Build-Developer.ps1
.\build\ninja-dev\run\Debug\VulkanLab.exe --project .
```

首次加载尚未生成 Native BC7 cache 的大型模型时，仍可能执行源纹理解码或派生资产构建。需要稳定加载性能时，在目标电脑通过 AssetTool 为对应 Catalog profile 生成本机 DerivedAssets；不要把 `%LOCALAPPDATA%` 缓存复制回仓库。

## 更新规则

- Catalog model 路径、SceneDocument 模型引用或 glTF URI 依赖变化后，重新生成资产包。
- 添加新的第三方 Catalog 模型时，在 `assets/development_assets.json` 中补充官方来源和许可归属。
- 升级不兼容的包结构时使用新的 `-Version`，不要覆盖仍供旧分支使用的版本。
- 传输后保留 `.json` 索引；它是选择正确归档并验证完整性的入口。
- 不把大型归档、分卷或 DerivedAssets 提交到 Git，也不把它们作为源码仓库历史的一部分。
