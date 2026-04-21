#pragma once

#include <glm/glm.hpp>

struct GLFWwindow;

namespace vkr {

class Window;

enum class Key : int {
    W = 87,          // GLFW_KEY_W
    A = 65,          // GLFW_KEY_A
    S = 83,          // GLFW_KEY_S
    D = 68,          // GLFW_KEY_D
    Space = 32,      // GLFW_KEY_SPACE
    LeftShift = 340, // GLFW_KEY_LEFT_SHIFT
    Escape = 256,    // GLFW_KEY_ESCAPE
};

class InputManager {
  public:
    explicit InputManager(Window &window);
    ~InputManager() = default;

    InputManager(const InputManager &) = delete;
    InputManager &operator=(const InputManager &) = delete;

    void update();

    bool isKeyDown(int key) const;
    bool isKeyDown(Key key) const;

    glm::vec2 mouseDelta() const { return mouseDelta_; }

    void setCursorCaptured(bool captured);
    bool isCursorCaptured() const { return cursorCaptured_; }

  private:
    static void mouseCallback(GLFWwindow *window, double xpos, double ypos);

    GLFWwindow *window_ = nullptr;

    glm::vec2 mouseDelta_{0.0f};
    double    lastMouseX_ = 0.0;
    double    lastMouseY_ = 0.0;
    bool      firstMouse_ = true;
    bool      cursorCaptured_ = false;
};

} // namespace vkr
