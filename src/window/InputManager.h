#pragma once

#include <array>
#include <glm/glm.hpp>

struct GLFWwindow;

namespace vkr {

class Window;

enum class Key : int {
    W = 87,          // GLFW_KEY_W
    A = 65,          // GLFW_KEY_A
    S = 83,          // GLFW_KEY_S
    D = 68,          // GLFW_KEY_D
    Q = 81,          // GLFW_KEY_Q
    E = 69,          // GLFW_KEY_E
    Space = 32,      // GLFW_KEY_SPACE
    LeftShift = 340, // GLFW_KEY_LEFT_SHIFT
    Escape = 256,    // GLFW_KEY_ESCAPE
    F12 = 301,       // GLFW_KEY_F12
};

enum class MouseButton : int {
    Left = 0,   // GLFW_MOUSE_BUTTON_LEFT
    Right = 1,  // GLFW_MOUSE_BUTTON_RIGHT
    Middle = 2, // GLFW_MOUSE_BUTTON_MIDDLE
};

class InputManager {
  public:
    explicit InputManager(Window &window);
    ~InputManager() = default;

    InputManager(const InputManager &) = delete;
    InputManager &operator=(const InputManager &) = delete;

    /// 每帧 glfwPollEvents 之后调用一次：刷新边沿触发状态。
    void update();

    /// 帧末调用：清零本帧鼠标增量（在已被消费之后）。
    void endFrame();

    // ---- 键盘 ----
    bool isKeyDown(int key) const;
    bool isKeyDown(Key key) const;
    bool isKeyPressed(Key key) const;  // 本帧刚按下
    bool isKeyReleased(Key key) const; // 本帧刚松开

    // ---- 鼠标按键 ----
    bool isMouseDown(MouseButton b) const;
    bool isMousePressed(MouseButton b) const;
    bool isMouseReleased(MouseButton b) const;

    // ---- 鼠标移动 ----
    /// 仅在光标被捕获时累加（兼容旧行为）。
    glm::vec2 mouseDelta() const { return mouseDelta_; }
    /// 不论捕获与否都累加。
    glm::vec2 rawMouseDelta() const { return rawMouseDelta_; }

    // ---- 光标位置 ----
    glm::dvec2 cursorPos() const;
    void       setCursorPos(glm::dvec2 pos);

    // ---- 捕获 ----
    void setCursorCaptured(bool captured);
    bool isCursorCaptured() const { return cursorCaptured_; }

  private:
    static void mouseCallback(GLFWwindow *window, double xpos, double ypos);

    GLFWwindow *window_ = nullptr;

    glm::vec2 mouseDelta_{0.0f};
    glm::vec2 rawMouseDelta_{0.0f};
    double    lastMouseX_ = 0.0;
    double    lastMouseY_ = 0.0;
    bool      firstMouse_ = true;
    bool      cursorCaptured_ = false;

    // 边沿触发：保存上一帧按键 / 按钮状态
    static constexpr int           kKeyCount = 512;
    static constexpr int           kButtonCount = 8;
    std::array<bool, kKeyCount>    prevKeys_{};
    std::array<bool, kKeyCount>    currKeys_{};
    std::array<bool, kButtonCount> prevButtons_{};
    std::array<bool, kButtonCount> currButtons_{};
};

} // namespace vkr
