# Step 7 具体执行计划

> 上游：[implementation_steps.md#step-7](./implementation_steps.md) · 前置 commit：Step 6 (`79ffc24`)
> 产出：单个 commit，去掉 `sheenChairSceneFactory` 残留的 `tex` 形参、同步 [src/main.cpp](../../../../src/main.cpp)，完成全部 7 步收尾。
> 非目标：本步不改 Loader / Material / Texture / shader / GltfAsset。

---

## 7.1 影响范围

| 文件 | 动作 |
| --- | --- |
| [src/scene/BuiltinScenes.h](../../../../src/scene/BuiltinScenes.h) | `sheenChairSceneFactory` 签名从 `(tex, vp, fp)` 改为 `(vp, fp)` |
| [src/scene/BuiltinScenes.cpp](../../../../src/scene/BuiltinScenes.cpp) | 去掉 `(void)tex;` 与 `tex` 形参；`vikingRoomSceneFactory` 不动 |
| [src/main.cpp](../../../../src/main.cpp) | 注册 Sheen Chair 的调用改成两参数 |

**不改**：`vikingRoomSceneFactory`（仍然吃 `config.texturePath`）、`Config` 本体、`Application`、Loader、shader。

---

## 7.2 设计要点

### 7.2.1 签名演进

- **旧**（Step 5/6 过渡版）：`sheenChairSceneFactory(std::string tex, std::string vp, std::string fp)` — `tex` 在实现里 `(void)tex;` 忽略。
- **新**（最终版）：`sheenChairSceneFactory(std::string vp, std::string fp)` — 内部贴图全部由 `GltfLoader::load` 从 glb 产出。

### 7.2.2 相机

保留 [src/scene/BuiltinScenes.cpp](../../../../src/scene/BuiltinScenes.cpp) 里现有的 `asset.suggestedCamera.value_or(...)` 逻辑。当前 Loader 不填 `suggestedCamera`，实际仍走 fallback `CameraPose{{1.5,1.5,1.0}, -135, -20}` — 与 Step 5/6 观测一致，Step 7 不改动。

### 7.2.3 `Config::texturePath` 去留

- Viking Room 仍然消费它（OBJ 走外部 PNG），**保留**。
- main.cpp 里只在 `vikingRoomSceneFactory(...)` 调用处引用；`sheenChairSceneFactory(...)` 不再引用。

---

## 7.3 代码骨架

### 7.3.1 [src/scene/BuiltinScenes.h](../../../../src/scene/BuiltinScenes.h)

```cpp
/// Factory for `models/SheenChair.glb` (glTF).  Static (no animation); all
/// textures/materials come from the glb itself.
SceneFactory sheenChairSceneFactory(std::string vertShaderPath,
                                    std::string fragShaderPath);
```

`vikingRoomSceneFactory` 签名不动。

### 7.3.2 [src/scene/BuiltinScenes.cpp](../../../../src/scene/BuiltinScenes.cpp)

```cpp
SceneFactory sheenChairSceneFactory(std::string vp, std::string fp) {
    return [vp = std::move(vp),
            fp = std::move(fp)](Device &device, FrameSync &frameSync,
                                Renderer &renderer) -> std::unique_ptr<Scene> {
        auto scene = std::make_unique<Scene>();
        auto baseCfg = makeStandardConfig(device, vp, fp);
        auto asset = GltfLoader::load("models/SheenChair.glb", device,
                                      frameSync, renderer, baseCfg);

        for (auto &t : asset.textures)
            scene->addTexture(t);
        for (auto &m : asset.materials)
            scene->addMaterial(m);
        for (auto &mesh : asset.meshes)
            scene->addMesh(mesh);
        for (auto &o : asset.objects)
            scene->addObject(o);

        scene->initialCamera = asset.suggestedCamera.value_or(
            CameraPose{{1.5f, 1.5f, 1.0f}, -135.0f, -20.0f});
        return scene;
    };
}
```

删除点：
- 函数签名里的 `std::string tex,`
- 函数体开头的 `(void)tex;`
- lambda capture 中已不捕获 `tex`，无需动作

### 7.3.3 [src/main.cpp](../../../../src/main.cpp)

```cpp
app.registerScene(
    {"Sheen Chair",
     vkr::sheenChairSceneFactory(config.vertShaderPath,
                                 config.fragShaderPath)});
```

（删除 `config.texturePath` 那个实参；Viking Room 行保持不变。）

---

## 7.4 验证步骤

1. **编译**：
   ```powershell
   cmake --build build-debug --config Debug
   cmake --build build       --config Release
   ```
   两者 0 error / 0 warning（第三方除外）。
2. **运行 Release**（`build\Release\VulkanLab.exe`）：
   - 默认 **Viking Room**：贴图正常、带软光照明暗。
   - 切到 **Sheen Chair**：贴图正确（木纹 + 布纹），各组件位置正确（Step 6 成果不回退）。
   - 反复切换 10 次以上：无 validation error、Task Manager 显存稳定。
3. **总验证清单**（对照 [implementation_steps.md §总验证清单](./implementation_steps.md#总验证清单完成全部-7-步后)）逐项确认。

---

## 7.5 回滚

本步仅改三个文件且互相依赖，回滚直接：
```powershell
git checkout HEAD -- src/scene/BuiltinScenes.h src/scene/BuiltinScenes.cpp src/main.cpp
```
回到 Step 6 的"三参数 + `(void)tex;`"版本，功能等价。

---

## 7.6 提交

```powershell
git add src/scene/BuiltinScenes.h src/scene/BuiltinScenes.cpp src/main.cpp doc/gltf/step7.md
git commit -m "feat(gltf): drop legacy texture param from sheenChairSceneFactory

- Textures now come from the glb itself via GltfLoader; external
  viking_room.png is no longer injected into the chair scene.
- Shrink sheenChairSceneFactory signature to (vp, fp) and update main.cpp
  to match. vikingRoomSceneFactory signature unchanged.
- Completes the P1-P3 glTF overhaul (steps 1-7)."
```

---

## 7.7 完成后状态

- 全部 7 步落盘；`feature/gltf-v1` 分支可合并回 main。
- 遗留工作（v2 规划，不在本分支）：
  - sampler 精确翻译（附录 A 的完整表）；目前统一 `LINEAR / REPEAT / sRGB`。
  - 正规 linear/sRGB 通道分离：normal/metallicRoughness 应走 `VK_FORMAT_R8G8B8A8_UNORM`。
  - glTF extensions（`KHR_materials_*`、`KHR_draco_*`）。
  - node 层面的动画、skin/joint。
