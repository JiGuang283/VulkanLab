#include "Camera.h"

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace vkr {

Camera::Camera() {
    updateVectors();
}

// ---- 投影参数 ----

void Camera::setPerspective(float fovDeg, float aspect, float nearPlane,
                            float farPlane) {
    fov_ = fovDeg;
    aspect_ = aspect;
    nearPlane_ = nearPlane;
    farPlane_ = farPlane;
}

void Camera::setAspect(float aspect) {
    aspect_ = aspect;
}

// ---- 位置与朝向 ----

void Camera::setPosition(const glm::vec3 &pos) {
    position_ = pos;
}

void Camera::lookAt(const glm::vec3 &target) {
    glm::vec3 dir = glm::normalize(target - position_);
    pitch_ = glm::degrees(std::asin(dir.z));
    yaw_ = glm::degrees(std::atan2(dir.y, dir.x));
    updateVectors();
}

// ---- FPS 风格控制 ----

void Camera::rotate(float yawOffset, float pitchOffset) {
    yaw_ += yawOffset;
    pitch_ += pitchOffset;
    pitch_ = glm::clamp(pitch_, -89.0f, 89.0f);
    updateVectors();
}

void Camera::translate(const glm::vec3 &localOffset) {
    position_ +=
        right_ * localOffset.x + up_ * localOffset.y + front_ * localOffset.z;
}

// ---- 矩阵输出 ----

glm::mat4 Camera::viewMatrix() const {
    return glm::lookAt(position_, position_ + front_, up_);
}

glm::mat4 Camera::projectionMatrix() const {
    auto proj =
        glm::perspective(glm::radians(fov_), aspect_, nearPlane_, farPlane_);
    proj[1][1] *= -1; // Vulkan Y 翻转
    return proj;
}

// ---- 内部 ----

void Camera::updateVectors() {
    float yawRad = glm::radians(yaw_);
    float pitchRad = glm::radians(pitch_);

    front_.x = std::cos(pitchRad) * std::cos(yawRad);
    front_.y = std::cos(pitchRad) * std::sin(yawRad);
    front_.z = std::sin(pitchRad);
    front_ = glm::normalize(front_);

    right_ = glm::normalize(glm::cross(front_, worldUp_));
    up_ = glm::normalize(glm::cross(right_, front_));
}

} // namespace vkr
