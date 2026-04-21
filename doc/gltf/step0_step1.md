# Step 0 + Step 1 具体执行计划

> 上游文档：[design.md](./design.md) · [implementation_steps.md](./implementation_steps.md)
> 本文件：把前两步落地到**文件级、函数级、命令级**的可执行清单。
> 预期产出：一个 commit（Step 0 基线）+ 一个 commit（Step 1 Texture 重构），两步都**不改外部行为**，仅做准备工作。

---

## Step 0 — 准备与基线

### 0.1 仓库卫生

```powershell
cd C:\Project\vulkan_learn
git status
git branch --show-current
```

- 如果 `git status` 非空：先 `git stash -u` 保存，或自行提交；不要带脏工作区进入。
- 如果已在功能分支上继续；否则：
  ```powershell
  git checkout -b feature/gltf-v1
  ```

### 0.2 基线构建与截图

```powershell
# Debug（优先，validation layer 全开）
cmake --build build-debug --config Debug 2>&1 | Select-String -Pattern "error|Error" -NotMatch | Select-Object -Last 20
# Release（发布配置回归）
cmake --build build --config Release 2>&1 | Select-String -Pattern "error|Error" -NotMatch | Select-Object -Last 20
```

运行并**手工**记录基线：

```powershell
cd C:\Project\vulkan_learn\build\Release
.\VulkanLab.exe
```

- 启动后用 GUI 切换 "Viking Room" / "Sheen Chair" 各看一眼。
- 截图保存到 `doc/gltf/baseline/step0_viking.png` 和 `doc/gltf/baseline/step0_sheenchair.png`（可选但强烈推荐，后续肉眼回归比对）。
- 控制台若出现 validation error：**先记录下来**，不要顺手修 —— 可能是既有问题，不归本次 PR。

### 0.3 可选 — 引入额外测试模型

从 `KhronosGroup/glTF-Sample-Assets` 下载（只要 glb 即可，不要放进 git 仓库，仅本地用于人眼验证）：
- `BoxTextured.glb` — 最小带贴图模型
- `DamagedHelmet.glb` — 含 PBR 完整材质

放到 `models/`，**不加入 git**（在 `.gitignore` 里加 `models/BoxTextured.glb` 等行，或直接放 `models/sample/` 并忽略 `models/sample/`）。

### 0.4 Step 0 验收清单

| 项 | 判据 |
| --- | --- |
| 分支正确 | `git branch --show-current` == `feature/gltf-v1` |
| Debug build | 0 error |
| Release build | 0 error |
| Viking Room 运行 | 启动成功，贴图正常 |
| SheenChair 运行 | 启动成功（贴图错误是已知项） |
| 基线截图 | `doc/gltf/baseline/` 下 2 张 PNG（可选） |

### 0.5 Step 0 提交

```powershell
# 若引入了 baseline 图片或 .gitignore 改动
git add doc/gltf/baseline .gitignore
git commit -m "docs(gltf): step0 baseline screenshots and working branch"
```
若无需提交文件，直接用这次构建后的 HEAD 作为基线，不强制 commit。

---

## Step 1 — `Texture` 支持从内存像素构造

### 1.1 目标复述

仅重构 [src/render/Texture.h](../../src/render/Texture.h) / [src/render/Texture.cpp](../../src/render/Texture.cpp)：

- 新增 `struct TextureCreateInfo` + `Texture(Device&, FrameSync&, const TextureCreateInfo&)` 构造。
- 抽取私有辅助 `createFromPixels(...)` 和 `createSamplerFrom(...)`。
- 旧 `Texture(Device&, FrameSync&, const std::string&)` 构造**函数签名不变**，内部改为走新路径（`stbi_load` → `createFromPixels` → `createSamplerFrom(默认值)`）。
- **无外部调用点改动**。

### 1.2 改动前检索（避免漏改）

```powershell
# 找所有构造 Texture 的调用点
rg -n "std::make_(shared|unique)<Texture>|new Texture\(|Texture\s*\w*\(" src include --type cpp --type h
```

预期结果（当前）：
- [src/scene/BuiltinScenes.cpp](../../src/scene/BuiltinScenes.cpp) 内 `std::make_shared<Texture>(device, frameSync, tex)` — 旧签名，**不动**。
- [src/render/Material.cpp](../../src/render/Material.cpp) / [.h](../../src/render/Material.h) — 只引用 `Texture&`，不构造。

若出现未预期的调用点，在动工前记录下来。

### 1.3 文件改动详单

#### 1.3.1 [src/render/Texture.h](../../src/render/Texture.h) — 新增 + 扩展

在 `namespace vkr {` 内，`class Texture` 之前添加结构体：

```cpp
struct TextureCreateInfo {
    const void*          pixels = nullptr;   // 必须：RGBA8，tightly packed
    uint32_t             width  = 0;
    uint32_t             height = 0;
    bool                 generateMipmaps = true;
    VkFormat             format          = VK_FORMAT_R8G8B8A8_SRGB;
    VkFilter             minFilter       = VK_FILTER_LINEAR;
    VkFilter             magFilter       = VK_FILTER_LINEAR;
    VkSamplerMipmapMode  mipmapMode      = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSamplerAddressMode wrapU           = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode wrapV           = VK_SAMPLER_ADDRESS_MODE_REPEAT;
};
```

在 `class Texture` 的 `public:` 区，紧跟现有构造之后，加：

```cpp
Texture(Device& device, FrameSync& frameSync, const TextureCreateInfo& info);
```

在 `private:` 区，删除/改写 `loadFromFile` + `createSampler`，改为：

```cpp
// 从已解码的 RGBA8 像素创建 Image + 上传 + (可选) mipmap
void createFromPixels(FrameSync& frameSync,
                      const void* pixels, uint32_t width, uint32_t height,
                      VkFormat format, bool generateMipmaps);

// 根据 filter/wrap 参数创建 sampler_
void createSamplerFrom(VkFilter minFilter, VkFilter magFilter,
                       VkSamplerMipmapMode mipmapMode,
                       VkSamplerAddressMode wrapU,
                       VkSamplerAddressMode wrapV);
```

现有的 `transitionImageLayout` / `copyBufferToImage` / `generateMipmaps` 保持不变。

#### 1.3.2 [src/render/Texture.cpp](../../src/render/Texture.cpp) — 实现重排

现有代码：
```
Texture(Device&, FrameSync&, const std::string&)
  → loadFromFile(frameSync, path);     // 内部: stbi_load + Image 创建 + 上传 + mipmap
  → createSampler();                   // 固定参数

loadFromFile(...)
createSampler()                        // 无参，硬编码 REPEAT + LINEAR
transitionImageLayout / copyBufferToImage / generateMipmaps
```

重构后：
```
Texture(Device&, FrameSync&, const std::string& path)
  → stbi_load → createFromPixels(fs, pixels, w, h, VK_FORMAT_R8G8B8A8_SRGB, true)
  → stbi_image_free(pixels)
  → createSamplerFrom(LINEAR,LINEAR,MIPMAP_LINEAR,REPEAT,REPEAT)  // 原默认

Texture(Device&, FrameSync&, const TextureCreateInfo& info)
  → createFromPixels(fs, info.pixels, info.width, info.height,
                     info.format, info.generateMipmaps)
  → createSamplerFrom(info.minFilter, info.magFilter, info.mipmapMode,
                      info.wrapU, info.wrapV)

createFromPixels(...)                  // 原 loadFromFile 中从 "Buffer staging" 到
                                       //   generateMipmaps + createView 的整段
createSamplerFrom(min, mag, mipmap, u, v)   // 原 createSampler 的 body，
                                            //   REPEAT/LINEAR 换成参数
```

关键改动点（code surgery 清单）：

1. 把 `loadFromFile` 里 `stbi_load` 下面、`stbi_image_free` 上面的一大段（从 `mipLevels_ = ...` 到最末 `image_->createView(...)`）原样搬到 `createFromPixels`，把硬编码的 `VK_FORMAT_R8G8B8A8_SRGB` 替换为参数 `format`；把"总是生成 mipmap"改为 `if (generateMipmaps) { mipLevels_ = ...; ... generateMipmaps(...); } else { mipLevels_ = 1; /* 不调 generateMipmaps */ }`。
2. `createSampler` → `createSamplerFrom(min, mag, mip, u, v)`，把 4 处硬编码替换为参数：
   - `samplerInfo.magFilter = magFilter;`
   - `samplerInfo.minFilter = minFilter;`
   - `samplerInfo.addressModeU = wrapU;`
   - `samplerInfo.addressModeV = wrapV;`
   - `samplerInfo.addressModeW = wrapU;`  ← 用 U 兜底（本项目都是 2D）
   - `samplerInfo.mipmapMode = mipmapMode;`
   - 其余保持（`anisotropyEnable`、`borderColor`、`compareOp`、`maxLod=VK_LOD_CLAMP_NONE`）。
3. 旧 path 构造：
   ```cpp
   Texture::Texture(Device& device, FrameSync& frameSync, const std::string& path)
       : device_(&device) {
       int w, h, c;
       stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &c, STBI_rgb_alpha);
       if (!pixels) throw std::runtime_error("failed to load texture image!");
       createFromPixels(frameSync, pixels, (uint32_t)w, (uint32_t)h,
                        VK_FORMAT_R8G8B8A8_SRGB, /*genMips*/ true);
       stbi_image_free(pixels);
       createSamplerFrom(VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                         VK_SAMPLER_MIPMAP_MODE_LINEAR,
                         VK_SAMPLER_ADDRESS_MODE_REPEAT,
                         VK_SAMPLER_ADDRESS_MODE_REPEAT);
   }
   ```
4. 新 path 构造：
   ```cpp
   Texture::Texture(Device& device, FrameSync& frameSync,
                    const TextureCreateInfo& info)
       : device_(&device) {
       if (!info.pixels || info.width == 0 || info.height == 0)
           throw std::runtime_error("TextureCreateInfo: missing pixels/size");
       createFromPixels(frameSync, info.pixels, info.width, info.height,
                        info.format, info.generateMipmaps);
       createSamplerFrom(info.minFilter, info.magFilter, info.mipmapMode,
                         info.wrapU, info.wrapV);
   }
   ```
5. 析构不变（`vkDestroySampler`）。

> 注意：`maxLod` 若 `generateMipmaps=false` 仍可保留 `VK_LOD_CLAMP_NONE`，因为实际 `mipLevels_ = 1`，采样器会自己收敛。无需额外分支。

### 1.4 验证脚本

```powershell
# 编译
cmake --build build-debug --config Debug 2>&1 | Select-String "error" | Select-Object -First 20

# 运行
cd C:\Project\vulkan_learn\build-debug\Debug   # 或 build\Release
.\VulkanLab.exe
```

人眼对比：
- Viking Room 的木纹、屋顶、门牌字样与 **Step 0 基线截图逐像素接近**（允许窗口尺寸差异）。
- SheenChair 表现与基线完全一致（仍贴着 viking_room，这是已知项）。

Validation layer：
- 控制台无新增 `VUID-*` 警告。
- 可选：环境变量 `VK_LAYER_MESSAGE_ID_FILTER` 为空；让所有 warning 输出。

### 1.5 Step 1 自测检查表

| 项 | 判据 |
| --- | --- |
| 头文件编译 | `TextureCreateInfo` 可被其他 TU 包含（手动加一个 `#include "render/Texture.h"` 的空测试 TU 可选） |
| 旧构造 API 未变 | `rg "Texture\(.*frameSync" src` 结果跟基线相同 |
| Debug build | 0 error |
| Release build | 0 error |
| 运行 Viking Room | 画面与基线一致 |
| 运行 SheenChair | 画面与基线一致（即错误贴图也一致） |
| validation | 无新增 VUID |
| `mipLevels_=1` 分支 | 可跳过，Step 1 不真正走这条路（Step 5 才触发）。但单步 debugger 看一下 `generateMipmaps=false` 分支不会触发就行 |

### 1.6 Step 1 提交

```powershell
git add src/render/Texture.h src/render/Texture.cpp
git commit -m "refactor(texture): add TextureCreateInfo ctor; reuse for path-based ctor

- Extract createFromPixels / createSamplerFrom as private helpers.
- Path-based ctor now forwards to the new pixel-based path.
- No behavior change for existing callers (verified visually)."
```

### 1.7 回滚

```powershell
git revert HEAD          # 保留历史
# 或
git reset --hard HEAD~1  # 丢弃本次 commit
```
Step 1 是纯内部重构，回滚后不影响任何外部模块。

---

## 常见陷阱

1. **`generateMipmaps` + `Image` 创建**：当前 `Image` 构造吃 `mipLevels` 参数，所以 `createFromPixels` 里要先算出 `mipLevels_`，再构造 `Image`。若 `genMips=false`，`mipLevels_ = 1`，且**不要**调 `Texture::generateMipmaps(...)`（否则 `mipLevels-1=0` 的循环不进入，但 layout transition 少了最后那步 → 图像停在 `TRANSFER_DST_OPTIMAL`，sampling 会炸）。需要补一条"单独把 mip 0 从 `TRANSFER_DST_OPTIMAL` 转到 `SHADER_READ_ONLY_OPTIMAL`"的命令：
   ```cpp
   if (!generateMipmaps) {
       VkCommandBuffer cmd = frameSync.beginSingleTimeCommands();
       transitionImageLayout(cmd, image_->handle(), format,
           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, /*mipLevels*/1);
       frameSync.endSingleTimeCommands(cmd);
   }
   ```
   Step 1 默认 `genMips=true`，这段代码在 Step 1 不被触发，但必须存在才算完成"可调"。
2. **`VkFormat` 与像素数据**：`TextureCreateInfo.format` 默认 `VK_FORMAT_R8G8B8A8_SRGB` 适用于 baseColor；法线 / metalRough 将来走 `UNORM`。Step 1 不涉及，但留好参数即可。
3. **`addressModeW`**：本项目都是 2D 贴图，用 `wrapU` 的值兜底完全正确。
4. **anisotropy 设备能力**：`createSamplerFrom` 仍然查询 `maxSamplerAnisotropy`，不需要参数化。若将来有设备不支持，再加 `deviceFeatures_.samplerAnisotropy` 判断。

---

## Step 0+1 整体时序图

```
┌─────────────┐   git checkout   ┌─────────────┐   build + run   ┌─────────────┐
│  main / dev │ ───────────────► │ feature/..  │ ───────────────►│ Step 0 基线  │
└─────────────┘                  └─────────────┘                 └──────┬──────┘
                                                                        │
                                                                        ▼
                                                              ┌───────────────────┐
                                                              │ 编辑 Texture.h/.cpp│
                                                              │  抽 helpers        │
                                                              │  新 ctor           │
                                                              └─────────┬─────────┘
                                                                        ▼
                                                              ┌───────────────────┐
                                                              │ Debug+Release     │
                                                              │ build & run 对照  │
                                                              └─────────┬─────────┘
                                                                        ▼
                                                              ┌───────────────────┐
                                                              │ commit Step 1     │
                                                              └───────────────────┘
```

完成 Step 1 后即可进入 [implementation_steps.md#step-2](./implementation_steps.md) 的 Step 2。
