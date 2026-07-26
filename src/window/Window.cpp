#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include "Window.h"

#include <stdexcept>

namespace vkr {

Window::Window(uint32_t width, uint32_t height, const std::string &title,
               bool resizable)
    : width_(width), height_(height) {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, resizable ? GLFW_TRUE : GLFW_FALSE);

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

void Window::resize(uint32_t width, uint32_t height) {
    if (width < 1 || height < 1 || width > 16384 || height > 16384)
        throw std::out_of_range("window size must be in 1..16384");
    width_ = width;
    height_ = height;
    glfwSetWindowSize(window_, static_cast<int>(width),
                      static_cast<int>(height));
}

void *Window::nativeHandle() const {
#ifdef _WIN32
    return glfwGetWin32Window(window_);
#else
    return nullptr;
#endif
}

void Window::setResizeCallback(ResizeCallback cb) {
    resizeCallback_ = std::move(cb);
}

void Window::framebufferResizeCallback(GLFWwindow *w, int width, int height) {
    auto *data = static_cast<WindowUserData *>(glfwGetWindowUserPointer(w));
    if (data->window && width > 0 && height > 0) {
        data->window->width_ = static_cast<uint32_t>(width);
        data->window->height_ = static_cast<uint32_t>(height);
    }
    if (data->window && data->window->resizeCallback_)
        data->window->resizeCallback_(width, height);
}

VkSurfaceKHR Window::createSurface(VkInstance instance) const {
    VkSurfaceKHR surface;
    if (glfwCreateWindowSurface(instance, window_, nullptr, &surface) !=
        VK_SUCCESS) {
        throw std::runtime_error("failed to create window surface!");
    }
    return surface;
}

std::vector<const char *> Window::getRequiredVulkanExtensions() {
    uint32_t     glfwExtensionCount = 0;
    const char **glfwExtensions =
        glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    return {glfwExtensions, glfwExtensions + glfwExtensionCount};
}

VkExtent2D Window::framebufferExtent() const {
    int w, h;
    glfwGetFramebufferSize(window_, &w, &h);
    return {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
}

} // namespace vkr
