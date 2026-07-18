# 资源加载

> Status: Current
> Last verified: 2026-07-18
> Verified against: `0516951`

## SceneFactory 事务

SceneRegistry 保存显示名称和 SceneFactory。工厂接收 Device、同一次加载共享的 UploadContext、DescriptorAllocator 与 SceneLoadContext，并返回完整 Scene。Scene 拥有 Texture、MaterialTemplate、MaterialInstance、Mesh、SceneObject 和可选 SceneLight 的共享引用。

替换场景时，Application 当前执行：

1. `vkDeviceWaitIdle()`。
2. 清空 PipelineCache 并销毁旧 Scene。
3. 记录加载前 VMA 快照。
4. 创建 UploadContext 并调用 SceneFactory。
5. `UploadContext::finish()` 提交最后一个批次并等待 fence。
6. 销毁临时 staging 后记录加载后 VMA 快照。
7. 发布新 Scene，应用初始相机和基于 bounds 的 near/far plane。

工厂或上传抛出异常时不发布新场景，已完成的诊断信息会保留，异常继续向上层传播。

## glTF 加载

GltfLoader 支持 `.gltf` 与 `.glb`，并完成以下主要转换：

- 解析 node hierarchy 和 world transform，按 primitive 创建 Mesh 与 SceneObject。
- 读取 position、normal、UV0、UV1、tangent、COLOR_0 和 index；缺失 tangent 时按 indexed triangle 生成，退化 UV 使用稳定 fallback。
- 读取 metallic-roughness 材质、alpha mode/cutoff、double-sided、normal scale、AO strength/UV、emissive strength 和 transmission factor。
- 保留 KHR_materials_volume 的基础参数，但当前 shader 不使用这些 volume 数据。
- `transmissionTexture` 当前只记录 warning，不创建额外纹理槽。

顶点色当前支持 float VEC3/VEC4。AO 支持 `texCoord` 0/1，其他纹理仍使用 UV0；AO 指定更高 UV set 时回退 UV0 并记录 warning。当前 loader 不导入 glTF punctual lights、动画、skin 或 morph target。

## 图片与 Texture

图片可以来自 GLB bufferView、解析器提供的 embedded bytes，或 `.gltf` 所在目录下的本地 URI。远程 URI 不下载，失败时使用 fallback texture。图片通过 stb 解码为 RGBA8。

格式按语义选择：BaseColor 和 Emissive 使用 sRGB；Normal、MetallicRoughness 与 Occlusion 使用 linear UNORM。同一个 glTF texture index 以目标 VkFormat 区分缓存，避免同一图片在不同颜色语义下复用错误格式。Sampler 会映射 repeat、clamp、mirrored repeat、min/mag filter 和 mipmap mode。

`SceneLoadContext::maxTextureSize` 默认是 2048，`0` 表示不限制。Texture 在上传前按最长边等比执行 CPU bilinear RGBA8 缩放，再以新尺寸创建 image 和 mip chain。该限制只作用于 glTF 加载链路，Viking Room 的直接 Texture 加载不受影响。

## UploadContext

每次场景加载共用一个 UploadContext。它使用 graphics queue、独立 transient/resettable command pool、单个 command buffer、可复用 fence 和持久映射 staging buffer。

- 默认批次上限为 128 MiB，staging 首次按需从 16 MiB 起增长。
- Buffer copy、buffer-to-image copy、mipmap blit 和 image barrier记录在批次 command buffer 中。
- 当前数据放不下时提交批次并等待 fence，然后复用 staging 内存。
- 单个资源超过默认上限时 staging 临时增长，并以独占批次处理。
- 正常路径必须调用 `finish()`；场景加载资源路径不再为每个资源调用 `vkQueueWaitIdle()`。

批量上传降低了 submit/wait 数量，但 fence wait 和全部 CPU 工作仍同步阻塞主线程。它不是后台 streaming 系统。

## LoadStats 与内存

Application 保存最后一次 SceneLoadStats，并在日志和 `Stats -> Last Scene Load` 显示：

- device idle、teardown、glTF parse、图片读取/解码/缩放、材质、mesh CPU、上传和 hierarchy 耗时。
- 纹理、mesh、vertex、index、material、object 数量及 encoded/decoded/upload 字节。
- legacy submit/queue wait、batch submit/fence wait 和 peak staging。
- 场景加载前后的 VMA allocation count、allocation bytes 与 block bytes。

VMA 数值只描述 allocator 管理的资源与内存块，不等同于 Windows 任务管理器显示的专用显存。

## 当前限制

- 加载、解码、缩放、上传和场景发布都在渲染主线程同步完成。
- 没有进度事件、取消、后台线程、每帧上传预算或增量场景发布。
- 没有 KTX2/BasisU、GPU block compression、磁盘缓存、LRU residency、virtual texturing 或专用 transfer queue。
- CMake 仍复制整个 `models/` 目录，没有 runtime asset manifest。

因此纹理限制和批量上传解决了显存峰值与过多 queue idle 的一部分问题，但没有解决大型场景加载期间窗口无响应的问题。
