#pragma once

#include <glm/glm.hpp>

struct GLFWwindow;

namespace vkr {

class Window;

class InputManager {
  public:
    explicit InputManager(Window &window);
    ~InputManager() = default;

    InputManager(const InputManager &) = delete;
    InputManager &operator=(const InputManager &) = delete;

    void update();

    bool isKeyDown(int key) const;

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
