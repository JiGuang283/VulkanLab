#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "InputManager.h"
#include "Window.h"

namespace vkr {

InputManager::InputManager(Window &window) : window_(window.handle()) {
    window.userData().input = this;
    glfwSetCursorPosCallback(window_, mouseCallback);
}

void InputManager::update() {
    // Shift current -> prev, then sample fresh state.
    prevKeys_ = currKeys_;
    prevButtons_ = currButtons_;
    for (int k = 0; k < kKeyCount; ++k)
        currKeys_[k] = glfwGetKey(window_, k) == GLFW_PRESS;
    for (int b = 0; b < kButtonCount; ++b)
        currButtons_[b] = glfwGetMouseButton(window_, b) == GLFW_PRESS;
}

void InputManager::endFrame() {
    mouseDelta_ = {0.0f, 0.0f};
    rawMouseDelta_ = {0.0f, 0.0f};
}

bool InputManager::isKeyDown(int key) const {
    return glfwGetKey(window_, key) == GLFW_PRESS;
}

bool InputManager::isKeyDown(Key key) const {
    return isKeyDown(static_cast<int>(key));
}

bool InputManager::isKeyPressed(Key key) const {
    const int k = static_cast<int>(key);
    if (k < 0 || k >= kKeyCount)
        return false;
    return currKeys_[k] && !prevKeys_[k];
}

bool InputManager::isKeyReleased(Key key) const {
    const int k = static_cast<int>(key);
    if (k < 0 || k >= kKeyCount)
        return false;
    return !currKeys_[k] && prevKeys_[k];
}

bool InputManager::isMouseDown(MouseButton b) const {
    return glfwGetMouseButton(window_, static_cast<int>(b)) == GLFW_PRESS;
}

bool InputManager::isMousePressed(MouseButton b) const {
    const int i = static_cast<int>(b);
    if (i < 0 || i >= kButtonCount)
        return false;
    return currButtons_[i] && !prevButtons_[i];
}

bool InputManager::isMouseReleased(MouseButton b) const {
    const int i = static_cast<int>(b);
    if (i < 0 || i >= kButtonCount)
        return false;
    return !currButtons_[i] && prevButtons_[i];
}

glm::dvec2 InputManager::cursorPos() const {
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(window_, &x, &y);
    return {x, y};
}

void InputManager::setCursorPos(glm::dvec2 pos) {
    glfwSetCursorPos(window_, pos.x, pos.y);
    lastMouseX_ = pos.x;
    lastMouseY_ = pos.y;
}

void InputManager::setCursorCaptured(bool captured) {
    cursorCaptured_ = captured;
    glfwSetInputMode(window_, GLFW_CURSOR,
                     captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (captured)
        firstMouse_ = true;
}

void InputManager::mouseCallback(GLFWwindow *w, double xpos, double ypos) {
    auto *data = static_cast<WindowUserData *>(glfwGetWindowUserPointer(w));
    if (!data || !data->input)
        return;

    auto *self = data->input;
    if (self->firstMouse_) {
        self->lastMouseX_ = xpos;
        self->lastMouseY_ = ypos;
        self->firstMouse_ = false;
        return;
    }

    const float dx = static_cast<float>(xpos - self->lastMouseX_);
    const float dy = static_cast<float>(ypos - self->lastMouseY_);
    self->rawMouseDelta_.x += dx;
    self->rawMouseDelta_.y += dy;
    if (self->cursorCaptured_) {
        self->mouseDelta_.x += dx;
        self->mouseDelta_.y += dy;
    }
    self->lastMouseX_ = xpos;
    self->lastMouseY_ = ypos;
}

} // namespace vkr
