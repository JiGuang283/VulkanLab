# Step 3 具体执行计划

> 上游：[implementation_steps.md#step-3](./implementation_steps.md) · 前置 commit：Step 2 (`3e47745`)
> 产出：单个 commit。让 `Material` 持有 PBR 因子（`MaterialParams`），`Scene::render` push 一个 128B 结构，shader 消费 factors。
> 关键约束：保留 **旧** `Material(Device&, Renderer&, const Texture&, const PipelineConfig&)` 重载，**新增**带 `MaterialParams` 的重载，避免动调用点。

---

## 3.1 影响范围盘点

| 文件 | 改动 |
| --- | --- |
| `src/render/Material.h`（旧路径：`../../../../src/render/Material.h`） | 新增 `MaterialParams` 结构、新构造重载、`params()` 访问器、私有字段 `params_` |
| `src/render/Material.cpp`（旧路径：`../../../../src/render/Material.cpp`） | 新构造实现：从 `params_.baseColor` 拿 `Texture`；旧构造内填 `params_` 默认值 |
| [src/scene/Scene.cpp](../../../../src/scene/Scene.cpp) | `GpuPushBlock` 128B；`vkCmdPushConstants` size 改 128，stageFlags = VS\|FS |
| [src/scene/BuiltinScenes.cpp](../../../../src/scene/BuiltinScenes.cpp) | `makeStandardConfig` 中 push range size 128 + stageFlags VS\|FS |
| `shader/shader.vert`（旧路径：`../../../../shader/shader.vert`） | push block 扩到 128B（仅声明，使用仅 model） |
| `shader/shader.frag`（旧路径：`../../../../shader/shader.frag`） | push block 同上；消费 `baseColorFactor`、`emissiveFactor`、`alphaCutoff` |

**不动**：调用点（`vikingRoomSceneFactory` / `sheenChairSceneFactory`）继续用旧 `Material(...)` 重载；视觉与 Step 2 完全一致（factors 默认 = 1，emissive = 0）。

---

## 3.2 设计要点

### 3.2.1 `MaterialParams`

```cpp
struct MaterialParams {
    std::shared_ptr<Texture> baseColor;             // 必填（旧重载下置空 → 描述符仍由旧 ctor 路径手工绑）
    glm::vec4                baseColorFactor{1.0f};
    glm::vec3                emissiveFactor{0.0f};
    float                    metallicFactor  = 1.0f;
    float                    roughnessFactor = 1.0f;
    float                    alphaCutoff     = 0.5f;
    bool                     doubleSided     = false;
};
```

### 3.2.2 两个构造的关系

- **旧** `Material(Device&, Renderer&, const Texture&, const PipelineConfig&)`：保留原行为；构造体内额外把 `params_` 填默认 factors，`params_.baseColor` **置空**（因为旧 API 用的是 `const Texture&` 引用，不持有 shared_ptr）。
- **新** `Material(Device&, Renderer&, MaterialParams, const PipelineConfig&)`：要求 `params.baseColor` 非空，内部以 `*params.baseColor` 调用现有的 `createDescriptorSets`；其他逻辑（layout/pool）完全复用。

> 二者描述符集结构相同（binding 0 UBO + binding 1 sampler），所以 layout/pool 创建无需分支。

### 3.2.3 `GpuPushBlock` 内存布局（与 [implementation_steps.md 附录 B](./implementation_steps.md#附录-b--push-constant-内存布局) 一致）

```cpp
struct GpuPushBlock {
    glm::mat4 model;            // 0,  64
    glm::vec4 baseColorFactor;  // 64, 16
    glm::vec4 emissiveMetallic; // 80, 16  (xyz=emissive, w=metallic)
    glm::vec4 roughnessAlpha;   // 96, 16  (x=roughness, y=alphaCutoff)
};
static_assert(sizeof(GpuPushBlock) == 128);
```

### 3.2.4 旧 ctor 兼容（关键决策）

`Scene::render` 拿不到 `Texture`，只能从 `obj.material->params()` 取 factors。当材质是用旧 ctor 构造时，`params_.baseColor` 为空，但 **factors 全是默认值 1**，所以 push 结果还是 `baseColorFactor = (1,1,1,1)`，shader 消费后视觉等同于"只用纹理本色"。**视觉不变**。

---

## 3.3 分阶段编辑清单

### 3.3.1 `src/render/Material.h`（旧路径：`../../../../src/render/Material.h`）

- 在 `class Material` 之前新增 `struct MaterialParams { ... };`
- 在原构造声明下方添加：
  ```cpp
  Material(Device &device, Renderer &renderer, MaterialParams params,
           const PipelineConfig &config);
  const MaterialParams &params() const { return params_; }
  ```
- 私有字段：`MaterialParams params_;`
- 头部需要 `#include <glm/glm.hpp>` 和 `#include <memory>`（`shared_ptr<Texture>`）。

### 3.3.2 `src/render/Material.cpp`（旧路径：`../../../../src/render/Material.cpp`）

旧构造体内最后追加一行：
```cpp
params_ = MaterialParams{};   // baseColor 为空，factors 走默认值
```
新增构造（贴在旧构造下方）：
```cpp
Material::Material(Device &device, Renderer &renderer, MaterialParams params,
                   const PipelineConfig &config)
    : device_(&device), renderer_(&renderer), config_(config),
      params_(std::move(params)) {
    assert(params_.baseColor && "MaterialParams.baseColor must not be null");
    createDescriptorSetLayout();
    config_.descriptorLayouts.push_back(descriptorSetLayout_);
    createDescriptorPool();
    createDescriptorSets(*params_.baseColor);
}
```
头：`#include <cassert>`。

### 3.3.3 [src/scene/Scene.cpp](../../../../src/scene/Scene.cpp)

完整 render 体替换为：
```cpp
struct GpuPushBlock {
    glm::mat4 model;
    glm::vec4 baseColorFactor;
    glm::vec4 emissiveMetallic;
    glm::vec4 roughnessAlpha;
};
static_assert(sizeof(GpuPushBlock) == 128, "push block must be 128B");

void Scene::render(VkCommandBuffer cmd, uint32_t frameIndex,
                   Pipeline &pipeline) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    for (const auto &obj : objects_) {
        obj.material->bindDescriptors(cmd, pipeline.layout(), frameIndex);

        const auto &p = obj.material->params();
        GpuPushBlock blk{};
        blk.model            = obj.transform;
        blk.baseColorFactor  = p.baseColorFactor;
        blk.emissiveMetallic = glm::vec4(p.emissiveFactor, p.metallicFactor);
        blk.roughnessAlpha   = glm::vec4(p.roughnessFactor, p.alphaCutoff,
                                         0.0f, 0.0f);
        vkCmdPushConstants(cmd, pipeline.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(GpuPushBlock), &blk);

        obj.mesh->bind(cmd);
        obj.mesh->draw(cmd);
    }
}
```

### 3.3.4 [src/scene/BuiltinScenes.cpp](../../../../src/scene/BuiltinScenes.cpp)

`makeStandardConfig`：
```cpp
cfg.pushConstants = {{
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128
}};
```

### 3.3.5 `shader/shader.vert`（旧路径：`../../../../shader/shader.vert`）

push block 扩容（其余不变）：
```glsl
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissiveMetallic;
    vec4 roughnessAlpha;
} push;
```

### 3.3.6 `shader/shader.frag`（旧路径：`../../../../shader/shader.frag`）

```glsl
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColorFactor;
    vec4 emissiveMetallic;   // xyz=emissive, w=metallic
    vec4 roughnessAlpha;     // x=roughness, y=alphaCutoff
} push;

void main()
{
    vec4 tex = texture(texSampler, fragTexCoord);
    vec4 albedo = tex * push.baseColorFactor;
    if (albedo.a < push.roughnessAlpha.y) discard;

    vec3 n = normalize(fragNormalWS);
    vec3 L = normalize(vec3(0.3, 0.8, 0.5));
    float ndl = max(dot(n, L), 0.0);
    vec3 lit = albedo.rgb * (0.25 + 0.75 * ndl) + push.emissiveMetallic.rgb;
    outColor = vec4(lit, albedo.a);
}
```

> 注意：`alphaCutoff` 在 `MaterialParams` 默认 = 0.5；旧 ctor 路径下贴图 alpha 通常是 1（viking_room 的 jpg/png 全不透明），不会触发 discard。glTF 路径未来 Step 5 会按需置 `alphaCutoff`。

### 3.3.7 重新编译 shader

```powershell
cd C:\Project\vulkan_learn\shader; .\compile.bat
```
（脚本最后的 `pause` 可忽略，或直接两条 glslc 命令手动跑）

---

## 3.4 验证

### 3.4.1 构建
```powershell
cmake --build build-debug --config Debug   2>&1 | Select-String "error " | Select-Object -First 30
cmake --build build       --config Release 2>&1 | Select-String "error " | Select-Object -First 30
```
两者 0 error。

### 3.4.2 运行（视觉判定）
- Viking Room：与 Step 2 视觉一致（factors=1, emissive=0），软光照明暗对比保留。
- SheenChair：与 Step 2 视觉一致（仍贴 viking_room、仍偏暗）。
- 控制台无 VUID 报错。

### 3.4.3 Push constant range 校验
- 启动时若 push range size 与 shader block 不匹配，validation layer 会报 `VUID-vkCmdPushConstants-offset-01795` 等。本步保证 128/VS|FS 一致 → 应静默。

### 3.4.4 故障定位

| 现象 | 原因 | 处置 |
| --- | --- | --- |
| validation `pushConstantRange ... not declared in layout` | shader 与 BuiltinScenes 的 stageFlags / size 不一致 | 检查 `makeStandardConfig` 与 shader push block |
| frag 无变化 | spv 未刷新 | 重新跑 `compile.bat`；CMake 会拷贝到 `build*/<Cfg>/shader/` |
| static_assert 失败 | glm 对齐问题（极少） | 用 `alignas(16)` 强制对齐 vec4，或拆 `vec4` 分量手动塞 |
| Black SheenChair | 与 Step 2 同因（假法线 +Y） | 不在本步范围 |

---

## 3.5 回滚
```powershell
git reset --hard HEAD     # 提交前
git revert HEAD           # 提交后
```
回滚不影响 Step 1/2。

---

## 3.6 Commit
```powershell
git add src/render/Material.h src/render/Material.cpp `
        src/scene/Scene.cpp src/scene/BuiltinScenes.cpp `
        shader/shader.vert shader/shader.frag `
        shader/vert.spv shader/frag.spv `
        doc/gltf/step3.md
git commit -m "feat(material): add MaterialParams + 128B push block

- New Material ctor taking MaterialParams (baseColor + PBR factors).
- Old ctor preserved; params_ filled with defaults for compat.
- Scene::render pushes 128B GpuPushBlock (model + factors) to VS|FS.
- BuiltinScenes uses VS|FS stage flags and 128B push range.
- Shaders consume baseColorFactor / emissiveFactor / alphaCutoff.
- Existing scenes are visually unchanged (factors default to 1)."
```

---

## 3.7 完成后状态
- `Material` 双 ctor 共存；旧场景工厂代码 0 改动。
- Push constant 128B / `VS|FS`，已就绪给 Step 5 真正塞 glTF factors。
- 视觉无回归；仍是 Step 2 看到的画面。
- 为 Step 4 (`GltfAsset`) / Step 5 (Loader emit MaterialParams) 解耦完毕。
