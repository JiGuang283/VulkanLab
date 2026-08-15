#pragma once

#include "render/pipeline/PipelineConfig.h"
#include "render/RenderItem.h"

#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

struct RenderFrameContext;
struct VisibilityFrame;

struct ShadowCasterDrawConfig {
    PipelineRenderingSignature rendering;
    VkDescriptorSetLayout sliceDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorSet sliceDescriptorSet = VK_NULL_HANDLE;
    uint32_t dynamicOffset = 0;
    std::string vertexShader;
    std::string opaqueFragmentShader;
    std::string maskFragmentShader;
    std::string pipelinePrefix;
    bool rasterDepthBias = true;
};

class ShadowCasterDrawRecorder {
  public:
    static void record(const RenderFrameContext &frame,
                       const VisibilityFrame &visibility,
                       const std::vector<RenderItemIndex> &casters,
                       const ShadowCasterDrawConfig &config);
};

} // namespace vkr
