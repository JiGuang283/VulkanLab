#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace vkr {

class InputManager;

struct WindowUserData {
    class Window *window = nullptr;
    InputManager *input = nullptr;
};

class Window {
  public:
    Window(uint32_t width, uint32_t height, const std::string &title);
    ~Window();

    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    bool shouldClose() const;
    void setShouldClose(bool value);
    void pollEvents();

    GLFWwindow *handle() const { return window_; }
    uint32_t    width() const { return width_; }
    uint32_t    height() const { return height_; }

    // Vulkan surface 创建（封装 GLFW 细节）
    VkSurfaceKHR                     createSurface(VkInstance instance) const;
    static std::vector<const char *> getRequiredVulkanExtensions();
    VkExtent2D                       framebufferExtent() const;

    using ResizeCallback = std::function<void(int width, int height)>;
    void setResizeCallback(ResizeCallback cb);

    WindowUserData &userData() { return userData_; }

  private:
    static void framebufferResizeCallback(GLFWwindow *window, int width,
                                          int height);

    GLFWwindow    *window_ = nullptr;
    uint32_t       width_;
    uint32_t       height_;
    ResizeCallback resizeCallback_;
    WindowUserData userData_;
};

} // namespace vkr
