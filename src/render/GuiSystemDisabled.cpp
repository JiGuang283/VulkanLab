#include "GuiSystem.h"

namespace vkr {

GuiSystem::GuiSystem(VkInstance, Device &, VkFormat, GLFWwindow *,
                     uint32_t, uint32_t) {}

GuiSystem::~GuiSystem() = default;

void GuiSystem::beginFrame() {}

void GuiSystem::render(VkCommandBuffer) {}

void GuiSystem::discardFrame() {}

void GuiSystem::onSwapChainRecreated(uint32_t) {}

bool GuiSystem::wantCaptureMouse() const { return false; }

bool GuiSystem::wantCaptureKeyboard() const { return false; }

void GuiSystem::setViewportTextures(
    VkSampler,
    const std::array<VkImageView, MAX_FRAMES_IN_FLIGHT> &) {}

void GuiSystem::clearViewportTextures() {}

uint64_t GuiSystem::viewportTextureId(uint32_t) const { return 0; }

} // namespace vkr
