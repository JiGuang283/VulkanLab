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
    mouseDelta_ = {0.0f, 0.0f};
}

bool InputManager::isKeyDown(int key) const {
    return glfwGetKey(window_, key) == GLFW_PRESS;
}

bool InputManager::isKeyDown(Key key) const {
    return isKeyDown(static_cast<int>(key));
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
    if (!data->input || !data->input->cursorCaptured_)
        return;

    auto *self = data->input;
    if (self->firstMouse_) {
        self->lastMouseX_ = xpos;
        self->lastMouseY_ = ypos;
        self->firstMouse_ = false;
        return;
    }

    self->mouseDelta_.x += static_cast<float>(xpos - self->lastMouseX_);
    self->mouseDelta_.y += static_cast<float>(ypos - self->lastMouseY_);
    self->lastMouseX_ = xpos;
    self->lastMouseY_ = ypos;
}

} // namespace vkr
