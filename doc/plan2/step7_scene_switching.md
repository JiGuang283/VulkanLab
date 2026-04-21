# 第七步：场景切换 + 编辑器式输入（Scene Switching & Editor Input）

> v2（2026-04-21 修订）
> 原版将场景切换绑定到 F1/F2 热键；接入 ImGui（step6）后出现新约束：
> 游戏模式下鼠标被 `GLFW_CURSOR_DISABLED` 捕获，ImGui 无法点击。
> 本步把交互范式统一成**类游戏引擎编辑器**：
>   - 默认光标可用，ImGui 可点击；
>   - 按住鼠标右键才进入"场景漫游"，松开立即回到 UI；
>   - 场景切换改为 ImGui 窗口里的列表/下拉，不再占用热键。

---

## 0. 背景：step6 之后暴露的问题

1. **鼠标冲突**。`Application::mainLoop()` 一开始就 `setCursorCaptured(true)`，光标被隐藏并锁在窗口中心，ImGui 获得的光标位置是中心点，无法点击按钮。
2. **单一输入模式**。当前代码只在 `wantCaptureMouse()` 为真时临时让出光标，但 ImGui 的 `WantCaptureMouse` 需要先检测到光标进入它自己的窗口才会变 true——光标一开始就被锁住根本进不去，陷入死循环。
3. **硬编码旋转 + 硬编码模型加载**（原 v1 已指出）。

核心结论：**应用有两种稳定的状态**——UI 状态（鼠标归 ImGui/OS）和漫游状态（鼠标归相机），必须显式、原子地切换。

---

## 1. 目标

### 1.1 场景切换（沿用 v1 目标）
- 启动时可通过 Config 指定默认场景
- 运行时通过 **ImGui 面板** 切换场景（不再用 F1/F2）
- 切换时正确释放旧场景的 GPU 资源
- 每个场景可以有自己的模型、贴图、初始相机位置、每帧更新逻辑

### 1.2 编辑器式输入（新）
- 光标默认可见，能点击 ImGui
- **按住鼠标右键**进入场景漫游：鼠标移动旋转相机，WASD/Space/Shift 平移
- **松开鼠标右键**：立刻恢复光标并回到 UI
- 如果右键**起始按下的位置在 ImGui 窗口之上**，不触发漫游（把事件让给 UI）
- 漫游过程中光标被隐藏，相机获得"无限量程"的鼠标增量；松开时光标恢复到按下前的位置（Unity/Unreal 的 Scene View 行为）

### 1.3 不做的事
- ❌ 场景序列化
- ❌ 热重载
- ❌ 场景图层级
- ❌ 异步加载
- ❌ 撤销/重做、gizmo、选择高亮（后续步骤可做）

---

## 2. 当前架构分析

### 2.1 资源所有权（已在 v1 分析，此处简述）

`Application` 直接持有 `texture_/material_/mesh_/gltfMeshes_`，切场景需要手动清场，容易遗漏。

**决策**：把这些移入 `Scene`，让 Scene 为自身资源的**唯一所有者**。切换场景 = `reset()` 旧 Scene → 构造新 Scene。

### 2.2 输入管道现状

```cpp
// InputManager（精简视图）
void update();                  // 仅重置 mouseDelta_
bool isKeyDown(Key);            // 查询式
glm::vec2 mouseDelta();         // 光标被捕获时才累加
void setCursorCaptured(bool);   // GLFW_CURSOR_DISABLED / NORMAL
```

问题：
- `mouseDelta_` 只在 `cursorCaptured_` 为真时被回调累加——编辑器模式下非漫游期间也应累加（将来鼠标拖拽 gizmo 会需要）。
- 没有**鼠标按键**状态，无法实现"按住右键才漫游"。
- 没有**边沿触发**（`isKeyPressed` / `isMouseButtonPressed`），一次性事件不好写。

### 2.3 跨场景不变的部分

| 对象 | 说明 |
|------|------|
| Window / InputManager / Context / Device / SwapChain / FrameSync / Renderer / Pipeline / GuiSystem | 生命周期与应用一致，切场景不动 |
| Camera | 观察者；切场景时只重置位姿，不重建 |

---

## 3. 顶层设计

### 3.1 InputMode：显式的模式状态机

在 `Application` 里维护一个枚举：

```cpp
enum class InputMode {
    UI,          // 光标可见，ImGui 接管
    CameraDrag,  // 光标隐藏并锁定，相机接管鼠标
};
```

状态机：

```
  ┌──────────┐   右键按下(且未在ImGui上)   ┌──────────────┐
  │    UI    │ ────────────────────────▶ │  CameraDrag  │
  │          │ ◀──────────────────────── │              │
  └──────────┘       右键释放              └──────────────┘
```

**进入 CameraDrag 的条件**（必须全部满足）：
1. 当前是 UI 模式
2. 本帧检测到鼠标右键**边沿按下**
3. `ImGui::GetIO().WantCaptureMouse` 为 **false**（点击未命中任何 ImGui 窗口）
4. `ImGui::IsAnyItemActive()` 为 **false**（例如正在拖滑块，不能被抢走）

**切换动作**：
- 进入 CameraDrag：记录光标位置 `savedCursor_`，设 `GLFW_CURSOR_DISABLED`，重置 `firstMouse_`，禁用 ImGui 鼠标输入（见 3.2）。
- 退出 CameraDrag：恢复 `GLFW_CURSOR_NORMAL`，`glfwSetCursorPos(savedCursor_)` 还原光标。

### 3.2 ImGui 与输入的共存

- 继续用 `ImGui_ImplGlfw_InitForVulkan(window, true)`——ImGui 链式接管 GLFW 回调。
- 在 CameraDrag 模式时设 `ImGuiConfigFlags_NoMouse`，防止 ImGui 把隐藏光标当成"鼠标停留在某窗口"。
- UI 模式时移除该 flag。

```cpp
auto& io = ImGui::GetIO();
if (mode == InputMode::CameraDrag) io.ConfigFlags |=  ImGuiConfigFlags_NoMouse;
else                                io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
```

### 3.3 相机输入改造

- 仅在 `mode == CameraDrag` 时 `camera_.rotate(delta.x * sens, -delta.y * sens)`；
- WASD/Space/Shift 同样仅在 CameraDrag 时响应（符合编辑器习惯）。
- Escape 键始终生效——任何模式按 Esc 退出（或先退漫游）。

### 3.4 场景切换 UI

ImGui 窗口 `Scene` 里放一个列表：

```cpp
void Application::drawGui() {
    ImGui::Begin("Scene");
    for (int i = 0; i < (int)sceneRegistry_.size(); ++i) {
        const bool selected = (i == currentSceneIndex_);
        if (ImGui::Selectable(sceneRegistry_[i].name.c_str(), selected))
            pendingSceneIndex_ = i;          // 延迟到下一帧开头切
    }
    ImGui::End();

    ImGui::Begin("Stats");
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    const auto p = camera_.position();
    ImGui::Text("Camera: (%.2f, %.2f, %.2f)", p.x, p.y, p.z);
    ImGui::Text("Mode: %s", mode_ == InputMode::UI ? "UI" : "Drag");
    ImGui::End();
}
```

**为什么用 `pendingSceneIndex_` 而不是立刻 `switchScene(i)`？**
ImGui 点击发生在 `beginFrame()` 之后；此时已经 `ImGui::NewFrame()`。
直接 `vkDeviceWaitIdle` + 释放资源虽然能跑，但把 GPU-stall 放在 UI 交互里语义混乱。
统一在下一帧**循环开始处**执行切换，保证所有切换都在帧外完成：

```cpp
// mainLoop 开头
if (pendingSceneIndex_ != -1) {
    switchScene(pendingSceneIndex_);
    pendingSceneIndex_ = -1;
}
```

---

## 4. InputManager 增量接口

保持向后兼容，新增：

```cpp
enum class MouseButton : int {
    Left   = 0,  // GLFW_MOUSE_BUTTON_LEFT
    Right  = 1,
    Middle = 2,
};

class InputManager {
  public:
    // 已有：isKeyDown / mouseDelta / setCursorCaptured ...

    // 新增：鼠标按键（查询 + 边沿）
    bool isMouseDown(MouseButton) const;
    bool isMousePressed(MouseButton) const;    // 上升沿
    bool isMouseReleased(MouseButton) const;   // 下降沿

    // 新增：键盘边沿触发
    bool isKeyPressed(Key) const;

    // 新增：无视 cursorCaptured_ 总是累加 delta
    glm::vec2 rawMouseDelta() const;

    // 光标位置（用于进入漫游前保存）
    glm::dvec2 cursorPos() const;
    void       setCursorPos(glm::dvec2);
};
```

### 4.1 实现要点

- 在 `update()` 里同步上一帧按键/按钮状态到 `prevKeys_ / prevButtons_`，生成边沿信号。
- `mouseCallback` 始终累加 `rawMouseDelta_`；`mouseDelta_` 在 `cursorCaptured_` 下同步累加（兼容原行为）。
- `cursorPos()` 直接调用 `glfwGetCursorPos`。

---

## 5. Scene / SceneFactory / BuiltinScenes

### 5.1 Scene 扩展（与 v1 一致）

```cpp
struct CameraPose {
    glm::vec3 position{2.0f, 2.0f, 2.0f};
    float     yaw   = -135.0f;
    float     pitch = -30.0f;
};

class Scene {
  public:
    using UpdateFn = std::function<void(Scene&, float dt, float time)>;

    void addTexture(std::shared_ptr<Texture>);
    void addMaterial(std::shared_ptr<Material>);
    void addMesh(std::shared_ptr<Mesh>);
    void addObject(SceneObject);

    void render(VkCommandBuffer, uint32_t frameIndex, Pipeline&) const;
    void setUpdateFn(UpdateFn);
    void update(float dt, float time);

    std::vector<SceneObject>       &objects();
    const std::vector<SceneObject> &objects() const;

    std::optional<CameraPose> initialCamera;

  private:
    std::vector<std::shared_ptr<Texture>>  textures_;
    std::vector<std::shared_ptr<Material>> materials_;
    std::vector<std::shared_ptr<Mesh>>     meshes_;
    std::vector<SceneObject>               objects_;
    UpdateFn                               updateFn_;
};
```

### 5.2 SceneFactory / SceneEntry

相较 v1 去掉 `hotKey` 字段：

```cpp
using SceneFactory = std::function<
    std::unique_ptr<Scene>(Device&, FrameSync&, Renderer&, Pipeline&)
>;

struct SceneEntry {
    std::string  name;
    SceneFactory factory;
};
```

> 注：`Pipeline` 目前是 Application 拥有的共享 `opaquePipeline_`，场景工厂只引用不拥有。
> 将来场景自带 pipeline 时，改为 Scene 持有 `vector<unique_ptr<Pipeline>>` 即可，接口不破坏。

### 5.3 BuiltinScenes

集中放具体场景构造函数：

```cpp
// BuiltinScenes.h
SceneFactory vikingRoomSceneFactory(std::string tex, std::string vp, std::string fp);
SceneFactory sheenChairSceneFactory(std::string tex, std::string vp, std::string fp);
```

工厂内部把原 `Application::init()` 里模型/贴图/材质的构造逻辑搬过来，**并把硬编码的旋转写进 `setUpdateFn`**：

```cpp
scene->setUpdateFn([](Scene& s, float, float time) {
    s.objects()[0].transform =
        glm::rotate(glm::mat4(1.0f), time * glm::radians(90.0f),
                    glm::vec3(0.0f, 0.0f, 1.0f));
});
scene->initialCamera = CameraPose{{2,2,2}, -135.0f, -30.0f};
```

---

## 6. Application 结构变化

### 6.1 头文件

```cpp
class Application {
  public:
    explicit Application(const Config& = {});
    void run();
    void registerScene(SceneEntry entry);

  private:
    // --- 基础设施 ---
    std::unique_ptr<Window>        window_;
    std::unique_ptr<InputManager>  input_;
    std::unique_ptr<VulkanContext> context_;
    std::unique_ptr<Device>        device_;
    std::unique_ptr<SwapChain>     swapChain_;
    std::unique_ptr<FrameSync>     frameSync_;
    std::unique_ptr<Renderer>      renderer_;
    std::unique_ptr<Pipeline>      opaquePipeline_;
    std::unique_ptr<GuiSystem>     gui_;

    // --- 场景切换 ---
    std::vector<SceneEntry>   sceneRegistry_;
    std::unique_ptr<Scene>    currentScene_;
    int                       currentSceneIndex_ = -1;
    int                       pendingSceneIndex_ = -1;

    // --- 输入模式 ---
    InputMode                 mode_ = InputMode::UI;
    glm::dvec2                savedCursor_{};

    Camera camera_;

    void init();
    void mainLoop();
    void drawGui();
    void switchScene(int index);
    void updateInputMode();
    void processCameraInput(float dt);
    void updateUniforms(uint32_t frameIndex);
};
```

删除字段：`texture_`, `material_`, `mesh_`, `gltfMeshes_`, `scene_`。

### 6.2 mainLoop 骨架

```cpp
void Application::mainLoop() {
    // 不再启动时捕获光标——默认 UI 模式
    auto startTime = std::chrono::high_resolution_clock::now();
    auto lastTime  = startTime;

    while (!window_->shouldClose()) {
        window_->pollEvents();
        input_->update();   // 刷新边沿 + 清 mouseDelta

        // 1. 帧外：场景切换
        if (pendingSceneIndex_ != -1) {
            switchScene(pendingSceneIndex_);
            pendingSceneIndex_ = -1;
        }

        // 2. 时间
        auto  now = std::chrono::high_resolution_clock::now();
        float dt  = std::chrono::duration<float>(now - lastTime).count();
        lastTime  = now;

        // 3. ImGui 新帧（updateInputMode 依赖 io）
        gui_->beginFrame();

        // 4. 模式切换 + 输入
        updateInputMode();
        if (mode_ == InputMode::CameraDrag) processCameraInput(dt);
        if (input_->isKeyDown(Key::Escape)) window_->setShouldClose(true);

        // 5. 场景 tick
        float t = std::chrono::duration<float>(now - startTime).count();
        if (currentScene_) currentScene_->update(dt, t);

        // 6. UI
        drawGui();

        // 7. 渲染
        auto ctx = frameSync_->beginFrame();
        if (!ctx) {
            handleSwapChainRecreate();
            ImGui::EndFrame();
            continue;
        }
        updateUniforms(ctx->frameIndex);
        renderer_->beginRenderPass(ctx->cmd, ctx->imageIndex);
        if (currentScene_) currentScene_->render(ctx->cmd, ctx->frameIndex, *opaquePipeline_);
        gui_->render(ctx->cmd);
        renderer_->endRenderPass(ctx->cmd);
        frameSync_->endFrame(*ctx);

        if (frameSync_->swapChainNeedsRecreation()) handleSwapChainRecreate();
    }
    vkDeviceWaitIdle(device_->logicalDevice());
}
```

### 6.3 updateInputMode

```cpp
void Application::updateInputMode() {
    auto& io = ImGui::GetIO();

    if (mode_ == InputMode::UI) {
        const bool pressed = input_->isMousePressed(MouseButton::Right);
        const bool overUI  = io.WantCaptureMouse || ImGui::IsAnyItemActive();
        if (pressed && !overUI) {
            savedCursor_ = input_->cursorPos();
            input_->setCursorCaptured(true);
            io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
            mode_ = InputMode::CameraDrag;
        }
    } else { // CameraDrag
        if (input_->isMouseReleased(MouseButton::Right)) {
            input_->setCursorCaptured(false);
            input_->setCursorPos(savedCursor_);
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
            mode_ = InputMode::UI;
        }
    }
}
```

### 6.4 processCameraInput

把原 `processInput(dt)` 的 WASD + mouseDelta 改名并改为只在 CameraDrag 时调用：

```cpp
void Application::processCameraInput(float dt) {
    glm::vec3 move{0.0f};
    if (input_->isKeyDown(Key::W))         move.z += config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::S))         move.z -= config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::A))         move.x -= config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::D))         move.x += config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::Space))     move.y += config_.moveSpeed * dt;
    if (input_->isKeyDown(Key::LeftShift)) move.y -= config_.moveSpeed * dt;
    camera_.translate(move);

    const auto d = input_->mouseDelta();
    camera_.rotate(d.x * config_.mouseSensitivity,
                  -d.y * config_.mouseSensitivity);
}
```

### 6.5 switchScene

```cpp
void Application::switchScene(int index) {
    if (index < 0 || index >= (int)sceneRegistry_.size()) return;
    if (index == currentSceneIndex_) return;

    vkDeviceWaitIdle(device_->logicalDevice());
    currentScene_.reset();

    const auto& entry = sceneRegistry_[index];
    currentScene_ = entry.factory(*device_, *frameSync_, *renderer_, *opaquePipeline_);
    currentSceneIndex_ = index;

    if (currentScene_->initialCamera) {
        const auto& p = *currentScene_->initialCamera;
        camera_.setPosition(p.position);
        camera_.setYawPitch(p.yaw, p.pitch);
    }
}
```

---

## 7. Camera 需要新增的 setter

```cpp
void Camera::setYawPitch(float yaw, float pitch);
```

`setPosition` 已有。

---

## 8. Config 变化

```cpp
struct Config {
    // 删除：modelPath（移入场景工厂）
    std::string texturePath    = "textures/viking_room.png";
    std::string vertShaderPath = "shader/vert.spv";
    std::string fragShaderPath = "shader/frag.spv";

    float moveSpeed        = 2.0f;
    float mouseSensitivity = 0.1f;

    int defaultSceneIndex  = 0;
    // ...其余字段不变
};
```

---

## 9. main.cpp 注册

```cpp
int main() {
    vkr::Config cfg;
    vkr::Application app(cfg);

    app.registerScene({"Viking Room",
        vkr::vikingRoomSceneFactory(cfg.texturePath, cfg.vertShaderPath, cfg.fragShaderPath)});
    app.registerScene({"Sheen Chair",
        vkr::sheenChairSceneFactory(cfg.texturePath, cfg.vertShaderPath, cfg.fragShaderPath)});

    try { app.run(); }
    catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
    return 0;
}
```

---

## 10. 同步/边界条件

| 场景 | 保障 |
|------|-----|
| 场景切换时释放 GPU 资源 | 在 mainLoop 开头、`beginFrame()` 之前 `vkDeviceWaitIdle` |
| ImGui 正在拖滑块时误按右键 | `IsAnyItemActive()` 判守，优先让 UI 吃事件 |
| 窗口失焦 / Alt-Tab | GLFW `WindowFocusCallback` 中强制 `mode_=UI` 并 `setCursorCaptured(false)`（可选扩展） |
| 光标从屏幕边缘回来 | `GLFW_CURSOR_DISABLED` 使鼠标增量无限，无需处理 |
| 漫游中窗口 resize | swap chain 重建分支已有，与输入模式正交 |

---

## 11. 验收标准

- [ ] 启动后光标可见，能点 ImGui 按钮/选项
- [ ] 点击 ImGui `Scene` 面板中 "Sheen Chair"，**下一帧**完成切换，无崩溃、无 validation 错误
- [ ] ImGui 窗口外**按住鼠标右键**：光标消失，鼠标移动能旋转相机，WASD 能平移
- [ ] 松开右键：光标出现在原位置，可继续点击 ImGui
- [ ] 右键按下瞬间鼠标正悬停在 ImGui 窗口之上 → **不进入漫游**，ImGui 正常响应
- [ ] 多次 UI↔Drag 切换稳定；连续切换场景 ≥ 10 次无泄漏
- [ ] Application 不再直接持有 `texture_/material_/mesh_/gltfMeshes_/scene_`
- [ ] 原来的硬编码 `scene_.objects()[0].transform = rotate(...)` 已移到场景工厂
- [ ] Stats 面板显示当前 FPS、相机位置、输入模式

---

## 12. 提交顺序建议

1. **InputManager 扩展**：加鼠标按钮、边沿触发、`cursorPos`/`setCursorPos`、`rawMouseDelta`（纯加法，不破坏现有 API）。
2. **Camera 扩展**：`setYawPitch`。
3. **Scene 扩展**：资源所有权字段、`update()` 钩子、`initialCamera`（纯加法）。
4. **SceneFactory + BuiltinScenes**：把 init() 里模型/贴图构造搬进工厂函数；暂先保持 Application 旧逻辑并存，用一个 factory 构建初始场景。
5. **Application 改造**：引入 `InputMode`、`updateInputMode`、`processCameraInput`、`pendingSceneIndex_`、`switchScene`；删除旧 `scene_/texture_/material_/mesh_`；`drawGui` 里加 Scene/Stats 面板。
6. **main.cpp**：注册两个场景；移除 Config 中 `modelPath`。
7. **回归测试**：两场景来回切、UI/漫游切换、resize 组合。

每一步都可编译运行。

---

## 13. 未来扩展接口（保持兼容）

| 需求 | 当前设计如何自然演进 |
|------|---------------------|
| 选中物体 / 属性面板 | Scene 公开 `objects()`，ImGui 遍历即可；Gizmo 之后加 `GizmoDrag` 模式 |
| 多 Pipeline / 多 Material 场景 | Scene 持有 vector<unique_ptr<Pipeline>> 与资源一起管理 |
| 游戏模式（全屏 + 捕获光标） | 再加 `InputMode::Game`：光标始终锁定，无需右键 |
| 暂停 / 步进调试 | `Scene::update` 接受 dt，外层轻松拦截 |
| 异步场景加载 | `SceneFactory` 改为返回 `std::future<unique_ptr<Scene>>`；mainLoop poll |
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
