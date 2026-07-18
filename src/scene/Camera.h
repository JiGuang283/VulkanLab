#pragma once

#include <glm/glm.hpp>

namespace vkr {

class Camera {
  public:
    Camera();

    // ---- 投影参数 ----
    void setPerspective(float fovDeg, float aspect, float nearPlane,
                        float farPlane);
    void setClipPlanes(float nearPlane, float farPlane);
    void setAspect(float aspect);

    // ---- 位置与朝向 ----
    void setPosition(const glm::vec3 &pos);
    void setYawPitch(float yaw, float pitch);
    void lookAt(const glm::vec3 &target);

    // ---- FPS 风格控制（阶段 4 接入）----
    void rotate(float yawOffset, float pitchOffset);
    void translate(const glm::vec3 &localOffset);

    // ---- 矩阵输出 ----
    glm::mat4 viewMatrix() const;
    glm::mat4 projectionMatrix() const; // 已含 Vulkan Y 翻转

    // ---- 访问器 ----
    glm::vec3 position() const { return position_; }
    glm::vec3 front() const { return front_; }
    glm::vec3 right() const { return right_; }
    glm::vec3 up() const { return up_; }
    float nearPlane() const { return nearPlane_; }
    float farPlane() const { return farPlane_; }

  private:
    void updateVectors();

    glm::vec3 position_{2.0f, 2.0f, 2.0f};

    float yaw_ = -135.0f;
    float pitch_ = -35.264f;

    glm::vec3 front_;
    glm::vec3 right_;
    glm::vec3 up_;

    glm::vec3 worldUp_{0.0f, 0.0f, 1.0f};

    float fov_ = 45.0f;
    float aspect_ = 800.0f / 600.0f;
    float nearPlane_ = 0.05f;
    float farPlane_ = 200.0f;
};

} // namespace vkr
