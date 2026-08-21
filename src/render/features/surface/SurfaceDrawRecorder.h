#pragma once

#include "render/shader/ShaderTypes.h"

#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class RenderResourcePool;
class SurfaceFrameData;
struct RenderFrameContext;
struct VisibilityFrame;

struct SurfaceDrawRecordConfig {
    std::string debugName;
    MaterialShaderPass opaquePass = MaterialShaderPass::SurfaceOpaque;
    MaterialShaderPass maskPass = MaterialShaderPass::SurfaceMask;
    std::vector<VkFormat> colorAttachmentFormats;
    VkFormat depthAttachmentFormat = VK_FORMAT_UNDEFINED;
    bool useVisibilityIndirect = false;
};

void recordSurfaceDraws(const RenderFrameContext &frame,
                        const RenderResourcePool &resources,
                        const VisibilityFrame &visibility,
                        const SurfaceFrameData &frameData,
                        const SurfaceDrawRecordConfig &config);

} // namespace vkr
