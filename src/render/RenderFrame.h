#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

namespace vkr {

class GuiSystem;
class Pipeline;

struct RenderFrameContext {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint32_t        frameIndex = 0;
    uint32_t        imageIndex = 0;
    VkExtent2D      extent{};
    Pipeline       *opaquePipeline = nullptr;
    GuiSystem      *gui = nullptr;
};

} // namespace vkr
