#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "Window.h"

#include <stdexcept>

namespace vkr {

Window::Window(uint32_t width, uint32_t height, const std::string &title)
    : width_(width), height_(height) {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    window_ =
        glfwCreateWindow(static_cast<int>(width_), static_cast<int>(height_),
                         title.c_str(), nullptr, nullptr);
    if (!window_)
        throw std::runtime_error("failed to create GLFW window");

    userData_.window = this;
    glfwSetWindowUserPointer(window_, &userData_);
    glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);
}

Window::~Window() {
    glfwDestroyWindow(window_);
    glfwTerminate();
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(window_);
}

void Window::setShouldClose(bool value) {
    glfwSetWindowShouldClose(window_, value ? GLFW_TRUE : GLFW_FALSE);
}

void Window::pollEvents() {
    glfwPollEvents();
}

void Window::setResizeCallback(ResizeCallback cb) {
    resizeCallback_ = std::move(cb);
}

void Window::framebufferResizeCallback(GLFWwindow *w, int width, int height) {
    auto *data = static_cast<WindowUserData *>(glfwGetWindowUserPointer(w));
    if (data->window && data->window->resizeCallback_)
        data->window->resizeCallback_(width, height);
}

} // namespace vkr
