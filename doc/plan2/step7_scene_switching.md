# 第七步：场景切换（Scene Switching）

## 目标

保留当前"加载单个模型"的行为，在此之上支持**运行时切换多个场景**。
最小可用功能：
- 启动时可通过 Config 指定默认场景
- 运行时按键（F1 / F2 / F3…）切换场景
- 切换时正确释放旧场景的 GPU 资源，无泄漏、无同步错误
- 每个场景可以有自己的模型、贴图、初始相机位置、每帧更新逻辑

不做的事情（避免过度设计）：
- ❌ 场景序列化（JSON/YAML 描述）
- ❌ 热重载（文件监听）
- ❌ 场景图层级结构
- ❌ 异步加载 / 加载画面

---

## 1. 当前架构分析

### 1.1 资源所有权现状

```cpp
class Application {
    // GPU 基础设施（与场景无关）
    unique_ptr<Window>        window_;
    unique_ptr<InputManager>  input_;
    unique_ptr<VulkanContext> context_;
    unique_ptr<Device>        device_;
    unique_ptr<SwapChain>     swapChain_;
    unique_ptr<FrameSync>     frameSync_;
    unique_ptr<Renderer>      renderer_;

    // ★ 场景相关资源（硬绑定到 Application）
    shared_ptr<Texture>              texture_;
    shared_ptr<Material>             material_;
    shared_ptr<Mesh>                 mesh_;         // OBJ
    vector<shared_ptr<Mesh>>         gltfMeshes_;   // glTF

    Scene  scene_;        // 值成员，装 SceneObject
    Camera camera_;
};
```

问题：
- **资源所有权分散**：mesh/texture/material 归 Application 持有，Scene 只拿 `shared_ptr`。切换场景时必须手动清理 Application 上这些字段。
- **硬编码动画**：`mainLoop()` 里写死 `scene_.objects()[0].transform = rotate(...)`，切换到没有"第一个物体"的场景会崩溃。
- **init() 职责膨胀**：既建基础设施又建场景内容，无法在运行时再跑一次。

### 1.2 与场景切换无关的部分

以下对象**创建一次即可，跨场景复用**，切换时不动：

| 对象 | 原因 |
|------|------|
| Window / InputManager | 窗口、输入设备不变 |
| VulkanContext / Device | Instance / PhysicalDevice / LogicalDevice 不变 |
| SwapChain | 只因 resize 重建，与场景无关 |
| FrameSync | 同步对象按帧管理，场景切换不影响 |
| Renderer | RenderPass / UBO / ColorImage / DepthImage 当前是通用的，还不需要 per-scene |
| Camera | 相机是观察者，切场景时只需**重置参数**，不用重新构造 |

---

## 2. 设计思路

### 2.1 核心决策

**把"场景相关"资源的所有权从 Application 移入 Scene 自身**——
Scene 不仅是 `vector<SceneObject>`，还拥有该场景所有的 Mesh / Texture / Material。
切换场景 = 销毁旧 Scene（自动释放其 GPU 资源）→ 构造新 Scene。

```cpp
class Scene {
    // 场景独占的 GPU 资源
    vector<shared_ptr<Texture>>  textures_;
    vector<shared_ptr<Material>> materials_;
    vector<shared_ptr<Mesh>>     meshes_;

    // 场景内容
    vector<SceneObject>          objects_;

    // 可选：场景自己的 update 逻辑、初始相机
    ...
};
```

**为什么用 shared_ptr 而不是 unique_ptr？**
同一 Mesh / Texture 可能被多个 SceneObject 引用（同场景内共享），`SceneObject` 已经持有 `shared_ptr<Mesh>` 和 `shared_ptr<Material>`。保持一致即可。

### 2.2 SceneFactory：函数而非类

场景构造逻辑用**一个函数**而非一个虚基类。理由（对齐 `architecture_review.md` 的"按需演进"原则）：
- 当前每个场景就是"加载一个模型 + 一个默认材质"，行为几乎没有状态差异
- 函数签名稳定：`unique_ptr<Scene> (Device&, FrameSync&, Renderer&)`
- 以后需要场景特有的 tick 逻辑时，再让 Scene 子类化即可

```cpp
// SceneFactory.h
using SceneFactory = std::function<
    std::unique_ptr<Scene>(Device&, FrameSync&, Renderer&)
>;

struct SceneEntry {
    std::string   name;     // "Viking Room" 显示用
    Key           hotKey;   // Key::F1 / F2 / F3 ...
    SceneFactory  factory;
};
```

### 2.3 Scene 增强：动画钩子 + 初始相机

为了消除 `mainLoop()` 里硬编码的旋转，给 Scene 加一个可选的 update 回调：

```cpp
class Scene {
  public:
    // ... 资源所有权字段 ...

    using UpdateFn = std::function<void(Scene&, float dt, float time)>;

    void setUpdateFn(UpdateFn fn) { updateFn_ = std::move(fn); }
    void update(float dt, float time) {
        if (updateFn_) updateFn_(*this, dt, time);
    }

    // 每个场景可以给出建议的初始相机位姿；
    // 若未设置则保留当前相机。
    std::optional<CameraPose> initialCamera;
};
```

`CameraPose` 是一个简单的 POD：
```cpp
struct CameraPose {
    glm::vec3 position{0.0f, 0.0f, 3.0f};
    float     yaw   = -90.0f;  // 度
    float     pitch = 0.0f;
};
```

### 2.4 切换流程

```
1. 用户按下 F2
2. Application 检测 hotKey → 得到目标 SceneEntry
3. vkDeviceWaitIdle(device_)      // 等 GPU 清空，任何 command buffer 完成
4. currentScene_.reset()          // 释放旧场景所有 Mesh/Texture/Material
5. currentScene_ = entry.factory(*device_, *frameSync_, *renderer_)
6. 应用 initialCamera（若有）
7. 继续 mainLoop 下一帧
```

**为什么 `vkDeviceWaitIdle` 是够用的**：
当前 Material 持有 DescriptorSet，这些 set 引用了 Texture 的 VkImageView 和 Buffer。
在前 `MAX_FRAMES_IN_FLIGHT` 帧可能还在 pending，因此必须等 idle 才能销毁。
`vkDeviceWaitIdle` 一定会 stall CPU，但场景切换是低频操作（用户按键），性能不敏感。

---

## 3. 改动清单

### A. 新文件

#### `src/scene/Scene.h/cpp` — 扩展

```cpp
// Scene.h
#pragma once
#include "SceneObject.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace vkr {

class Mesh;
class Material;
class Texture;

struct CameraPose {
    glm::vec3 position{0.0f, 0.0f, 3.0f};
    float     yaw   = -90.0f;
    float     pitch = 0.0f;
};

class Scene {
  public:
    using UpdateFn = std::function<void(Scene&, float dt, float time)>;

    // ---- 资源装填（由 factory 在构造期调用）----
    void addTexture(std::shared_ptr<Texture> t)   { textures_.push_back(std::move(t)); }
    void addMaterial(std::shared_ptr<Material> m) { materials_.push_back(std::move(m)); }
    void addMesh(std::shared_ptr<Mesh> m)         { meshes_.push_back(std::move(m)); }
    void addObject(SceneObject obj)               { objects_.push_back(std::move(obj)); }

    // ---- 渲染 ----
    void render(VkCommandBuffer cmd, uint32_t frameIndex) const;

    // ---- 每帧 tick（可选）----
    void setUpdateFn(UpdateFn fn) { updateFn_ = std::move(fn); }
    void update(float dt, float time) {
        if (updateFn_) updateFn_(*this, dt, time);
    }

    // ---- 访问器 ----
    std::vector<SceneObject>       &objects()       { return objects_; }
    const std::vector<SceneObject> &objects() const { return objects_; }

    std::optional<CameraPose> initialCamera;

  private:
    std::vector<std::shared_ptr<Texture>>  textures_;
    std::vector<std::shared_ptr<Material>> materials_;
    std::vector<std::shared_ptr<Mesh>>     meshes_;
    std::vector<SceneObject>               objects_;
    UpdateFn                               updateFn_;
};

} // namespace vkr
```

#### `src/scene/SceneFactory.h` — 工厂类型

```cpp
#pragma once
#include "window/InputManager.h"  // for Key
#include <functional>
#include <memory>
#include <string>

namespace vkr {

class Device;
class FrameSync;
class Renderer;
class Scene;

using SceneFactory = std::function<
    std::unique_ptr<Scene>(Device&, FrameSync&, Renderer&)
>;

struct SceneEntry {
    std::string  name;
    Key          hotKey;
    SceneFactory factory;
};

} // namespace vkr
```

#### `src/scene/BuiltinScenes.h/cpp` — 内置场景

集中放具体场景构造函数，避免污染 Application。

```cpp
// BuiltinScenes.h
#pragma once
#include "SceneFactory.h"

namespace vkr {

// 默认纹理 + 着色器（和 Config 保持一致的路径）
SceneFactory vikingRoomSceneFactory(std::string texturePath,
                                    std::string vertPath,
                                    std::string fragPath);

SceneFactory sheenChairSceneFactory(std::string texturePath,
                                    std::string vertPath,
                                    std::string fragPath);

} // namespace vkr
```

```cpp
// BuiltinScenes.cpp（示例片段）
std::unique_ptr<Scene> buildObjScene(Device& dev, FrameSync& fs, Renderer& r,
                                     const std::string& modelPath,
                                     const std::string& texturePath,
                                     const std::string& vertPath,
                                     const std::string& fragPath) {
    auto scene    = std::make_unique<Scene>();
    auto texture  = std::make_shared<Texture>(dev, fs, texturePath);
    auto material = std::make_shared<Material>(dev, r, *texture, vertPath, fragPath);
    auto mesh     = std::shared_ptr<Mesh>(Mesh::fromOBJ(dev, fs, modelPath).release());

    scene->addTexture(texture);
    scene->addMaterial(material);
    scene->addMesh(mesh);
    scene->addObject({mesh, material, glm::mat4(1.0f)});

    // 把原来硬编码的旋转搬到场景 update 里
    scene->setUpdateFn([](Scene& s, float, float time) {
        s.objects()[0].transform =
            glm::rotate(glm::mat4(1.0f), time * glm::radians(90.0f),
                        glm::vec3(0.0f, 0.0f, 1.0f));
    });
    scene->initialCamera = CameraPose{{2.0f, 2.0f, 2.0f}, -135.0f, -30.0f};
    return scene;
}

SceneFactory vikingRoomSceneFactory(std::string tex, std::string vp, std::string fp) {
    return [tex, vp, fp](Device& d, FrameSync& fs, Renderer& r) {
        return buildObjScene(d, fs, r, "models/viking_room.obj", tex, vp, fp);
    };
}

// sheenChairSceneFactory 类似，内部调用 GltfLoader 版本
```

### B. 修改 `src/app/Application.h`

```cpp
class Application {
    // ... 原有 GPU 基础设施保留 ...

    // 删除：shared_ptr<Texture>/Material/Mesh, vector<shared_ptr<Mesh>> gltfMeshes_
    // 删除：Scene scene_;

    // 新增：
    std::vector<SceneEntry>   sceneRegistry_;
    std::unique_ptr<Scene>    currentScene_;
    int                       currentSceneIndex_ = -1;

    void registerScene(SceneEntry entry);
    void switchScene(int index);

    Camera camera_;
};
```

### C. 修改 `src/app/Application.cpp`

#### init() 简化

```cpp
void Application::init() {
    // ... Window/Input/Context/Device/SwapChain/FrameSync/Renderer 创建不变 ...

    window_->setResizeCallback([this](int, int) { frameSync_->notifyResize(); });

    // 不再在 init() 里直接加载模型，而是注册场景 + 激活第一个
    registerBuiltinScenes();             // 由 main.cpp 或此处调用
    switchScene(config_.defaultSceneIndex);

    camera_.setAspect(
        float(swapChain_->extent().width) / float(swapChain_->extent().height));
}
```

#### switchScene 实现

```cpp
void Application::switchScene(int index) {
    if (index < 0 || index >= int(sceneRegistry_.size())) return;
    if (index == currentSceneIndex_) return;

    // GPU 等空，确保旧资源安全销毁
    if (device_) vkDeviceWaitIdle(device_->logicalDevice());

    currentScene_.reset();   // 释放旧场景全部资源

    const auto& entry = sceneRegistry_[index];
    currentScene_ = entry.factory(*device_, *frameSync_, *renderer_);
    currentSceneIndex_ = index;

    if (currentScene_->initialCamera) {
        const auto& p = *currentScene_->initialCamera;
        camera_.setPosition(p.position);
        camera_.setYawPitch(p.yaw, p.pitch);
    }

    std::cout << "[Scene] switched to " << entry.name << "\n";
}
```

#### mainLoop 清理

```cpp
// 原来：
// scene_.objects()[0].transform = glm::rotate(...);
// scene_.render(...);

// 改为：
float time = std::chrono::duration<float>(now - startTime).count();
if (currentScene_) currentScene_->update(dt, time);

// 热键切换（在 processInput 中或此处）
for (size_t i = 0; i < sceneRegistry_.size(); ++i) {
    if (input_->isKeyPressed(sceneRegistry_[i].hotKey)) {
        switchScene(int(i));
        break;
    }
}

renderer_->beginRenderPass(ctx->cmd, ctx->imageIndex);
if (currentScene_) currentScene_->render(ctx->cmd, ctx->frameIndex);
renderer_->endRenderPass(ctx->cmd);
```

> **注意**：`InputManager` 需要一个"仅按下当帧触发一次"的 `isKeyPressed()`（边沿触发），
> 不同于已有的 `isKeyDown()`（按住期间一直 true）。
> 如果当前没有这个方法，作为本步最小增量补一个。

### D. 修改 `src/app/Config.h`

```cpp
struct Config {
    // 删除：modelPath / texturePath（挪到场景工厂内部）
    // 保留：vertShaderPath / fragShaderPath（还是公共的）
    // 新增：
    int defaultSceneIndex = 0;

    // 原字段保留，用来让场景工厂读取
    std::string texturePath      = "textures/viking_room.png";
    std::string vertShaderPath   = "shader/vert.spv";
    std::string fragShaderPath   = "shader/frag.spv";
    // ...
};
```

### E. `main.cpp` 注册场景

```cpp
int main() {
    vkr::Config config;
    vkr::Application app(config);

    app.registerScene({"Viking Room", Key::F1,
        vkr::vikingRoomSceneFactory(
            config.texturePath, config.vertShaderPath, config.fragShaderPath)});

    app.registerScene({"Sheen Chair", Key::F2,
        vkr::sheenChairSceneFactory(
            config.texturePath, config.vertShaderPath, config.fragShaderPath)});

    try { app.run(); }
    catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
    return 0;
}
```

> `registerScene` 必须在 `run()` → `init()` 之前调用，或者改为 `run()` 内先调用
> 用户提供的 `registerScenes(App&)` 回调。前者更直接。

### F. Camera 新增 setter

Scene 的 initialCamera 需要设置相机位姿，给 `Camera` 加（如果尚未存在）：
```cpp
void Camera::setPosition(const glm::vec3& p);
void Camera::setYawPitch(float yaw, float pitch);
```

---

## 4. 线程/同步保证

| 操作 | 同步保证 |
|------|---------|
| 销毁旧 Scene（释放 Texture/Material/Mesh） | `vkDeviceWaitIdle` 保证 GPU 端不再引用 |
| 构造新 Scene（上传 staging 缓冲） | `FrameSync::beginSingleTimeCommands/endSingleTimeCommands` 内部 `vkQueueWaitIdle`，已是同步 |
| 切换发生在主循环中 | 保证在 `beginFrame/endFrame` 之外，不会破坏当前帧 |

**唯一需要注意**：`switchScene` 不能在 `beginFrame()` 和 `endFrame()` 之间调用——
它会 stall 正在记录的 command buffer。我们把切换检测放在 `processInput` 阶段，
即 `beginFrame` 之前，天然满足这点。

---

## 5. 未来扩展的预留接口

以下都是**本步不做**但设计上保持向前兼容的方向：

| 未来特性 | 当前设计如何配合 |
|---------|----------------|
| 场景特有的 update 数据（粒子系统、动画状态机） | 把 Scene 改成虚基类，派生特定 Scene，覆盖 `update()`；或者在 Scene 里持有一个 `unique_ptr<SceneLogic>` |
| 异步加载 | `SceneFactory` 返回 `future<unique_ptr<Scene>>`，在主循环里 poll |
| ImGui 场景选择下拉框（step6 完成后） | 直接读 `sceneRegistry_` 展示列表，调用 `switchScene(i)` |
| 场景内多材质、多 Pipeline | Scene 内再持有 `vector<Pipeline>` 即可，Scene 所有权模型不变 |
| 场景保存/加载 | 在 `SceneEntry` 加 `serialize()`/`deserialize()`；当前工厂模式与之正交 |

---

## 6. 验收标准

- [ ] 启动时默认加载 Viking Room，表现与当前一致（旋转、贴图、深度正常）
- [ ] 按 F2 切到 Sheen Chair，无崩溃、无 validation error、无资源泄漏
- [ ] 多次来回切换（F1/F2 交替）稳定运行 ≥ 1 分钟
- [ ] RenderDoc 抓帧：切换后旧 Texture/Buffer 已从 Device 释放
- [ ] 场景相机位姿按 `initialCamera` 正确应用
- [ ] 硬编码的 `scene_.objects()[0]` 引用全部消失
- [ ] Application 不再直接持有 mesh_/texture_/material_/gltfMeshes_ 字段

---

## 7. 提交顺序建议（可拆子 PR）

1. Scene 扩展（资源所有权 + update 钩子 + initialCamera） — 纯结构改动
2. SceneFactory + BuiltinScenes — 把 init() 的模型加载搬进工厂
3. Application 接入 sceneRegistry_ + switchScene，`init()` 瘦身
4. InputManager 增加 `isKeyPressed`（边沿触发），接入 F1/F2 切换
5. 删除 Config 中 `modelPath`，回归测试

每一步都可独立编译运行，降低回归风险。
