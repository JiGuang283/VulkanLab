# 资源加载

> Status: Current
> Last verified: 2026-07-19
> Verified against: `c3aa7eb`

## 加载入口

SceneRegistry 的 `SceneEntry` 可以提供两种入口：

- `SceneFactory`：同步创建完整 GPU Scene，当前用于 Viking Room 和初始化兼容路径。
- `ScenePrepareFactory`：只产生 `PreparedSceneData`，当前用于全部 glTF 场景。

`PreparedSceneData` 保存纹理 payload、sampler/format 描述、材质参数、vertex/index、对象引用和相机建议，不包含 Vulkan handle、VMA allocation 或 descriptor set。纹理 payload 可以是运行时解码的 RGBA8 base level，也可以是 KTX2 转码后带完整 mip chain 的数据。主线程随后由 `SceneGpuBuilder` 把它转换为运行时 Scene。

## 异步 glTF Prepare

`SceneLoadManager` 持有一个 C++17 worker thread。新的 glTF 请求获得递增的 taskId 和 generation，并取消旧的 pending/active 请求。worker 调用 `GltfPreparer` 完成：

- 解析 `.gltf`/`.glb`、node hierarchy 和 world transform。
- 读取 position、normal、UV0、UV1、tangent、COLOR_0 和 index；缺少 tangent 时生成稳定 fallback。
- 转换 metallic-roughness、alpha、double-sided、normal scale、AO、emissive strength、transmission 和 volume 基础参数。
- 读取 embedded image 或 `.gltf` 相对路径下的本地图片，使用 stb 解码为 RGBA8。
- 按 `SceneLoadContext::maxTextureSize` 在 CPU 侧执行等比 bilinear 缩放。
- 生成 primitive、bounds、材质与对象的稳定索引关系。

worker 在文件、图片、材质、primitive 和 hierarchy 等自然边界检查 cancellation token。异常写入任务的 Failed 状态，不跨线程调用 Vulkan，也不向 Application 抛异常。

图片格式仍按语义选择：BaseColor/Emissive 使用 sRGB，Normal/MetallicRoughness/Occlusion 使用 linear。同一 glTF image 在不同语义下可以对应不同派生纹理，不能只按 image index 复用；sampler 映射 repeat、clamp、mirrored repeat、min/mag filter 和 mipmap mode。

## KTX2 派生资产（阶段三开发中）

阶段三在原始 glTF 与运行时 loader 之间增加只读派生缓存。离线 `VulkanLabAssetTool texture-cache build` 扫描材质实际引用的图片，用 KTX-Software 生成 UASTC + Zstd KTX2 和完整 mip chain。源 glTF、GLB 和图片保持不变。

默认缓存根目录为 `derived_assets`：

```text
derived_assets/
  manifests/<normalized-scene-key>/<full|512|1024|2048>.json
  blobs/<content-cache-key>.ktx2
```

manifest schema v1 记录 scene、texture limit、源文件 size/write time/SHA-256，以及每个 image 的语义、mipmap wrap、输出尺寸、cache key 和 blob。`TextureSemantic` 固定为 `SrgbColor`、`LinearData` 和 `Normal`；BaseColor/Emissive 使用 `SrgbColor`，MetallicRoughness/Occlusion 使用 `LinearData`，normal map 使用 `Normal`。同一图片以不同语义或 wrap 参与材质时必须生成独立条目。

运行时按 scene 和 texture limit 查找精确 profile，并用 size + write time 快速检查依赖。命中后由 worker 使用 libktx 读取 KTX2：支持 BC7 的设备转码到 BC7 SRGB/UNORM，不支持 BC7 时转为 RGBA32。两条路径都保留离线 mip chain，不执行 stb decode、bilinear resize 或 GPU blit mip generation。

`PreparedImage` 的 prebuilt mip payload 包含总字节数组、最终 `VkFormat` 和每级 mip 的 offset、size、width、height。`SceneGpuBuilder` 将所有 mip 写入 staging，并通过多个 `VkBufferImageCopy` region 上传到只需要 `TRANSFER_DST | SAMPLED` 的 image。原始 RGBA8 fallback 继续上传 base level，并由 GPU blit 生成 mip。

以下情况必须记录 cache miss/invalid 原因并回退 stb，而不是使场景加载失败：manifest 不存在、profile 不匹配、源文件戳变化、schema 不支持、条目缺失、blob 缺失或损坏、KTX2 读取/转码失败。缓存生成只由显式资产工具执行，运行时不会自动编码或修改缓存。

## 增量 GPU Build

CPU prepare 完成后，Application 在开始 GPU build 前执行一次明确的 teardown 边界：

1. `vkDeviceWaitIdle()`，确保旧 Scene 不再被帧命令引用。
2. 清空 PipelineCache，释放旧 Scene，避免两个 Main Sponza 级场景重叠占用显存。
3. 记录 VMA before 快照，创建 SceneGpuBuilder。

SceneGpuBuilder 每帧以默认 `32 MiB` 和 `2 ms` 软预算推进：fallback、Texture、Mesh、等待 GPU、Material、SceneObject 和 publish。单个不可拆分 Texture/Mesh 可以超过软预算，但不会因大于预算而饥饿。

`IncrementalUploadQueue` 使用 graphics queue 和两个延迟创建的 slot。每个 slot 拥有独立的 128 MiB staging、command pool/buffer 和 fence：

- copy、mipmap blit、barrier 记录到当前批次。
- 当前 slot 放不下时提交，并继续使用另一个空闲 slot。
- 普通帧只用 `vkGetFenceStatus()` 轮询，不调用无限期 `vkWaitForFences()` 或 `vkQueueWaitIdle()`。
- fence 完成前不覆盖 staging，也不销毁关联的半成品 Texture/Mesh。
- 全部上传完成后才创建材质 descriptor 并把 Scene 发布到 RenderQueue。

取消或 GPU build 失败会停止记录新资源，提交已经记录的命令，并逐帧轮询在途 fence。相关 fence 完成后才销毁半成品；正常取消路径不使用 device idle。程序退出和显式 teardown 可以 drain。

DescriptorAllocator 创建支持单独释放 set 的 pool。MaterialInstance 析构会归还 descriptor set，因此失败、取消和反复重载不会持续耗尽 pool 容量。

## 发布与失败语义

- CPU prepare 期间继续渲染当前 Scene。
- 开始 GPU build 后旧 Scene 已释放，期间渲染空场景和 ImGui Loading 面板。
- 只有最新 generation 且所有 upload fence 完成的任务可以发布。
- CPU prepare 失败时保留当前 Scene；旧 Scene 已释放后的 GPU build 失败会保留可操作的空场景。
- 同步 Viking Room 切换会先取消后台任务，旧 generation 不能在稍后覆盖同步场景。
- Application 关闭前停止控制服务、取消 builder、join worker，再销毁 Vulkan 对象。

## LoadStats

日志、`Stats -> Last Scene Load` 和 Runtime Control JSON 会记录：

- taskId、generation、最终状态、worker queue、CPU prepare、GPU build 和总 wall time。
- glTF parse、图片读取/解码/缩放、材质、mesh CPU、hierarchy 和 command recording 耗时。
- prepared CPU bytes、纹理/mesh/vertex/index/material/object 数量及上传字节。
- 每帧 upload pump 的最大耗时/字节、batch submit/completion、fence poll/wait 和 peak in-flight。
- KTX2 cache lookup/hit/miss/invalid、读取字节与耗时、transcode 耗时、BC7/RGBA32 fallback 数量和 prebuilt mip 数量。
- 场景资源创建前后的 VMA allocation count、allocation bytes 与 block bytes。

VMA 快照在 SceneGpuBuilder 和 staging 销毁后采集，不把临时 staging 计入场景常驻差值。VMA 数值不等同于 Windows 任务管理器显示的专用显存。

## 当前限制

- CPU prepare 只有一个 worker；不同场景不会并行解码。
- 单个大 Texture/Mesh 仍是原子 pump，可能造成短帧尖峰。
- 使用 graphics queue，没有专用 transfer queue 或 queue ownership transfer。
- 未生成或未命中 KTX2 profile 的图片仍需运行时 decode/resize，并以 RGBA8 上传。
- KTX2 v1 统一使用 BC7，不使用 BC5 normal 或 BC4 AO；无 BC7 设备使用 RGBA32，因此不会获得压缩显存收益。
- 没有运行时自动编码、mip streaming、LRU residency、virtual texturing 或加载时保留旧大场景的显存预算策略。
- Viking Room 仍使用同步工厂；CMake 仍复制整个 `models/` 目录。
