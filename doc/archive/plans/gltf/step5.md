# Step 5 具体执行计划

> 上游：[implementation_steps.md#step-5](./implementation_steps.md) · 前置 commit：Step 4 (`81d9706`)
> 产出：单个 commit，重写 `GltfLoader::load(...)` 返回 `GltfAsset`；让 `sheenChairSceneFactory` **临时**接入新接口验证（Step 7 才做最终扶正）。
> 关键约束：**API 破坏性变更** — 旧的 `static vector<unique_ptr<Mesh>> load(...)` 删除。所有调用点必须同时改完，否则编译失败。

---

## 5.1 影响范围

| 文件 | 动作 |
| --- | --- |
| [src/render/GltfLoader.h](../../../../src/render/GltfLoader.h) | 接口替换：返回 `GltfAsset`，新增 `Options` |
| [src/render/GltfLoader.cpp](../../../../src/render/GltfLoader.cpp) | 全文重写：images→Texture, materials→Material, primitives→Mesh, **node 暂用单位阵** |
| [src/scene/BuiltinScenes.cpp](../../../../src/scene/BuiltinScenes.cpp) | `sheenChairSceneFactory` 改为消费 `GltfAsset`（保留 `tex` 形参以避免改 main.cpp，本步骤内**忽略** `tex`） |

**不改**：`vikingRoomSceneFactory` / main.cpp / shader / Material / Texture / Mesh / GltfAsset.h。

> Step 6 会接 node 层级；Step 7 才删 `sheenChairSceneFactory` 的 `tex` 形参。

---

## 5.2 设计要点

### 5.2.1 新接口

```cpp
// GltfLoader.h
#include "GltfAsset.h"
#include "Renderer.h"
#include "core/PipelineConfig.h"

class GltfLoader {
  public:
    struct Options {
        bool                     generateMissingNormals = true; // v1 暂用 (0,1,0)
        std::shared_ptr<Texture> fallbackWhite;                 // 可跨场景复用
    };

    static GltfAsset load(const std::string&    path,
                          Device&               device,
                          FrameSync&            frameSync,
                          Renderer&             renderer,
                          const PipelineConfig& baseConfig,
                          const Options&        opts = {});
};
```
**删掉**旧的 `static vector<unique_ptr<Mesh>> load(...)`。

### 5.2.2 Loader 流程（7 段）

1. **解析**：与现状相同，但 `opts.images_as_is = 0`（让 tg3 解码 PNG/JPG）。错误处理保留。
2. **fallback white 1×1**：`opts.fallbackWhite` 优先；否则用 `Texture(TextureCreateInfo{4 字节 0xFF})` 现场造一张。
3. **images → `imageTextures` (vector<shared_ptr<Texture>>)**：
   - 每个 `tg3_image`：要求 `bits == 8`（`bits_per_channel`），否则用 `whiteTex` 占位并记一条 stderr 警告（不抛）。
   - 通道数 `image.component`：1/3/4 全部展平为 RGBA8（辅助函数 `expandToRgba8`）；2 通道（RG）罕见，按 `(R,G,0,255)` 处理。
   - 用 `Texture(device, fs, TextureCreateInfo{ pixels, w, h, generateMipmaps=true, format=VK_FORMAT_R8G8B8A8_SRGB, ... })`。
   - **注意**：`base_color` 走 SRGB；将来 `metallic/roughness/normal` 等走 UNORM——本步不区分，统一 SRGB（v1 限制，可接受，因 v1 只用 baseColor）。
4. **textures → `asset.textures`**：
   - v1 简化：`asset.textures[t] = imageTextures[m->textures[t].source]`，sampler 暂忽略（用默认 LINEAR/REPEAT，已写在 image 那一步）。
   - 完整 sampler 拆分（同一 image 多 sampler 副本）→ 留待 v2。
5. **materials → `asset.materials`**：
   - 每 `tg3_material` 翻成 `MaterialParams`：
     - `baseColor` ← `asset.textures[pbr.base_color_texture.index]`，越界/无 → `whiteTex`
     - `baseColorFactor` ← `pbr.base_color_factor[0..3]` 转 float
     - `metallicFactor` / `roughnessFactor` ← `pbr.metallic_factor / roughness_factor`
     - `emissiveFactor` ← `material.emissive_factor[0..3]`
     - `alphaCutoff` ← `material.alpha_cutoff`
     - `doubleSided` ← `material.double_sided != 0`
   - 用 `Material(device, renderer, std::move(params), baseConfig)` 构造。
   - 末尾**追加** fallback material：`MaterialParams{whiteTex}` + 默认 factors，用于 `prim.material == -1` 的情形。
6. **primitives → Mesh + 索引**：
   - 复用现有 attribute/index 解析逻辑（POSITION / TEXCOORD_0 / indices）。
   - **新增 NORMAL accessor 读取**：找 `"NORMAL"`，按 `vec3 float` 步长读；找不到则 fallback `(0,1,0)`。这一步替代 Step 2 中 `v.normal = {0,1,0}` 的占位。
   - 维护 `vector<vector<PrimEntry>> primsByMesh`，`PrimEntry{mesh, materialIdx}`。
7. **objects（v1 临时实现）**：遍历所有 mesh 的所有 primitive，每个生成一个 `transform=identity` 的 `SceneObject`，绑对应 `Material`（材质越界用 fallback）。
   - **Step 6 会替换**为 node 递归。

### 5.2.3 sampler 翻译（暂时跳过）

v1 全部用 `LINEAR/LINEAR/REPEAT/REPEAT`（与 viking_room 一致），节省工作量。完整翻译表见 [implementation_steps.md 附录 A](./implementation_steps.md#附录-a--sampler-常量翻译表)，留给 v2。

### 5.2.4 SheenChair 场景工厂临时接入

```cpp
// BuiltinScenes.cpp
SceneFactory sheenChairSceneFactory(std::string tex /*ignored in v1*/,
                                    std::string vp, std::string fp) {
    return [vp = std::move(vp), fp = std::move(fp)](
               Device &device, FrameSync &fs, Renderer &r) {
        auto scene = std::make_unique<Scene>();
        auto baseCfg = makeStandardConfig(device, vp, fp);
        auto asset = GltfLoader::load("models/SheenChair.glb",
                                      device, fs, r, baseCfg);
        for (auto &t : asset.textures)  scene->addTexture(t);
        for (auto &m : asset.materials) scene->addMaterial(m);
        for (auto &m : asset.meshes)    scene->addMesh(m);
        for (auto &o : asset.objects)   scene->addObject(o);
        scene->initialCamera = CameraPose{{1.5f, 1.5f, 1.0f}, -135.0f, -20.0f};
        return scene;
    };
}
```

`tex` 形参保留但加 `(void)tex;` 抑制警告，main.cpp 0 改动。

---

## 5.3 关键代码骨架

### 5.3.1 `expandToRgba8`

```cpp
static std::vector<uint8_t> expandToRgba8(const uint8_t *src, int w, int h,
                                          int c) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    for (int i = 0; i < w * h; ++i) {
        out[4*i+0] = (c >= 1) ? src[c*i+0] : 0;
        out[4*i+1] = (c >= 2) ? src[c*i+1] : out[4*i+0];
        out[4*i+2] = (c >= 3) ? src[c*i+2] : out[4*i+0];
        out[4*i+3] = (c >= 4) ? src[c*i+3] : 255;
    }
    return out;
}
```

### 5.3.2 white 1×1 helper

```cpp
static std::shared_ptr<Texture> makeWhite1x1(Device &d, FrameSync &fs) {
    static const uint8_t kWhite[4] = {255, 255, 255, 255};
    TextureCreateInfo ci;
    ci.pixels = kWhite;
    ci.width = ci.height = 1;
    ci.generateMipmaps = false;
    ci.format = VK_FORMAT_R8G8B8A8_SRGB;
    return std::make_shared<Texture>(d, fs, ci);
}
```

### 5.3.3 NORMAL 读取（在 primitive 循环里）

```cpp
const tg3_str_int_pair *nAttr = findAttr(prim, "NORMAL");
const uint8_t *nBase = nullptr; int32_t nStride = 0;
if (nAttr) { nBase = accessorBase(m, nAttr->value);
             nStride = accessorStride(m, nAttr->value); }
...
if (nBase) {
    const float *n = reinterpret_cast<const float*>(
        nBase + static_cast<size_t>(i) * nStride);
    v.normal = {n[0], n[1], n[2]};
} else {
    v.normal = {0.0f, 1.0f, 0.0f};
}
```

### 5.3.4 textures + materials 翻译

```cpp
// images
std::vector<std::shared_ptr<Texture>> imageTextures;
imageTextures.reserve(m->images_count);
for (uint32_t i = 0; i < m->images_count; ++i) {
    const tg3_image &img = m->images[i];
    if (img.bits != 8 || img.image.data == nullptr) {
        imageTextures.push_back(whiteTex);
        std::fprintf(stderr,
            "[GltfLoader] image[%u] (bits=%d) unsupported, using white.\n",
            i, img.bits);
        continue;
    }
    auto rgba = expandToRgba8(img.image.data, img.width, img.height,
                              img.component);
    TextureCreateInfo ci;
    ci.pixels = rgba.data();
    ci.width  = static_cast<uint32_t>(img.width);
    ci.height = static_cast<uint32_t>(img.height);
    ci.generateMipmaps = true;
    ci.format = VK_FORMAT_R8G8B8A8_SRGB;
    imageTextures.push_back(std::make_shared<Texture>(device, frameSync, ci));
}

// textures
asset.textures.reserve(m->textures_count);
for (uint32_t i = 0; i < m->textures_count; ++i) {
    const tg3_texture &t = m->textures[i];
    if (t.source >= 0 && t.source < (int)imageTextures.size())
        asset.textures.push_back(imageTextures[t.source]);
    else
        asset.textures.push_back(whiteTex);
}

// materials
auto fallbackMat = std::make_shared<Material>(device, renderer,
    MaterialParams{whiteTex}, baseConfig);

asset.materials.reserve(m->materials_count + 1);
for (uint32_t i = 0; i < m->materials_count; ++i) {
    const tg3_material &gm = m->materials[i];
    MaterialParams p;
    int bc = gm.pbr_metallic_roughness.base_color_texture.index;
    p.baseColor = (bc >= 0 && bc < (int)asset.textures.size())
                  ? asset.textures[bc] : whiteTex;
    const double *bcf = gm.pbr_metallic_roughness.base_color_factor;
    p.baseColorFactor = { (float)bcf[0],(float)bcf[1],(float)bcf[2],(float)bcf[3] };
    p.metallicFactor  = (float)gm.pbr_metallic_roughness.metallic_factor;
    p.roughnessFactor = (float)gm.pbr_metallic_roughness.roughness_factor;
    const double *ef = gm.emissive_factor;
    p.emissiveFactor  = { (float)ef[0],(float)ef[1],(float)ef[2] };
    p.alphaCutoff     = (float)gm.alpha_cutoff;
    p.doubleSided     = gm.double_sided != 0;
    asset.materials.push_back(std::make_shared<Material>(
        device, renderer, std::move(p), baseConfig));
}
asset.materials.push_back(fallbackMat);   // 最后一个永远是 fallback
```

### 5.3.5 v1 objects（identity transform）

```cpp
struct PrimEntry { std::shared_ptr<Mesh> mesh; int materialIdx; };
std::vector<std::vector<PrimEntry>> primsByMesh(m->meshes_count);
// ... 在 primitive 循环里 push 进去 ...

// Step 6 会替换为 node 递归
for (auto &list : primsByMesh) {
    for (auto &pe : list) {
        auto mat = (pe.materialIdx >= 0 &&
                    pe.materialIdx < (int)m->materials_count)
                   ? asset.materials[pe.materialIdx] : fallbackMat;
        asset.objects.push_back({pe.mesh, mat, glm::mat4(1.0f)});
    }
}
```

注意：`asset.materials` 的最后一个是 fallback，所以 `materialIdx` 用 `< m->materials_count` 比较即可保持索引一致。

---

## 5.4 已知限制与故障定位

| 项 | v1 行为 | 之后 |
| --- | --- | --- |
| sampler 拆分 | 全 LINEAR/REPEAT | v2 拆 |
| metallicRoughness/normal/occlusion 贴图 | 不读 | v2 |
| 16-bit / KTX2 / RGBE 图 | 报警告退 white | v2 |
| node 层级 | identity transform，组件可能散开 | Step 6 |
| double_sided | 字段已读，但 PipelineConfig 没消费 | 留待将来加 cullMode |

| 现象 | 原因 | 处置 |
| --- | --- | --- |
| 椅子贴图全黑 | image 解码失败（bits!=8） | 看 stderr 警告；确认 .glb 内嵌为 PNG/JPG |
| 椅子位置/朝向乱 | identity transform | 预期，Step 6 修 |
| validation `descriptorPool: max sets exceeded` | Material 数量很多 → 每个 Material 都 pool 1 set，未来 Loader 共享池 | 暂不处理 |
| validation `image layout undefined` | mipmap 链 transition 漏 | 由 Texture 内部已处理（Step 1 修过）；若仍报 → 检查 1×1 white 用 `generateMipmaps=false` 路径 |
| 编译错 `cannot convert vector<unique_ptr<Mesh>>` | 旧调用点没改完 | 全局 `grep "GltfLoader::load"` 确认全部走新签名 |

---

## 5.5 验证

### 5.5.1 编译
```powershell
cmake --build build-debug --config Debug   2>&1 | Select-String "error " | Select-Object -First 30
cmake --build build       --config Release 2>&1 | Select-String "error " | Select-Object -First 30
```

### 5.5.2 运行
- Viking Room：与 Step 4 完全一致（Loader 路径未触及它）。
- SheenChair：
  - **现在贴上 glb 内嵌的木纹/布纹**，**不再**是 viking_room！
  - 各组件位置可能错乱（identity transform）—— 这是预期。
  - 控制台无 VUID。
  - 有 `[GltfLoader] image[N] ...` 警告 → 看是哪张图、是否 16-bit。

### 5.5.3 故障兜底
- 如果 SheenChair 启动崩溃在 `Material` 构造，多半是某张 image 的 `bits != 8` 又没 fallback：检查 `imageTextures.push_back(whiteTex)` 路径覆盖。
- 如果黑屏但无 VUID：可能 `baseColorFactor` 全 0（glTF 里有这种风格化材质）—— 临时把 frag 里的 `*push.baseColorFactor` 改成 `* max(push.baseColorFactor, vec4(0.001))` 排除。

---

## 5.6 回滚
```powershell
git reset --hard HEAD     # 提交前
git revert HEAD           # 提交后
```
回滚后 Step 4 的 `GltfAsset.h` 仍在，无消费者。

---

## 5.7 Commit
```powershell
git add src/render/GltfLoader.h src/render/GltfLoader.cpp `
        src/scene/BuiltinScenes.cpp doc/gltf/step5.md
git commit -m "feat(gltf): loader emits GltfAsset (images+materials+meshes)

- New API: GltfLoader::load returns GltfAsset, takes Renderer + baseConfig.
- Decode glb-embedded images to RGBA8 SRGB (mipmapped).
- Translate glTF materials -> MaterialParams (baseColor + factors).
- Read NORMAL accessor when present; fallback +Y.
- Sampler nuances and per-sampler texture splits deferred to v2.
- SheenChair scene factory consumes GltfAsset (identity transform);
  node hierarchy handled in Step 6."
```

---

## 5.8 完成后状态
- SheenChair 正确贴图（虽然摆放仍乱）；Viking Room 不变。
- `GltfAsset` 有真实消费者；`Texture(TextureCreateInfo)` 路径首次被实战调用。
- Step 6 接入 node 递归只需改 Loader 第 7 段。
