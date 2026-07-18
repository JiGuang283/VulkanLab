# 鼠标无法控制视角 — 分析与修复方案

> 症状：按住鼠标右键进入 `CameraDrag` 模式后，移动鼠标完全不改变视角；WASD 位移正常。
> 目标：定位根因，给出最小改动修复，附可选的健壮性加强项。

---

## 1. 现场复盘

`Application::mainLoop()` 的每帧顺序（[src/app/Application.cpp](../../../src/app/Application.cpp)）：

```cpp
while (!window_->shouldClose()) {
    window_->pollEvents();      // ①  触发 GLFW mouseCallback，累加 mouseDelta_
    input_->update();           // ②  ← 见下文
    ...
    updateInputMode();
    if (mode_ == InputMode::CameraDrag)
        processCameraInput(dt); // ③  读取 input_->mouseDelta()
    ...
}
```

`InputManager::update()`（[src/window/InputManager.cpp](../../../src/window/InputManager.cpp)）：

```cpp
void InputManager::update() {
    prevKeys_    = currKeys_;
    prevButtons_ = currButtons_;
    for (...) currKeys_[k]    = glfwGetKey(...);
    for (...) currButtons_[b] = glfwGetMouseButton(...);

    mouseDelta_    = {0.0f, 0.0f};   // ← 关键语句
    rawMouseDelta_ = {0.0f, 0.0f};
}
```

`mouseCallback` 在 `pollEvents()` 里同步执行，把位移累加进 `mouseDelta_`。

---

## 2. 根因：时序错配

每帧顺序实际上是：

| 步骤 | 行为 | `mouseDelta_` |
|---|---|---|
| ①  `pollEvents` | callback 累加本帧位移 `dx, dy` | `= (dx, dy)` |
| ②  `update()` | **立刻清零** | `= (0, 0)` |
| ③  `processCameraInput` | 读 `mouseDelta()` | **永远读到 0** |

**任何帧读到的鼠标增量都是 0**，`camera_.rotate(0, 0)` 自然不动视角。

> 注意：`input_->update()` 的本意是"帧首：把 curr 推入 prev，准备接收新数据"。**但它同时把鼠标增量也清了**，而鼠标增量是在 `pollEvents` 里 *已经写入* 的"本帧数据"。它和按键状态是两种不同的数据流（按键靠 `glfwGetKey` 在 `update` 里 *拉取*；鼠标靠 callback 在 `pollEvents` 里 *推入*），但现有代码把它们当同类处理，于是把"推入"的数据也一起清零了。

---

## 3. 修复方案

### 3.1 最小修复（推荐）

**策略**：鼠标增量在 *消费之后* 清零，而不是 *接收之后* 立刻清零。

把 [src/window/InputManager.cpp](../../../src/window/InputManager.cpp) 里 `update()` 内的两行清零**删除**；在 [src/window/InputManager.h](../../../src/window/InputManager.h) 给增量 getter 改为"取出并清零"语义，或者新增显式 `endFrame()`。

**方案 A — 新增 `endFrame()`（语义最清晰）**：

```cpp
// InputManager.h
void endFrame();   // 放在帧末：清零鼠标增量
```

```cpp
// InputManager.cpp
void InputManager::update() {
    prevKeys_    = currKeys_;
    prevButtons_ = currButtons_;
    for (int k = 0; k < kKeyCount; ++k)
        currKeys_[k] = glfwGetKey(window_, k) == GLFW_PRESS;
    for (int b = 0; b < kButtonCount; ++b)
        currButtons_[b] = glfwGetMouseButton(window_, b) == GLFW_PRESS;
    // 不再清零 mouseDelta_ / rawMouseDelta_
}

void InputManager::endFrame() {
    mouseDelta_    = {0.0f, 0.0f};
    rawMouseDelta_ = {0.0f, 0.0f};
}
```

```cpp
// Application.cpp · mainLoop() 末尾
// 7. 渲染
...
// 8. 帧末：丢弃本帧鼠标增量
input_->endFrame();
```

**方案 B — 调换顺序（零新 API，最一行改动）**：

```cpp
// Application.cpp
window_->pollEvents();
input_->update();       // ← 这里不动
// ↑↑↑ 但把 update() 里的 mouseDelta_ 清零两行删掉
```
在 `pollEvents` 之前先清零（即保留 `update` 中清零行为，但调换调用顺序）：

```cpp
input_->update();        // 先清零 + 更新 prev/curr（curr 会在下一次 update 被覆盖）
window_->pollEvents();   // 后累加 delta
updateInputMode();
processCameraInput(dt);
```

> 缺点：`update()` 读出来的 `currKeys_` 是 `pollEvents` 之前的状态（慢一帧）。对帧率敏感时序不友好。
> **不推荐。**

### 3.2 推荐实施 = 方案 A

改动文件：
- [src/window/InputManager.h](../../../src/window/InputManager.h) — 声明 `void endFrame();`
- [src/window/InputManager.cpp](../../../src/window/InputManager.cpp) — `update()` 去掉两行清零；新增 `endFrame()`。
- [src/app/Application.cpp](../../../src/app/Application.cpp) — `mainLoop()` 每帧末尾调 `input_->endFrame();`（放在渲染提交**之后**即可，场景切换分支也会途经这里，无需特殊处理）。

---

## 4. 次要加强（可选）

1. **首帧吞掉巨大跳变**：进入 `CameraDrag` 的那一帧，`lastMouseX_/Y_` 已被 `setCursorCaptured(true)` 里的 `firstMouse_ = true` 标记重置 — 这一路径 OK，保持原样。
2. **捕获模式下的 `rawMouseDelta_`**：当前 callback 里 `rawMouseDelta_` 总累加，`mouseDelta_` 只在捕获时累加。方案 A 不影响该语义。
3. **ImGui 接管鼠标时的一致性**：`CameraDrag` 激活时已经设置 `ImGuiConfigFlags_NoMouse` + `GLFW_CURSOR_DISABLED`，ImGui 不会再 hover 消费 — OK。
4. **回归测试建议**：在 `drawGui()` 的 `Stats` 面板临时加一行 `ImGui::Text("mDelta: %.2f, %.2f", d.x, d.y);` 观察数值，在修复前后对比。

---

## 5. 验证

1. 编译：`cmake --build build --config Release`
2. 运行：右键按住画面，移动鼠标 → 视角应跟随；松开右键鼠标回到 ImGui；WASD 仍然正常。
3. 反复进入/退出 `CameraDrag` 模式 5 次，`Stats` 面板 `Camera` 坐标和 yaw/pitch 变化应与操作一致。
4. 快速切场景时视角不应发生意外旋转（首帧 `firstMouse_` 保护生效）。

---

## 6. 回滚

单文件回滚：
```powershell
git checkout HEAD -- src/window/InputManager.h src/window/InputManager.cpp src/app/Application.cpp
```

---

## 7. 结论

视角死锁的根因不是相机/捕获/ImGui 任何一处，而是 `InputManager::update()` 把 GLFW 回调 *已经写入本帧* 的鼠标增量立刻清零。把增量的清零时机挪到帧末（`endFrame()`），或者不要在 `update()` 里清它，问题即消失。改动范围 3 个文件、约 10 行。
