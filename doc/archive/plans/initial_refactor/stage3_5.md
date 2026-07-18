# 阶段 3.5：Camera

## 一、目标

引入 Camera 类，将 `updateUniformBuffer()` 中硬编码的 view / proj 矩阵替换为 Camera 驱动的计算，为后续阶段 4（Window / Input）接入键鼠控制做好准备。

---

## 二、当前状态

`app.cpp` 中 `updateUniformBuffer()`：

```cpp
void HelloTriangleApplication::updateUniformBuffer(uint32_t currentImage) {
    UniformBufferObject ubo{};
    ubo.view = glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f),
                           glm::vec3(0.0f, 0.0f, 0.0f),
                           glm::vec3(0.0f, 0.0f, 1.0f));
    ubo.proj = glm::perspective(glm::radians(45.0f),
                                swapChain_->extent().width /
                                    (float)swapChain_->extent().height,
                                0.1f, 10.0f);
    ubo.proj[1][1] *= -1;
    memcpy(renderer_->mappedUniformBuffer(currentImage), &ubo, sizeof(ubo));
}
```

- view 矩阵：固定在 (2,2,2) 看向原点，up = Z 轴
- proj 矩阵：45° FOV，aspect 随窗口，0.1~10.0
- Vulkan Y 翻转（`proj[1][1] *= -1`）
- 无法动态调整相机参数

---

## 三、类设计

### 3.1 Camera

```cpp
// src/scene/Camera.h
#pragma once
#include <glm/glm.hpp>

namespace vkr {

class Camera {
public:
    Camera();

    // ---- 投影参数 ----
    void setPerspective(float fovDeg, float aspect, float nearPlane, float farPlane);
    void setAspect(float aspect);

    // ---- 位置与朝向 ----
    void setPosition(const glm::vec3& pos);
    void lookAt(const glm::vec3& target);

    // ---- FPS 风格控制（为阶段 4 预留接口）----
    void rotate(float yawOffset, float pitchOffset);   // 鼠标偏移 → 旋转
    void translate(const glm::vec3& localOffset);       // 本地坐标系平移 (right/up/front)

    // ---- 矩阵输出 ----
    glm::mat4 viewMatrix() const;
    glm::mat4 projectionMatrix() const;   // 已包含 Vulkan Y 翻转

    // ---- 访问器 ----
    glm::vec3 position() const { return position_; }
    glm::vec3 front()    const { return front_; }
    glm::vec3 right()    const { return right_; }
    glm::vec3 up()       const { return up_; }

private:
    void updateVectors();   // 从 yaw_/pitch_ 重新计算 front_/right_/up_

    // 位置
    glm::vec3 position_{2.0f, 2.0f, 2.0f};

    // 欧拉角（度）
    float yaw_   = -135.0f;   // 初始朝向：从 (2,2,2) 看向原点在 XY 平面的投影
    float pitch_ = -35.264f;  // arcsin(1/√3) ≈ 35.264°，向下看

    // 通过 updateVectors() 计算
    glm::vec3 front_;
    glm::vec3 right_;
    glm::vec3 up_;

    // 世界上方向（本项目使用 Z-up）
    glm::vec3 worldUp_{0.0f, 0.0f, 1.0f};

    // 透视参数
    float fov_       = 45.0f;
    float aspect_    = 800.0f / 600.0f;
    float nearPlane_ = 0.1f;
    float farPlane_  = 10.0f;
};

} // namespace vkr
```

### 设计要点

1. **Z-up 坐标系** — 与当前代码一致，`worldUp_ = (0,0,1)`。
2. **欧拉角驱动** — 使用 `yaw_`（绕 Z 轴水平旋转）和 `pitch_`（俯仰角）推导朝向向量。pitch 限制在 (-89°, 89°) 避免万向锁。
3. **初始值精确匹配** — 默认 position/yaw/pitch 计算出的 viewMatrix 应与当前硬编码的 `glm::lookAt(2,2,2 → 0,0,0)` 结果一致（允许浮点误差）。
4. **Vulkan Y 翻转** — `projectionMatrix()` 内部执行 `proj[1][1] *= -1`，调用者无需关心。
5. **`lookAt()` 方法** — 给定目标点，反算 yaw/pitch 并调用 `updateVectors()`。用于初始化或脚本控制。
6. **`translate()` 方法** — 接受本地坐标偏移 `(dx, dy, dz)` 分别映射到 `right_`、`up_`、`front_`。阶段 4 中 WASD 键映射到此方法。
7. **不依赖 GLFW** — Camera 是纯数学类，不包含任何输入处理逻辑。

### 3.2 yaw / pitch 初始值推导

当前 lookAt: position = (2,2,2), target = (0,0,0), 方向 = normalize(-1,-1,-1)。

Z-up 下欧拉角定义:
- `front.x = cos(pitch) * cos(yaw)`
- `front.y = cos(pitch) * sin(yaw)`
- `front.z = sin(pitch)`

方向 = normalize(-1,-1,-1) = (-0.5774, -0.5774, -0.5774)

```
yaw   = atan2(front.y, front.x) = atan2(-1, -1) = -135°
pitch = asin(front.z)           = asin(-1/√3)   ≈ -35.264°
```

---

## 四、实施步骤

### 步骤 1：创建 Camera.h

文件位置：`src/scene/Camera.h`

包含上述所有声明。构造函数中调用 `updateVectors()` 初始化 `front_/right_/up_`。

### 步骤 2：创建 Camera.cpp

文件位置：`src/scene/Camera.cpp`

#### updateVectors()

```cpp
void Camera::updateVectors() {
    float yawRad   = glm::radians(yaw_);
    float pitchRad = glm::radians(pitch_);

    front_.x = cos(pitchRad) * cos(yawRad);
    front_.y = cos(pitchRad) * sin(yawRad);
    front_.z = sin(pitchRad);
    front_ = glm::normalize(front_);

    right_ = glm::normalize(glm::cross(front_, worldUp_));
    up_    = glm::normalize(glm::cross(right_, front_));
}
```

#### viewMatrix()

```cpp
glm::mat4 Camera::viewMatrix() const {
    return glm::lookAt(position_, position_ + front_, up_);
}
```

#### projectionMatrix()

```cpp
glm::mat4 Camera::projectionMatrix() const {
    auto proj = glm::perspective(glm::radians(fov_), aspect_, nearPlane_, farPlane_);
    proj[1][1] *= -1;   // Vulkan Y 翻转
    return proj;
}
```

#### lookAt(target)

```cpp
void Camera::lookAt(const glm::vec3& target) {
    glm::vec3 dir = glm::normalize(target - position_);
    pitch_ = glm::degrees(asin(dir.z));
    yaw_   = glm::degrees(atan2(dir.y, dir.x));
    updateVectors();
}
```

#### rotate(yawOffset, pitchOffset)

```cpp
void Camera::rotate(float yawOffset, float pitchOffset) {
    yaw_   += yawOffset;
    pitch_ += pitchOffset;
    pitch_ = glm::clamp(pitch_, -89.0f, 89.0f);
    updateVectors();
}
```

#### translate(localOffset)

```cpp
void Camera::translate(const glm::vec3& localOffset) {
    position_ += right_ * localOffset.x
               + up_    * localOffset.y
               + front_ * localOffset.z;
}
```

### 步骤 3：修改 app.h

- 新增 `#include "scene/Camera.h"`
- 新增成员 `vkr::Camera camera_`
- `updateUniformBuffer` 签名不变

### 步骤 4：修改 app.cpp — initVulkan

在 `scene_.addObject(...)` 之后初始化 Camera（使用默认值即可，已精确匹配当前硬编码参数）：

```cpp
// camera_ 使用默认构造，已匹配当前 lookAt(2,2,2 → 0,0,0)
// 根据窗口大小更新 aspect
camera_.setAspect(swapChain_->extent().width /
                  (float)swapChain_->extent().height);
```

### 步骤 5：修改 app.cpp — updateUniformBuffer

替换硬编码矩阵为 Camera 输出：

```cpp
void HelloTriangleApplication::updateUniformBuffer(uint32_t currentImage) {
    UniformBufferObject ubo{};
    ubo.view = camera_.viewMatrix();
    ubo.proj = camera_.projectionMatrix();
    memcpy(renderer_->mappedUniformBuffer(currentImage), &ubo, sizeof(ubo));
}
```

- 移除 `glm::lookAt(...)` 硬编码
- 移除 `glm::perspective(...)` 硬编码
- 移除 `proj[1][1] *= -1`（已在 Camera 内部处理）

### 步骤 6：构建验证

- 两个 build 目录执行 `cmake ..`（新增 Camera.cpp）
- Release / Debug 编译通过
- 运行程序，渲染画面与阶段 3.4 完全一致（旋转的 viking_room）

---

## 五、变更文件清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `src/scene/Camera.h` | 新建 | Camera 类声明 |
| `src/scene/Camera.cpp` | 新建 | Camera 类实现 |
| `src/app.h` | 修改 | 新增 `#include` 和 `camera_` 成员 |
| `src/app.cpp` | 修改 | `initVulkan` 设置 aspect，`updateUniformBuffer` 使用 Camera |

---

## 六、依赖关系

```
Camera (纯数学类)
  └── glm

App
  ├── Camera         (成员)
  ├── Scene          (成员)
  └── Renderer       (UBO 写入)

updateUniformBuffer:
  camera_.viewMatrix()  ──► UBO.view
  camera_.projectionMatrix() ──► UBO.proj
```

---

## 七、后续展望

Camera 本身没有输入处理逻辑。阶段 4 引入 Window / InputManager 后：

```cpp
// 阶段 4 中的 mainLoop 伪代码
float dt = deltaTime();
if (input.isKeyDown(GLFW_KEY_W)) camera_.translate({0, 0, speed * dt});
if (input.isKeyDown(GLFW_KEY_S)) camera_.translate({0, 0, -speed * dt});
if (input.isKeyDown(GLFW_KEY_A)) camera_.translate({-speed * dt, 0, 0});
if (input.isKeyDown(GLFW_KEY_D)) camera_.translate({speed * dt, 0, 0});
auto [dx, dy] = input.mouseDelta();
camera_.rotate(dx * sensitivity, -dy * sensitivity);
```
