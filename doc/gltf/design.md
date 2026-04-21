# glTF 模块完善设计方案

> 版本：v1  · 面向当前 `VulkanLab` 架构（step6 完成，step7 进行中）
> 目标：在 **不破坏现有 Scene / Material / Pipeline / Renderer 约定** 的前提下，把 glTF 支持从"仅几何"提升到"几何 + 贴图 + PBR 因子 + 节点层级"，并预留动画 / 法线贴图 / alphaMode 的扩展位。

---

## 1. 现状与问题诊断

### 1.1 现有实现

| 位置 | 作用 | 现状 |
| --- | --- | --- |
| [src/render/GltfLoader.h](../../src/render/GltfLoader.h) | 对外 API | 只有 `static vector<unique_ptr<Mesh>> load(path, device, frameSync)` |
| [src/render/GltfLoader.cpp](../../src/render/GltfLoader.cpp) | 解析 | 遍历 `model.meshes[*].primitives[*]`，只读 `POSITION` + `TEXCOORD_0` + `indices`；`opts.images_as_is = 1`；完全忽略 `primitive.material / model.materials / model.textures / model.images / model.samplers / model.nodes / model.scenes` |
| [src/scene/BuiltinScenes.cpp](../../src/scene/BuiltinScenes.cpp) `sheenChairSceneFactory` | 组装场景 | 用外部传入的 `config.texturePath`（viking_room 纹理）给 SheenChair 的 **所有 primitive** 套同一 `Material`，节点 transform 全为单位阵 |
| [src/render/Vertex.h](../../src/render/Vertex.h) | 顶点格式 | `pos + color + texCoord`，**无 normal / tangent / 第二套 UV** |
| [src/render/Material.h](../../src/render/Material.h) | 材质 | 1 sampler2D（binding=1）+ push-constant model，**无 PBR 因子、无第二张贴图** |
| [shader/shader.frag](../../shader/shader.frag) | 片元 | `outColor = texture(texSampler, uv)`，**无光照、无 baseColorFactor、无 alpha** |

### 1.2 为什么 SheenChair 显示的是 viking_room 纹理

[main.cpp](../../src/main.cpp) 第 17 行：
```cpp
vkr::sheenChairSceneFactory(config.texturePath, ...)   // = textures/viking_room.png
```
Loader 不产出纹理，所以场景工厂只能把这张外部贴图强行塞给所有 primitive。这是**表象 Bug**；**根因**是 Loader 没有输出 `images + textures + materials + primitive→material 映射`。

### 1.3 gap 清单（按重要性排序）

1. **贴图缺失**：`tg3_image` 未被解码、未建 `Texture`；`primitive.material` 索引被丢。
2. **材质缺失**：`baseColorFactor / metallicFactor / roughnessFactor / emissiveFactor / alphaMode` 全部被丢。
3. **节点变换缺失**：SheenChair 的实际朝向依赖 `nodes[*].matrix / TRS` + 父子层级，被当成单位阵。
4. **法线缺失**：无 NORMAL 属性 → 无法做任何光照（现在 frag 直接吐贴图，勉强看起来"亮"是因为没有光照差异，颜色不会"歪"，但只要换个 PBR 模型立刻穿帮）。
5. **采样器缺失**：wrap / filter 用 Texture 默认值（`REPEAT + LINEAR + mipmap`），对 `CLAMP_TO_EDGE` 的贴图会溢出。
6. **顶点去重/morph/skin** 未支持（本阶段不做）。
7. **资源所有权混乱**：Loader 只返回 Mesh，场景需要自己配纹理，耦合且易错。

---

## 2. 设计目标 & 非目标

### 本次（v1）目标
- [G1] **Loader 自闭**：一次 `load()` 调用产出完整可渲染资源（Image + Texture + Material + Mesh + Object 列表 + 节点矩阵）。
- [G2] **SheenChair 正确显示**：使用 glb 内嵌的 baseColor 贴图和 baseColorFactor，不再依赖外部 `config.texturePath`。
- [G3] **架构零入侵**：不改 `Scene` / `Pipeline` / `Renderer` 现有契约；通过扩展 `Vertex`、`Material`、`Texture` 三个叶子类实现。
- [G4] **OBJ 路径不退化**：`Mesh::fromOBJ` + viking_room 继续跑通，顶点格式向后兼容。

### 非目标（留到后续步骤）
- PBR 直接光照 / IBL（这次仅在 frag 做 `albedo * baseColorFactor`）
- 法线贴图 / 遮蔽贴图
- `alphaMode = BLEND` 的透明排序
- Skin / Animation / Morph target
- KTX2 / basisU 压缩纹理
- 多场景（`model.scenes[n]`）选择

---

## 3. 架构总览

```
┌───────────────────────────────────────────────────────────────┐
│                       GltfLoader (static)                     │
│   parse → decode images → flatten nodes → build resources     │
│   输出 GltfAsset { textures, materials, meshes, objects }     │
└───────────────────────┬───────────────────────────────────────┘
                        │ (一次调用, 拷贝语义 shared_ptr)
                        ▼
┌───────────────────────────────────────────────────────────────┐
│                  BuiltinScenes / SceneFactory                 │
│   scene->addTexture/Material/Mesh/Object (直接搬运)            │
└───────────────────────┬───────────────────────────────────────┘
                        ▼
                  现有 Scene / Renderer 完全不变
```

关键原则：**Loader 负责把 glTF 语义翻译成 vkr 的资源对象**，场景工厂只做"摆放 + 相机 + 每帧 tick"。

---

## 4. 数据模型

### 4.1 新增顶层结构 `GltfAsset`

新建 [src/render/GltfAsset.h](../../src/render/GltfAsset.h)：
```cpp
struct GltfAsset {
    std::vector<std::shared_ptr<Texture>>  textures;   // 与 model.textures 一一对应，缺省用 1x1 白贴图兜底
    std::vector<std::shared_ptr<Material>> materials;  // 与 model.materials 对应 + 1 个 fallback
    std::vector<std::shared_ptr<Mesh>>     meshes;     // 扁平，一个 primitive 一个 Mesh
    std::vector<SceneObject>               objects;    // primitive × node instance，transform 已是世界矩阵
    std::optional<CameraPose>              suggestedCamera; // 由 bbox 推导（可选）
};
```

> 注意：**一个 `model.mesh` 可能有多个 primitive，每个 primitive 可能引用不同 material**。`objects` 列表就是把它们按 node 展开后的实例。

### 4.2 `GltfLoader` API 变更（破坏性但受控）

```cpp
class GltfLoader {
  public:
    struct Options {
        bool generateMissingNormals = true;
        bool flipV                   = false;   // glTF 默认 UV 左上原点，与 Vulkan 一致；预留
        std::shared_ptr<Texture> fallbackWhite; // 可选：跨场景复用 1x1 白贴图
    };

    static GltfAsset load(const std::string& path,
                          Device&            device,
                          FrameSync&         frameSync,
                          Renderer&          renderer,          // ← 新增，用于 Material
                          const PipelineConfig& baseConfig,     // ← 新增
                          const Options&     opts = {});
};
```
旧的 `vector<unique_ptr<Mesh>>` 重载在 v1 中**删除**（只有 `BuiltinScenes.cpp` 一处调用点）。

### 4.3 `Vertex` 扩展

| location | 现有 | v1 目标 | 说明 |
| --- | --- | --- | --- |
| 0 | vec3 pos | vec3 pos | 不变 |
| 1 | vec3 color | vec3 normal | **语义更换**；OBJ / 旧数据生成时若缺失，Loader 负责填默认 `(0,1,0)` |
| 2 | vec2 texCoord | vec2 texCoord | 不变 |

**向后兼容策略**：保留 `color` 字段不再有意义，故直接复用 location 1 改成 normal，shader 同步更新。`Mesh::fromOBJ` 填 `(0,1,0)` 作为占位（tinyobj 也可以读 normal，顺手补）。

> 若希望零破坏，可新增 location 3 存 normal、保留 color。代价：per-vertex +12B。本方案选"替换 color"，因为 color 在项目中没有任何消费方，保留只是历史遗留。

### 4.4 `Material` 扩展

新增构造形式：
```cpp
struct MaterialParams {
    std::shared_ptr<Texture> baseColor;     // 必填；缺失由 Loader 填 1x1 白
    glm::vec4                baseColorFactor{1.0f};
    float                    metallicFactor  = 1.0f;
    float                    roughnessFactor = 1.0f;
    glm::vec3                emissiveFactor{0.0f};
    bool                     doubleSided     = false;
    // alphaMode 预留：enum { Opaque, Mask, Blend }; float alphaCutoff;
};

Material(Device&, Renderer&, const MaterialParams&, const PipelineConfig&);
```
内部 UBO 布局（新增 binding=2，per-frame 动态 UBO 之外再挂一个 per-material 静态 UBO）：
```glsl
layout(set=0, binding=2) uniform MaterialUBO {
    vec4  baseColorFactor;
    vec4  emissiveFactor;   // .xyz = emissive, .w = metallic
    vec4  miscFactor;       // .x = roughness, .y = alphaCutoff, .zw reserved
} mat;
```
> 也可塞进 push constant（当前 push constant 已占 64B = mat4 model，还剩 ≥64B，完全够放 factors）。v1 **优选 push constant**，省掉一套 descriptor，成本最低：

```glsl
layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissiveMetallic; // xyz=emissive, w=metallic
    vec4 roughnessAlpha;   // x=roughness, y=alphaCutoff
} push;
```
对应 C++ 端 push 结构体：`sizeof == 128`，小于 VK 规范最低保证 128 字节。✅ 

因此 `Material` 不需要新增 UBO，仅把 factors 存进自身、绘制时通过 `SceneObject` → `Pipeline` 拼 push 数据即可。这需要修改 `Scene::render` 的 push 组装逻辑（见 §6.3）。

### 4.5 `Texture` 扩展

新增构造：
```cpp
struct TextureCreateInfo {
    const void*  pixels;     // 已解码 RGBA8
    uint32_t     width, height;
    bool         generateMipmaps = true;
    VkFilter     minFilter       = VK_FILTER_LINEAR;
    VkFilter     magFilter       = VK_FILTER_LINEAR;
    VkSamplerAddressMode wrapU   = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode wrapV   = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkFormat     format          = VK_FORMAT_R8G8B8A8_SRGB; // baseColor 用 sRGB；normal/metalRough 用 UNORM
};
Texture(Device&, FrameSync&, const TextureCreateInfo&);
```
现有 `Texture(Device&, FrameSync&, const std::string&)` 保留，内部改写为：先 `stbi_load` → 再调上面的 `TextureCreateInfo` 构造路径，避免两套加载代码。

---

## 5. Loader 算法详细步骤

### 5.1 解析阶段

```cpp
opts.images_as_is = 0;    // ← 关键：让 tg3 替我们解码 PNG/JPG
```
若想走自定义 IO，可用 `tg3_image_callbacks`；v1 用内置解码足够。

### 5.2 图像 → Texture

```cpp
for (uint32_t i = 0; i < m->images_count; ++i) {
    const tg3_image& img = m->images[i];
    // img.image 此时是解码后的 RGBA8 或 RGB8（按 img.component 判断）
    //   若是 RGB8，复制到临时 buffer 扩成 RGBA8 再传。
    textures_img[i] = make_shared<Texture>(..., TextureCreateInfo{...});
}
```
若缺图（比如 KTX2 unsupported）→ 用 `opts.fallbackWhite`。

### 5.3 Sampler → 合并进 Texture

glTF 的 sampler 是可分离的（多 texture 可共享 sampler）。本项目 `Texture` 里 sampler 内嵌，因此：
```cpp
for (uint32_t t = 0; t < m->textures_count; ++t) {
    int srcImg = m->textures[t].source;
    int smp    = m->textures[t].sampler;
    // 重新构造一个 Texture，共享 pixels 但 sampler 可能不同
}
```
**v1 简化**：一个 `tg3_texture` → 一个 `vkr::Texture`（不去重 image），采样器参数从 `m->samplers[smp]` 翻译而来。`wrap_s → addressModeU`、`min_filter` → `VK_FILTER_*` + mipmap mode 按 OpenGL 常量表翻译。

### 5.4 Material → vkr::Material

```cpp
for (uint32_t i = 0; i < m->materials_count; ++i) {
    const tg3_material& gm = m->materials[i];
    MaterialParams p;
    int bcIdx = gm.pbr_metallic_roughness.base_color_texture.index;
    p.baseColor = (bcIdx >= 0 ? textures[bcIdx] : fallbackWhite);
    p.baseColorFactor = glm::make_vec4(gm.pbr_metallic_roughness.base_color_factor);
    p.metallicFactor  = (float)gm.pbr_metallic_roughness.metallic_factor;
    p.roughnessFactor = (float)gm.pbr_metallic_roughness.roughness_factor;
    p.emissiveFactor  = glm::make_vec3(gm.emissive_factor);
    p.doubleSided     = gm.double_sided != 0;
    materials[i] = make_shared<Material>(device, renderer, p, baseConfig);
}
// 再造一个 fallback material 给 primitive.material == -1 使用。
```

### 5.5 Primitive → Mesh

与现状相同，但额外：
- 读 `NORMAL` 属性；若缺失且 `opts.generateMissingNormals`，按面法线求平均生成。
- 记录该 primitive 的 `material` 索引。
- （v2 预留）读 `TANGENT` + `TEXCOORD_1`。

### 5.6 Node → SceneObject 世界矩阵

```cpp
function<void(int, const mat4&)> walk = [&](int nodeIdx, const mat4& parent) {
    const tg3_node& n = m->nodes[nodeIdx];
    mat4 local = n.has_matrix
        ? make_mat4(n.matrix)
        : translate(mat4(1), make_vec3(n.translation))
        * mat4_cast(quat(n.rotation[3], n.rotation[0], n.rotation[1], n.rotation[2]))
        * scale(mat4(1), make_vec3(n.scale));
    mat4 world = parent * local;
    if (n.mesh >= 0) {
        for (auto& prim : primitivesOfMesh[n.mesh]) {
            asset.objects.push_back({ prim.mesh, materials[prim.materialIdx], world });
        }
    }
    for (uint32_t c = 0; c < n.children_count; ++c) walk(n.children[c], world);
};
// 从 model.scenes[model.scene].nodes 开始走；fallback 走 model.scenes[0]。
```

### 5.7 推荐相机（可选）

遍历 objects 聚合 AABB → 中心 + 半径 → `position = center + normalize(1,1,1) * radius * 2.5`。若不想耗时，v1 可省略，场景工厂自己设 `initialCamera`。

---

## 6. 与现有模块的集成改动

### 6.1 `BuiltinScenes.cpp` 的 `sheenChairSceneFactory` 改写

```cpp
SceneFactory sheenChairSceneFactory(std::string vp, std::string fp) {
    return [vp, fp](Device& device, FrameSync& fs, Renderer& r)
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
注意签名去掉了 `tex` 参数 → `main.cpp` 同步更新。

### 6.2 `Scene::render` 的 push constant 组装

当前（推测）只 push `mat4 model`。改为 push 一个 128B 结构：
```cpp
struct PushBlock {
    glm::mat4 model;
    glm::vec4 baseColorFactor;
    glm::vec4 emissiveMetallic;
    glm::vec4 roughnessAlpha;
};
```
Scene 遍历 object 时，从 `object.material->params()` 读 factors 填进去。`Material` 需要 `const MaterialParams& params() const` 访问器。

### 6.3 shader 更新

`shader.vert`：把 `inColor` 换成 `inNormal`，传 `fragNormalWS`。  
`shader.frag`：
```glsl
vec4 tex = texture(texSampler, fragTexCoord);
vec3 albedo = tex.rgb * push.baseColorFactor.rgb;
float alpha = tex.a * push.baseColorFactor.a;
// v1: 用法线做个软光照，避免"贴图硬套"
float ndl = max(dot(normalize(fragNormalWS), normalize(vec3(0.3,0.8,0.5))), 0.0);
vec3 color = albedo * (0.2 + 0.8 * ndl) + push.emissiveMetallic.rgb;
outColor = vec4(color, alpha);
```
> 这只是"让模型看起来正确"的临时着色，不是 PBR。PBR 放到 `doc/gltf/design-v2.md`。

---

## 7. 实施计划（分阶段，建议按 PR 粒度落地）

| 阶段 | 内容 | 产物 | 验证 |
| --- | --- | --- | --- |
| **P1** | `Texture` 增加 `TextureCreateInfo` 构造；`Vertex` 用 normal 替换 color；shader 同步；`Mesh::fromOBJ` 填默认 normal | viking_room 场景不崩，外观可接受 | 运行 Viking Room 场景，画面与旧版一致 / 仅光照差异 |
| **P2** | 新增 `GltfAsset` + 改版 `GltfLoader`（images / materials / primitive-material），node transform 暂用单位阵；`Material` 加 `MaterialParams` + push-constant factors；`Scene::render` push 扩展 | SheenChair 显示自己的贴图，但摆放可能歪 | 检查 SheenChair 的椅子贴图正确（木纹+布纹），控制台无 validation error |
| **P3** | node 层级 + TRS → SceneObject.world matrix | SheenChair 正确摆放 | 目视 |
| **P4**（可选） | AABB 推荐相机 / alphaMode=MASK 支持 / double-sided pipeline 变体 | 更多模型适配 | 切换多个 sample glb |
| **v2**（未来） | 法线贴图 / PBR 直接光照 / KTX2 / animation | 另立设计文档 | — |

**回滚策略**：每个阶段独立 commit；P1 不触碰 Loader，风险最低；P2 失败可直接 revert Loader 与 BuiltinScenes，回到"Viking 可用、Sheen 摆烂"的当前状态。

---

## 8. 风险与决策备忘

1. **替换 `color` → `normal`**：如果 shader 后续需要顶点色（比如 glTF 的 `COLOR_0`），需要再开一个 location。本次接受。
2. **push constant 128B**：规范保证最小 128B，大多数桌面 GPU 实际 256B。若未来要加更多材质参数，必须转 UBO。
3. **sampler 参数翻译**：OpenGL 的 `LINEAR_MIPMAP_LINEAR` 需要映射到 VK 的 `(minFilter=LINEAR, mipmapMode=LINEAR)`；实现时列一张表避免错。
4. **非 RGBA8 图像**：tg3 可能返回 RGB8 / grayscale；Loader 内部统一扩成 RGBA8 再传，避免 Texture 支持多格式的复杂度。
5. **共享 image / 不同 sampler**：v1 按 `tg3_texture` 粒度重新上传 image。只有在同一张图被多 sampler 引用时有冗余；SheenChair 这类样例不存在此情况。若后续要优化，可以把 `Texture` 拆成 `Image2D` + `Sampler` 两个对象。
6. **与 step7 的关系**：本重构只碰 Loader / Vertex / Material / Texture / BuiltinScenes / shader，**不影响** step7 的 GUI 场景切换 / RMB 相机 逻辑。建议顺序：先完成 step7，再做本文档的 P1→P3。

---

## 9. TODO 清单（可直接转任务）

- [ ] P1-a: `Texture::TextureCreateInfo` 构造 + 旧 path 构造复用新路径
- [ ] P1-b: `Vertex`：color → normal；`Mesh::fromOBJ` 填默认法线
- [ ] P1-c: `shader.vert/frag` 用 normal 做软光照
- [ ] P1-d: 回归测试 Viking Room
- [ ] P2-a: `GltfAsset` 结构 + `GltfLoader::load` 新签名
- [ ] P2-b: images_as_is=0 + image → Texture
- [ ] P2-c: materials → `Material` (push-constant factors)
- [ ] P2-d: primitive.material 索引落到 `SceneObject.material`
- [ ] P2-e: `BuiltinScenes::sheenChairSceneFactory` 简化；`main.cpp` 去掉 texturePath 入参
- [ ] P3-a: node 递归 + TRS / matrix → world transform
- [ ] P3-b: 默认 scene / 多根节点处理
- [ ] P4-a: AABB 推荐相机
- [ ] P4-b: alphaMode / doubleSided pipeline 变体（扩 `PipelineConfig`）
