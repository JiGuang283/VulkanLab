# Step 4 具体执行计划

> 上游：[implementation_steps.md#step-4](./implementation_steps.md) · 前置 commit：Step 3 (`1fa1738`)
> 产出：单个 commit，**仅新增**一个头文件 [src/render/GltfAsset.h](../../src/render/GltfAsset.h)。无消费者，零行为变化。
> 关键约束：纯头文件、不动 CMakeLists（src/* 通配收集；如非通配需要补 `add_*`）；编译通过即视为完成。

---

## 4.1 影响范围

| 文件 | 动作 |
| --- | --- |
| 新建 [src/render/GltfAsset.h](../../src/render/GltfAsset.h) | 定义 `struct GltfAsset` |
| [CMakeLists.txt](../../CMakeLists.txt) | **检查**是否用通配（`file(GLOB ...)`）；若是则无需改；否则补 `${CMAKE_SOURCE_DIR}/src/render/GltfAsset.h` |

**不改**：任何 .cpp / 任何调用点 / 任何 shader / 任何场景工厂。

---

## 4.2 文件内容

参照 [implementation_steps.md Step 4](./implementation_steps.md#step-4--新增-gltfasset-数据包) 与 [SceneObject.h](../../src/scene/SceneObject.h)（已确认 `SceneObject` 字段：`mesh / material / transform`）和 [Scene.h](../../src/scene/Scene.h)（`CameraPose` 在此声明）。

完整内容：

```cpp
#pragma once

#include "scene/Scene.h"        // CameraPose
#include "scene/SceneObject.h"  // SceneObject

#include <memory>
#include <optional>
#include <vector>

namespace vkr {

class Texture;
class Material;
class Mesh;

/// glTF 一次加载的全部产出：贴图、材质、网格、已展开的实例（带 world matrix）
/// 以及（可选）作者建议的相机位姿。
/// 由 GltfLoader 产生（见 Step 5），由场景工厂消费（见 Step 7）。
struct GltfAsset {
    std::vector<std::shared_ptr<Texture>>  textures;
    std::vector<std::shared_ptr<Material>> materials;
    std::vector<std::shared_ptr<Mesh>>     meshes;   // 一个 primitive → 一个 Mesh
    std::vector<SceneObject>               objects;  // 节点展开后的实例
    std::optional<CameraPose>              suggestedCamera;
};

} // namespace vkr
```

注意点：
- 使用前向声明 `Texture / Material / Mesh`，**避免**头文件爆炸引入 Vulkan 头链。`shared_ptr<T>` 仅需 T 的不完整类型即可。
- `SceneObject` 字段含 `shared_ptr<Mesh>` / `shared_ptr<Material>`，故必须 include `SceneObject.h`（不能仅前向声明）。同理 `optional<CameraPose>` 需要完整定义，故 include `Scene.h`。
- 命名空间 `vkr` 与现有代码一致。

---

## 4.3 CMakeLists 检查

```powershell
Select-String -Path C:\Project\vulkan_learn\CMakeLists.txt -Pattern "GLOB|GltfAsset|render/.*\.h" | Select-Object -First 20
```
- 若看到 `file(GLOB ... src/render/*.h)` → **无需改 CMake**。
- 否则在 source list 内追加：
  ```cmake
  ${CMAKE_SOURCE_DIR}/src/render/GltfAsset.h
  ```
  （和其它 render 头文件并列即可。本项目 .h 是否纳入 target 不影响编译，但纳入可让 IDE 列出该文件。）

---

## 4.4 验证

### 4.4.1 编译
```powershell
cmake --build build-debug --config Debug   2>&1 | Select-String "error " | Select-Object -First 30
cmake --build build       --config Release 2>&1 | Select-String "error " | Select-Object -First 30
```
两者 0 error；本步骤本就不会触发任何 .cpp 重编译，应秒过。

### 4.4.2 运行
- 跑一次 `VulkanLab.exe` 确认与 Step 3 视觉一致 + 无 VUID（防御性回归测试）。

### 4.4.3 包含图自检（可选）
```powershell
& 'C:\VulkanSDK\1.4.335.0\Bin\glslc.exe' --version  # 与本步无关，仅占位
# 若要单独测头文件能 include：
echo "#include `"render/GltfAsset.h`"" > test_inc.cpp; cl /I src /I external /c test_inc.cpp
```
（不建议跑这步，CMake 已能验证。）

---

## 4.5 故障定位

| 现象 | 原因 | 处置 |
| --- | --- | --- |
| `incomplete type 'vkr::Texture'` | 误用了 `unique_ptr` 或显式析构 | 保持 `shared_ptr<T>` 前向声明即可 |
| `Scene.h` 循环依赖 | Scene.h 反过来 include GltfAsset.h | 不要这么做；GltfAsset 只单向依赖 Scene.h |
| CMake configure 重新触发 | 改了 CMakeLists | 正常，1 次 reconfigure 不影响其他目标 |

---

## 4.6 回滚
```powershell
git reset --hard HEAD     # 提交前
git revert HEAD           # 提交后
```

---

## 4.7 Commit
```powershell
git add src/render/GltfAsset.h doc/gltf/step4.md
# 若改了 CMakeLists：
# git add CMakeLists.txt
git commit -m "feat(gltf): add GltfAsset header (no consumers yet)

- Pure type definition for the future GltfLoader output.
- Forward-declares Texture/Material/Mesh; includes Scene/SceneObject only.
- No behavioral change; sets up Step 5/6 wiring."
```

---

## 4.8 完成后状态
- 多了一个未被任何 .cpp include 的纯类型头。
- 编译产物字节级等于 Step 3。
- 为 Step 5 的 `GltfLoader::load(...) -> GltfAsset` 重写就绪。
