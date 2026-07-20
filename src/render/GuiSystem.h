#pragma once

#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace vkr {

class Device;

/// Dear ImGui integration (Vulkan + GLFW backends).
/// Draws into the application's main render pass as the last step before
/// endRenderPass, so no extra pass or attachment is required.
class GuiSystem {
  public:
    GuiSystem(VkInstance instance, Device &device, VkRenderPass renderPass,
              GLFWwindow *window, uint32_t minImageCount, uint32_t imageCount);
    ~GuiSystem();

    GuiSystem(const GuiSystem &) = delete;
    GuiSystem &operator=(const GuiSystem &) = delete;

    /// Begin a new ImGui frame.  Call once per frame before any ImGui:: calls.
    void beginFrame();

    /// End the ImGui frame and record its draw commands into the given cmd.
    /// Must be called while a compatible render pass is active.
    void render(VkCommandBuffer cmd);

    /// End the current ImGui frame without producing draw commands.
    void discardFrame();

    /// After swap chain recreation, update the backend's image count.
    void onSwapChainRecreated(uint32_t minImageCount);

    /// Whether ImGui is currently consuming mouse / keyboard input.
    bool wantCaptureMouse() const;
    bool wantCaptureKeyboard() const;

  private:
    Device          *device_ = nullptr;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
};

} // namespace vkr
