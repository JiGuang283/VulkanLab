#include "GuiSystem.h"

namespace vkr {

GuiSystem::GuiSystem(VkInstance, Device &, VkRenderPass, GLFWwindow *,
                     uint32_t, uint32_t) {}

GuiSystem::~GuiSystem() = default;

void GuiSystem::beginFrame() {}

void GuiSystem::render(VkCommandBuffer) {}

void GuiSystem::discardFrame() {}

void GuiSystem::onSwapChainRecreated(uint32_t) {}

bool GuiSystem::wantCaptureMouse() const { return false; }

bool GuiSystem::wantCaptureKeyboard() const { return false; }

} // namespace vkr
