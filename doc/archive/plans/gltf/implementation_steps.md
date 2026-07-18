# glTF 模块完善 — 分步实现方案

> 对应设计文档：[design.md](./design.md)
> 目标：把 [design.md](./design.md) 的 P1 → P3 落成可独立提交、可独立验证的 **7 个实现步骤**，每步给出**改动文件清单、关键代码骨架、验证方法、回滚点**。
> 约束：每一步结束时项目都能 **编译通过 + 运行不崩**；视觉上允许临时"难看"（比如 P1 后 SheenChair 仍是错的贴图）。

---

## 目录

- [Step 0 — 准备与基线](#step-0--准备与基线)
- [Step 1 — `Texture` 支持从内存像素构造](#step-1--texture-支持从内存像素构造)
- [Step 2 — `Vertex` 用 normal 替换 color（+ shader 同步）](#step-2--vertex-用-normal-替换-color-shader-同步)
- [Step 3 — `Material` 扩展 `MaterialParams` + push-constant 扩容](#step-3--material-扩展-materialparams--push-constant-扩容)
- [Step 4 — 新增 `GltfAsset` 数据包](#step-4--新增-gltfasset-数据包)
- [Step 5 — 重写 `GltfLoader`：images + materials + primitive→material](#step-5--重写-gltfloaderimages--materials--primitivematerial)
- [Step 6 — node 层级 → world matrix](#step-6--node-层级--world-matrix)
- [Step 7 — `BuiltinScenes` / `main.cpp` 接入 + 清理](#step-7--builtinscenes--maincpp-接入--清理)
- [附录 A — sampler 常量翻译表](#附录-a--sampler-常量翻译表)
- [附录 B — push constant 内存布局](#附录-b--push-constant-内存布局)

---

## Step 0 — 准备与基线

### 动作
1. `git status` 确认工作区干净；在 `feature/gltf-v1` 分支上工作。
2. 跑一次 Release：`cmake --build build --config Release` + 运行 `VulkanLab.exe`，截图 Viking Room 与 SheenChair 现状作为**回归基线**。
3. 把 SheenChair 的 glb 在 `models/` 下确认存在；可选下载 glTF-Sample-Assets 的 `DamagedHelmet` / `BoxTextured` 作为 P3 验证素材。

### 验证
- 两个场景均能启动，切换无崩溃。（SheenChair 此刻贴 viking_room 纹理是已知问题）

### 回滚点
- 任何后续步骤失败均可 `git reset --hard HEAD~1` 回到本 commit。

---

## Step 1 — `Texture` 支持从内存像素构造

### 目的
为 Step 5 做准备：让 `Texture` 能直接吃"已解码 RGBA8 + 宽高 + sampler 参数"，不依赖磁盘路径。

### 改动文件
- [src/render/Texture.h](../../../../src/render/Texture.h)
- [src/render/Texture.cpp](../../../../src/render/Texture.cpp)

### 代码骨架

**Texture.h** 新增：
```cpp
struct TextureCreateInfo {
    const void*          pixels = nullptr;   // 必须是 RGBA8，tightly packed
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

Texture(Device& device, FrameSync& frameSync, const TextureCreateInfo& info);
```

**Texture.cpp** 重构：
- 抽出私有方法 `createFromPixels(FrameSync&, const void*, uint32_t w, uint32_t h, VkFormat, bool genMips)`（原 `loadFromFile` 中从 staging buffer 到 `generateMipmaps` 的全部内容）。
- 抽出私有方法 `createSamplerWith(VkFilter min, VkFilter mag, VkSamplerMipmapMode, VkSamplerAddressMode u, VkSamplerAddressMode v)`。
- 原 `Texture(Device&, FrameSync&, const std::string&)` 改为：`stbi_load` → 调 `createFromPixels` → 调 `createSamplerWith(默认值)`。
- 新构造：直接调上面两个私有方法。

### 验证
- `cmake --build build --config Debug` 通过。
- 运行 Viking Room，贴图与 Step 0 基线一致。

### 回滚点
- 本步骤仅重构内部实现，外部调用点不变 → 回滚无风险。

---

## Step 2 — `Vertex` 用 normal 替换 color（+ shader 同步）

### 目的
为 PBR / 软光照准备顶点法线通道。本步骤会**同时**改 `Vertex`、`shader.vert/frag`、`Mesh::fromOBJ`。若分开改会中间态崩溃。

### 改动文件
- [src/render/Vertex.h](../../../../src/render/Vertex.h)
- [src/render/Mesh.cpp](../../../../src/render/Mesh.cpp)
- `shader/shader.vert`（旧路径：`../../../../shader/shader.vert`）
- `shader/shader.frag`（旧路径：`../../../../shader/shader.frag`）
- [shader/compile.bat](../../../../shader/compile.bat)（如需重新触发）

### 代码骨架

**Vertex.h**：
```cpp
struct Vertex {
    glm::vec3 pos;
    glm::vec3 normal;     // ← 原 color
    glm::vec2 texCoord;
    // hash / getAttributeDescriptions 中 color → normal 同步改名
};
```
`offsetof(Vertex, color)` 全部替换为 `offsetof(Vertex, normal)`；attribute format 仍是 `VK_FORMAT_R32G32B32_SFLOAT`，布局字节级兼容。

**Mesh.cpp (`fromOBJ`)**：
```cpp
vertex.normal = (index.normal_index >= 0)
    ? glm::vec3{ attrib.normals[3*index.normal_index + 0],
                 attrib.normals[3*index.normal_index + 1],
                 attrib.normals[3*index.normal_index + 2] }
    : glm::vec3{0.0f, 1.0f, 0.0f};
```
删掉 `vertex.color = {1,1,1}` 行。

**shader.vert**：
```glsl
layout(location = 1) in vec3 inNormal;
layout(location = 0) out vec3 fragNormalWS;
...
fragNormalWS = mat3(push.model) * inNormal;   // 仅刚体变换，暂不用 normalMatrix
```

**shader.frag**（临时软光照）：
```glsl
layout(location = 0) in vec3 fragNormalWS;
layout(location = 1) in vec2 fragTexCoord;
...
vec4 tex = texture(texSampler, fragTexCoord);
float ndl = max(dot(normalize(fragNormalWS), normalize(vec3(0.3,0.8,0.5))), 0.0);
outColor = vec4(tex.rgb * (0.25 + 0.75 * ndl), tex.a);
```

手动执行 `shader\compile.bat` 生成新的 `vert.spv / frag.spv`。

### 验证
- Debug 编译通过。
- Viking Room 启动：屋顶 / 侧面存在明暗对比（软光照生效）；贴图无 UV 错位。
- 控制台无 validation error。

### 回滚点
- 本步只动顶点结构 + shader。失败则 revert 这 5 个文件。

---

## Step 3 — `Material` 扩展 `MaterialParams` + push-constant 扩容

### 目的
让 `Material` 持有 PBR 因子；`Scene::render` push 一个 128B 结构。**注意**：此步保持旧的 `Material(Device&, Renderer&, const Texture&, const PipelineConfig&)` 构造**不删**，仅新增一个重载，避免调用点改动爆炸。

### 改动文件
- `src/render/Material.h`（旧路径：`../../../../src/render/Material.h`）
- `src/render/Material.cpp`（旧路径：`../../../../src/render/Material.cpp`）
- [src/scene/Scene.cpp](../../../../src/scene/Scene.cpp)
- [src/scene/BuiltinScenes.cpp](../../../../src/scene/BuiltinScenes.cpp)（只改 `makeStandardConfig` 里的 push size）
- `shader/shader.vert`（旧路径：`../../../../shader/shader.vert`）（push 结构扩容）
- `shader/shader.frag`（旧路径：`../../../../shader/shader.frag`）（消费 factors）

### 代码骨架

**Material.h**：
```cpp
struct MaterialParams {
    std::shared_ptr<Texture> baseColor;          // 必填
    glm::vec4                baseColorFactor{1.0f};
    glm::vec3                emissiveFactor{0.0f};
    float                    metallicFactor  = 1.0f;
    float                    roughnessFactor = 1.0f;
    float                    alphaCutoff     = 0.5f;
    bool                     doubleSided     = false;
};

// 新增重载（旧重载保留）
Material(Device&, Renderer&, MaterialParams params, const PipelineConfig&);

const MaterialParams& params() const { return params_; }
```
字段：私有 `MaterialParams params_;`。若构造用的是旧重载，`params_.baseColor` 置空、factors 全默认。

**Scene.cpp**：push 结构体（和 shader 保持一致布局）：
```cpp
struct GpuPushBlock {
    glm::mat4 model;
    glm::vec4 baseColorFactor;
    glm::vec4 emissiveMetallic; // xyz=emissive, w=metallic
    glm::vec4 roughnessAlpha;   // x=roughness, y=alphaCutoff, zw=0
};
static_assert(sizeof(GpuPushBlock) == 128);

// render() 内：
GpuPushBlock blk{};
blk.model = obj.transform;
const auto& p = obj.material->params();
blk.baseColorFactor  = p.baseColor ? p.baseColorFactor : glm::vec4(1.0f);
blk.emissiveMetallic = glm::vec4(p.emissiveFactor, p.metallicFactor);
blk.roughnessAlpha   = glm::vec4(p.roughnessFactor, p.alphaCutoff, 0, 0);
vkCmdPushConstants(cmd, pipeline.layout(),
                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                   0, sizeof(GpuPushBlock), &blk);
```

**BuiltinScenes.cpp** 的 `makeStandardConfig`：
```cpp
cfg.pushConstants = {{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, 128 }};
```

**shader.vert / frag**：push block 扩容到 128B（见附录 B）；frag 使用 `push.baseColorFactor.rgb` 乘 albedo，`emissiveMetallic.rgb` 作为自发光加法。

### 验证
- 编译通过；Viking Room 画面与 Step 2 一致（factors=1, emissive=0，视觉无变化）。
- 用 RenderDoc 或 validation layer 确认 push constant size==128 通过。

### 回滚点
- 本步失败可单独 revert：Material 新重载、Scene push 扩容、BuiltinScenes makeStandardConfig、2 个 shader。

---

## Step 4 — 新增 `GltfAsset` 数据包

### 目的
纯新增类型，不动任何现有代码。为 Step 5 铺路。

### 改动文件
- 新增 [src/render/GltfAsset.h](../../../../src/render/GltfAsset.h)

### 代码骨架
```cpp
#pragma once
#include "scene/Scene.h"          // CameraPose
#include "scene/SceneObject.h"
#include <memory>
#include <optional>
#include <vector>

namespace vkr {
class Texture;
class Material;
class Mesh;

struct GltfAsset {
    std::vector<std::shared_ptr<Texture>>  textures;
    std::vector<std::shared_ptr<Material>> materials;
    std::vector<std::shared_ptr<Mesh>>     meshes;    // 一个 primitive → 一个 Mesh
    std::vector<SceneObject>               objects;   // 已展开的实例（transform=world）
    std::optional<CameraPose>              suggestedCamera;
};
} // namespace vkr
```

### 验证
- 编译通过（没有消费者，纯头文件）。

### 回滚点
- 删掉该文件即可。

---

## Step 5 — 重写 `GltfLoader`：images + materials + primitive→material

### 目的
把 Loader 从"只解几何"升级到"解 images + textures + samplers + materials + primitives"，输出 `GltfAsset`。**node 层级暂用单位阵**（放 Step 6）。

### 改动文件
- [src/render/GltfLoader.h](../../../../src/render/GltfLoader.h)
- [src/render/GltfLoader.cpp](../../../../src/render/GltfLoader.cpp)

### 代码骨架

**GltfLoader.h**：
```cpp
class GltfLoader {
  public:
    struct Options {
        bool                     generateMissingNormals = true;
        std::shared_ptr<Texture> fallbackWhite;   // 可跨场景复用
    };

    static GltfAsset load(const std::string&    path,
                          Device&               device,
                          FrameSync&            frameSync,
                          Renderer&             renderer,
                          const PipelineConfig& baseConfig,
                          const Options&        opts = {});
};
```
原 `static vector<unique_ptr<Mesh>> load(...)` **删除**。

**GltfLoader.cpp** 关键流程：
```cpp
GltfAsset GltfLoader::load(...) {
    // 1. 解析：images_as_is = 0
    tg3_parse_options opts; tg3_parse_options_init(&opts);
    opts.images_as_is = 0;   // 让 tg3 解码 PNG/JPG
    ...parse_file...

    const tg3_model* m = model.get();
    GltfAsset asset;

    // 2. fallback white 1x1
    auto whiteTex = opts.fallbackWhite ? opts.fallbackWhite
                                       : makeWhite1x1(device, frameSync);

    // 3. 解码后 images → 中间 vector<shared_ptr<Texture>> imageTextures
    //    对 RGB8 补 alpha=255；grayscale 复制成 RRR-1。
    //    注意：tg3 返回 bits_per_channel 可能为 16，这时 v1 报错跳过（fallback white）。

    // 4. textures[]：每个 tg3_texture = imageTexture[source] 的"同像素、可能不同 sampler" 副本。
    //    v1 简化：直接把 imageTextures[source] 复用，sampler 只取每张 image 首个 sampler。
    //    即 asset.textures[t] = imageTextures[m->textures[t].source];
    //    （真实的多 sampler 场景 v2 再拆。）

    // 5. materials
    asset.materials.reserve(m->materials_count + 1);
    for (uint32_t i = 0; i < m->materials_count; ++i) {
        const auto& gm = m->materials[i];
        MaterialParams p;
        int bc = gm.pbr_metallic_roughness.base_color_texture.index;
        p.baseColor = (bc >= 0 && bc < (int)asset.textures.size())
                      ? asset.textures[bc] : whiteTex;
        const double* bcf = gm.pbr_metallic_roughness.base_color_factor;
        p.baseColorFactor  = {(float)bcf[0],(float)bcf[1],(float)bcf[2],(float)bcf[3]};
        p.metallicFactor   = (float)gm.pbr_metallic_roughness.metallic_factor;
        p.roughnessFactor  = (float)gm.pbr_metallic_roughness.roughness_factor;
        const double* ef = gm.emissive_factor;
        p.emissiveFactor = {(float)ef[0],(float)ef[1],(float)ef[2]};
        p.doubleSided    = gm.double_sided != 0;
        asset.materials.push_back(std::make_shared<Material>(
            device, renderer, std::move(p), baseConfig));
    }
    // fallback material（baseColor=white, factors=default）
    auto fallbackMat = std::make_shared<Material>(device, renderer,
        MaterialParams{whiteTex}, baseConfig);

    // 6. primitives → Mesh + 记录每个 primitive 的 materialIndex
    struct PrimEntry { std::shared_ptr<Mesh> mesh; int materialIdx; };
    std::vector<std::vector<PrimEntry>> primsByMesh(m->meshes_count);
    for (uint32_t mi = 0; mi < m->meshes_count; ++mi) {
        for (uint32_t pi = 0; pi < m->meshes[mi].primitives_count; ++pi) {
            const auto& prim = m->meshes[mi].primitives[pi];
            // ... 读 POSITION / NORMAL (生成默认 if missing) / TEXCOORD_0 / indices
            auto mesh = std::make_shared<Mesh>(device, frameSync,
                                               verts.data(), verts.size()*sizeof(Vertex),
                                               indices.data(), (uint32_t)indices.size());
            asset.meshes.push_back(mesh);
            primsByMesh[mi].push_back({mesh, prim.material});
        }
    }

    // 7. v1: 不走 node；为 mesh[0] 的每个 primitive 生成一个 identity-transform SceneObject
    //    （Step 6 会替换这一块为真正的节点递归）
    for (auto& list : primsByMesh)
        for (auto& pe : list)
            asset.objects.push_back({
                pe.mesh,
                (pe.materialIdx >= 0 && pe.materialIdx < (int)asset.materials.size())
                    ? asset.materials[pe.materialIdx] : fallbackMat,
                glm::mat4(1.0f)
            });

    asset.materials.push_back(fallbackMat);
    return asset;
}
```

**图像 RGB8 → RGBA8 辅助**：
```cpp
static std::vector<uint8_t> expandToRgba8(const uint8_t* src, int w, int h, int c) {
    std::vector<uint8_t> out(w*h*4);
    for (int i = 0; i < w*h; ++i) {
        out[4*i+0] = c>=1 ? src[c*i+0] : 0;
        out[4*i+1] = c>=2 ? src[c*i+1] : out[4*i+0];
        out[4*i+2] = c>=3 ? src[c*i+2] : out[4*i+0];
        out[4*i+3] = c>=4 ? src[c*i+3] : 255;
    }
    return out;
}
```

**sampler 翻译**：见[附录 A](#附录-a--sampler-常量翻译表)。

### 验证
- 编译通过。
- **临时**将 `BuiltinScenes.cpp` 的 `sheenChairSceneFactory` 改为：
  ```cpp
  auto asset = GltfLoader::load("models/SheenChair.glb", device, fs, r, baseCfg);
  for (auto& t : asset.textures)  scene->addTexture(t);
  for (auto& m : asset.materials) scene->addMaterial(m);
  for (auto& m : asset.meshes)    scene->addMesh(m);
  for (auto& o : asset.objects)   scene->addObject(o);
  ```
- 运行 SheenChair：椅子显示**木纹 + 布面**（glb 内嵌贴图），**不再**是 viking_room 纹理。摆放可能散开/朝向不对（node 未处理），正常。
- Viking Room 不受影响。

### 回滚点
- 只动 Loader + SheenChair 工厂两处；回滚不影响 Viking Room。

---

## Step 6 — node 层级 → world matrix

### 目的
Loader 内部递归 `m->scenes[m->scene].nodes`（或 `scenes[0]`）展开 TRS/matrix 到 world matrix。替换 Step 5 末尾的临时"identity transform"段。

### 改动文件
- [src/render/GltfLoader.cpp](../../../../src/render/GltfLoader.cpp)（仅第 7 段）

### 代码骨架
```cpp
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

auto localOf = [&](const tg3_node& n) -> glm::mat4 {
    if (n.has_matrix)
        return glm::make_mat4(n.matrix);  // glTF matrix 已是列主序 float64 → 转 float
    glm::vec3 t(n.translation[0], n.translation[1], n.translation[2]);
    glm::quat q((float)n.rotation[3], (float)n.rotation[0],
                (float)n.rotation[1], (float)n.rotation[2]); // w,x,y,z
    glm::vec3 s(n.scale[0], n.scale[1], n.scale[2]);
    return glm::translate(glm::mat4(1.0f), t)
         * glm::mat4_cast(q)
         * glm::scale(glm::mat4(1.0f), s);
};

std::function<void(int, const glm::mat4&)> walk =
    [&](int idx, const glm::mat4& parent) {
        const tg3_node& n = m->nodes[idx];
        glm::mat4 world = parent * localOf(n);
        if (n.mesh >= 0 && n.mesh < (int)primsByMesh.size()) {
            for (auto& pe : primsByMesh[n.mesh]) {
                auto mat = (pe.materialIdx >= 0
                         && pe.materialIdx < (int)asset.materials.size())
                         ? asset.materials[pe.materialIdx] : fallbackMat;
                asset.objects.push_back({ pe.mesh, mat, world });
            }
        }
        for (uint32_t c = 0; c < n.children_count; ++c)
            walk(n.children[c], world);
    };

int sceneIdx = (m->scene >= 0 && m->scene < (int)m->scenes_count) ? m->scene : 0;
if (m->scenes_count > 0) {
    for (uint32_t r = 0; r < m->scenes[sceneIdx].nodes_count; ++r)
        walk(m->scenes[sceneIdx].nodes[r], glm::mat4(1.0f));
} else {
    // 无 scenes：遍历所有 mesh 当根
    for (size_t i = 0; i < primsByMesh.size(); ++i)
        for (auto& pe : primsByMesh[i])
            asset.objects.push_back({pe.mesh, fallbackMat, glm::mat4(1.0f)});
}
```

### 验证
- SheenChair 椅子朝向、各组件位置正确（椅面、椅腿、靠背在位）。
- 切换到 Viking Room 正常。
- 取 `DamagedHelmet.glb` / `BoxTextured.glb` 注册为第三个场景快速验证（可选）。

### 回滚点
- 仅回退 Loader 末尾一段，回到 identity-transform 版本。

---

## Step 7 — `BuiltinScenes` / `main.cpp` 接入 + 清理

### 目的
收尾：把"临时接入"扶正，去掉 `sheenChairSceneFactory` 的 `tex` 参数，main.cpp 同步。

### 改动文件
- [src/scene/BuiltinScenes.h](../../../../src/scene/BuiltinScenes.h)
- [src/scene/BuiltinScenes.cpp](../../../../src/scene/BuiltinScenes.cpp)
- [src/main.cpp](../../../../src/main.cpp)

### 代码骨架

**BuiltinScenes.h**：
```cpp
SceneFactory vikingRoomSceneFactory(std::string tex, std::string vp, std::string fp);
SceneFactory sheenChairSceneFactory(std::string vp, std::string fp);   // 去掉 tex
```

**BuiltinScenes.cpp**（最终版）：
```cpp
SceneFactory sheenChairSceneFactory(std::string vp, std::string fp) {
    return [vp=std::move(vp), fp=std::move(fp)](
               Device& device, FrameSync& fs, Renderer& r)
               -> std::unique_ptr<Scene> {
        auto scene = std::make_unique<Scene>();
        auto baseCfg = makeStandardConfig(device, vp, fp);
        auto asset = GltfLoader::load("models/SheenChair.glb",
                                      device, fs, r, baseCfg);
        for (auto& t : asset.textures)  scene->addTexture(t);
        for (auto& m : asset.materials) scene->addMaterial(m);
        for (auto& m : asset.meshes)    scene->addMesh(m);
        for (auto& o : asset.objects)   scene->addObject(o);
        scene->initialCamera = asset.suggestedCamera.value_or(
            CameraPose{{1.5f, 1.5f, 1.0f}, -135.0f, -20.0f});
        return scene;
    };
}
```
`vikingRoomSceneFactory` 保持不变（仍走 OBJ + 外部贴图）。

**main.cpp**：
```cpp
app.registerScene({"Sheen Chair",
    vkr::sheenChairSceneFactory(config.vertShaderPath, config.fragShaderPath)});
```

### 验证
- 全量清理后 Debug + Release 均 build 0 error。
- 交互验证：
  - Viking Room：贴图正常、软光照生效。
  - SheenChair：贴图正确（木纹+布纹）、摆放正确。
  - GUI 切换场景无 validation error、无泄漏（可用 `VK_LAYER_KHRONOS_validation` + `VK_EXT_debug_utils`）。

### 回滚点
- 本步只是接线；失败直接退回到 Step 6 结束状态（临时接入版本）。

---

## 总验证清单（完成全部 7 步后）

| 项 | 预期 |
| --- | --- |
| Viking Room 运行 | 贴图正确 + 有光照明暗 |
| SheenChair 运行 | 椅子多组件、正确贴图、正确摆放 |
| 切换场景 | 无 validation error、无 GPU 悬挂资源 |
| 反复切换 10+ 次 | 显存不增长（RenderDoc / Task Manager 侧观察） |
| Release build warning | 零或仅第三方库 |
| push constant 大小 | 128B，stageFlags = VS \| FS |

---

## 附录 A — sampler 常量翻译表

| glTF / OpenGL | Vulkan |
| --- | --- |
| `9728` NEAREST | `minFilter = VK_FILTER_NEAREST`, `mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST` |
| `9729` LINEAR | `minFilter = VK_FILTER_LINEAR`, `mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST` |
| `9984` NEAREST_MIPMAP_NEAREST | `NEAREST`, `NEAREST` |
| `9985` LINEAR_MIPMAP_NEAREST | `LINEAR`, `NEAREST` |
| `9986` NEAREST_MIPMAP_LINEAR | `NEAREST`, `LINEAR` |
| `9987` LINEAR_MIPMAP_LINEAR | `LINEAR`, `LINEAR` |
| magFilter 9728/9729 | `VK_FILTER_NEAREST` / `VK_FILTER_LINEAR` |
| wrap `10497` REPEAT | `VK_SAMPLER_ADDRESS_MODE_REPEAT` |
| wrap `33071` CLAMP_TO_EDGE | `VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE` |
| wrap `33648` MIRRORED_REPEAT | `VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT` |
| `sampler == -1`（未指定） | 默认 `LINEAR / LINEAR / REPEAT / REPEAT` |

对应常量见 [tiny_gltf_v3.h](../../../../external/gltf/tiny_gltf_v3.h) 第 142–152 行。

---

## 附录 B — push constant 内存布局

CPU 侧：
```cpp
struct GpuPushBlock {   // size = 128, alignof = 16
    glm::mat4 model;            // offset 0,  size 64
    glm::vec4 baseColorFactor;  // offset 64, size 16
    glm::vec4 emissiveMetallic; // offset 80, size 16  (xyz=emissive, w=metallic)
    glm::vec4 roughnessAlpha;   // offset 96, size 16  (x=roughness, y=alphaCutoff)
};
```
GLSL 侧（std430 不适用 push，等同 std140-like，但单 block 无数组 → 布局直观）：
```glsl
layout(push_constant) uniform Push {
    mat4 model;              // 0
    vec4 baseColorFactor;    // 64
    vec4 emissiveMetallic;   // 80
    vec4 roughnessAlpha;     // 96
} push;                      // total 128
```
`VkPushConstantRange`：
```cpp
range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
range.offset = 0;
range.size   = 128;
```
Vulkan 规范保证最小 `maxPushConstantsSize = 128`，可用。
