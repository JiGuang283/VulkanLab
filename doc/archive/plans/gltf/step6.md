# Step 6 具体执行计划

> 上游：[implementation_steps.md#step-6](./implementation_steps.md) · 前置 commit：Step 5 (`70586a5`)
> 产出：单个 commit，在 `GltfLoader::load` 内部用 **node 层级递归** 替换 Step 5 末尾的"identity transform per primitive"段。只动 [src/render/GltfLoader.cpp](../../../../src/render/GltfLoader.cpp) 一个文件。
> 视觉验收：SheenChair 各组件（椅面 / 椅腿 / 靠背）位置与朝向正确，不再散开。

---

## 6.1 影响范围

| 文件 | 动作 |
| --- | --- |
| [src/render/GltfLoader.cpp](../../../../src/render/GltfLoader.cpp) | 仅替换第 7 段（`// 7. v1: identity transform …` 到 `asset.meshes.empty()` 前）；新增 `#include <glm/gtc/...>` 与 `<functional>` |

**不改**：GltfLoader.h / GltfAsset.h / BuiltinScenes / main.cpp / Material / Texture / Mesh / shader。
> Step 7 才会把 `sheenChairSceneFactory(tex, vp, fp)` 的 `tex` 形参删掉，与 main.cpp 同步清理。

---

## 6.2 设计要点

### 6.2.1 tg3 节点数据契约（已验证）

- `tg3_model::default_scene` — int32；**可能为 -1**（未指定），`scenes_count` 本身也可能为 0（极少数 glb 只有 nodes/meshes 数组）。
- `tg3_node`：
  - 可选字段：`mesh`、`camera`、`children/children_count`，缺失用 `-1` / `0`。
  - 变换：`has_matrix == 1` → 读 `matrix[16]`（列主序 double）；否则按 **T · R · S** 合成：
    - `translation[3]` 默认 `{0,0,0}`
    - `rotation[4]` 默认 `{0,0,0,1}`，存储顺序是 **(x, y, z, w)**（glTF 规范 + tg3 `n->rotation[3]=1.0` 作为单位四元数印证）
    - `scale[3]` 默认 `{1,1,1}`
  - 即使缺省 TRS 也合法（tg3 会把默认值填好）。

### 6.2.2 变换合成约定

- 矩阵类型：`glm::mat4`（float）。
- 节点 local：
  - `has_matrix` → `glm::make_mat4(n.matrix)` 后逐元素 `static_cast<float>`（`glm::make_mat4` 模板按源类型推断，直接喂 `double*` 会给出 `glm::dmat4`，要显式转 `mat4`）。
  - 否则：`T * R * S`，其中 `R = glm::mat4_cast(glm::quat(w, x, y, z))`。⚠️ glm 构造参数顺序是 `(w, x, y, z)`，而 glTF 存的是 `(x, y, z, w)` → 映射 `glm::quat((float)r[3], (float)r[0], (float)r[1], (float)r[2])`。
- `world = parent * local`（列主序约定保持与项目 push-constant 的 `model` 矩阵一致）。

### 6.2.3 遍历策略

- 递归 lambda 捕获 `m`、`primsByMesh`、`asset`、`fallbackMatIdx`。参数：`(int nodeIdx, const glm::mat4& parentWorld)`。
- 根集合选择顺序：
  1. `default_scene >= 0 && default_scene < scenes_count` → 用 `scenes[default_scene].nodes`
  2. 否则 `scenes_count > 0` → 用 `scenes[0].nodes`
  3. 否则 fallback：对所有 `n.mesh >= 0` 的顶层节点按 identity 处理（极端场景；保留安全网使运行不崩）。
- 安全网：`nodeIdx` 越界 (`<0 || >=nodes_count`) 直接 `return`，避免非法 glb 递归到 `m->nodes[-1]`。
- 不需要显式 visited 集合 —— glTF 2.0 规范强制 node 图为**有根树**（§5.26），tg3 解析侧也不会制造环。

---

## 6.3 代码骨架

### 6.3.1 新增 include（文件顶部附近）

```cpp
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <functional>
```

### 6.3.2 新增文件级辅助（放在 `makeWhite1x1` 之后，`GltfLoader::load` 之前）

```cpp
static glm::mat4 nodeLocalMatrix(const tg3_node &n) {
    if (n.has_matrix) {
        // glTF matrix 是列主序 double[16]；glm::make_mat4<double*> 返回 dmat4
        return glm::mat4(glm::make_mat4(n.matrix));
    }
    glm::vec3 t{(float)n.translation[0], (float)n.translation[1],
                (float)n.translation[2]};
    // glTF 存储顺序 (x,y,z,w)；glm::quat 构造顺序 (w,x,y,z)
    glm::quat q{(float)n.rotation[3], (float)n.rotation[0],
                (float)n.rotation[1], (float)n.rotation[2]};
    glm::vec3 s{(float)n.scale[0], (float)n.scale[1], (float)n.scale[2]};
    return glm::translate(glm::mat4(1.0f), t) *
           glm::mat4_cast(q) *
           glm::scale(glm::mat4(1.0f), s);
}
```

### 6.3.3 替换 `GltfLoader::load` 第 7 段

删除当前 lines 277–286（identity transform 循环），替换为：

```cpp
    // 7. Walk the node hierarchy: local TRS/matrix -> world; emit SceneObjects.
    std::function<void(int, const glm::mat4 &)> walk =
        [&](int nodeIdx, const glm::mat4 &parent) {
            if (nodeIdx < 0 || nodeIdx >= (int)m->nodes_count)
                return;
            const tg3_node &n = m->nodes[nodeIdx];
            glm::mat4       world = parent * nodeLocalMatrix(n);

            if (n.mesh >= 0 && n.mesh < (int)primsByMesh.size()) {
                for (auto &pe : primsByMesh[n.mesh]) {
                    auto mat = (pe.materialIdx >= 0 &&
                                pe.materialIdx < (int)m->materials_count)
                                   ? asset.materials[pe.materialIdx]
                                   : asset.materials[fallbackMatIdx];
                    asset.objects.push_back({pe.mesh, mat, world});
                }
            }
            for (uint32_t c = 0; c < n.children_count; ++c)
                walk(n.children[c], world);
        };

    int sceneIdx = -1;
    if (m->default_scene >= 0 && m->default_scene < (int)m->scenes_count)
        sceneIdx = m->default_scene;
    else if (m->scenes_count > 0)
        sceneIdx = 0;

    if (sceneIdx >= 0) {
        const tg3_scene &scn = m->scenes[sceneIdx];
        for (uint32_t r = 0; r < scn.nodes_count; ++r)
            walk(scn.nodes[r], glm::mat4(1.0f));
    } else {
        // No scenes array — drop each mesh at identity (极端 fallback).
        for (auto &list : primsByMesh)
            for (auto &pe : list)
                asset.objects.push_back(
                    {pe.mesh, asset.materials[fallbackMatIdx], glm::mat4(1.0f)});
    }
```

（后续的 `if (asset.meshes.empty()) throw …; return asset;` 段保持不变。）

---

## 6.4 验证步骤

1. **编译**：
   ```powershell
   cmake --build build-debug --config Debug
   cmake --build build       --config Release
   ```
   两者 0 error、无新增 warning。
2. **运行 Release** (`build\Release\VulkanLab.exe`)：
   - 切到 **Sheen Chair** — 椅面、靠背、四腿组装正确，不再悬空 / 叠加在原点；贴图（木纹 + 布面）仍然正确（Step 5 成果不回退）。
   - 切到 **Viking Room** — 无变化（OBJ 路径不走 Loader）。
   - 反复切换 10+ 次：无 validation error、无 Task Manager 显存增长。
3. **（可选）**下载 `BoxTextured.glb` / `DamagedHelmet.glb` 到 `models/` 临时 hack 进 SheenChair 场景路径验证多根节点 / 纯 matrix 节点路径 —— 验证完立即回退。

**预期控制台**：无 `[GltfLoader]` 警告；每个 glb 只打印一次 `[Scene] switched to Sheen Chair`。

---

## 6.5 常见坑位

| 症状 | 可能原因 |
| --- | --- |
| 模型整体镜像翻转 | glm `mat4_cast(q)` 默认是标准 glTF 右手系；若传错 quat 顺序（漏了 `(w,x,y,z)` 映射）会出现旋转方向反 |
| 模型比原尺寸巨大或压扁 | `has_matrix` 分支忘了 `glm::mat4(...)` 外包，导致 `glm::dmat4` 隐式转换丢位 |
| 部分组件仍在原点 | 递归漏了 `children`，或 `n.mesh` 检查用了 `primsByMesh.size()` 但源数组被 size_t 截断（注意都用 `int` 比较） |
| SheenChair 完全消失 | `default_scene == -1` 且 `scenes_count == 0` 走了 fallback 分支但材质索引越界——检查 `asset.materials[fallbackMatIdx]` 非空 |
| validation error: push constant mat4 含 NaN | `scale` 默认值漏填（tg3 默认 1,1,1 已处理），或 `matrix` 读取按行主序导致错误 |

---

## 6.6 回滚

本步仅改 `GltfLoader.cpp` 一个文件。失败：
```powershell
git checkout HEAD -- src/render/GltfLoader.cpp
```
即可回到 Step 5 的 identity-transform 版本，其它模块不受影响。

---

## 6.7 提交

```powershell
git add src/render/GltfLoader.cpp doc/gltf/step6.md
git commit -m "feat(gltf): walk node hierarchy to compute world transforms

- Replace identity-transform primitive emission with recursive node walk
  starting from default_scene (fallback scenes[0], then no-scenes fallback).
- Compose local transforms from matrix[16] when has_matrix, otherwise
  T * R * S with correct glTF (x,y,z,w) -> glm::quat(w,x,y,z) mapping.
- SheenChair now assembles correctly; Viking Room unaffected."
```

---

## 6.8 后续

Step 6 完成后，SheenChair 视觉上应已"看起来对"。Step 7 纯接线：
- 删 `sheenChairSceneFactory` 的 `tex` 形参 + `(void)tex;` 行
- 改 [src/main.cpp](../../../../src/main.cpp) 调用签名
- 清理不再需要的 viking_room.png 注入

不涉及 Loader 再改动。
