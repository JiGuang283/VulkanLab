#pragma once

#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

struct ComputePipelineConfig {
    // Diagnostic-only label. Pipeline cache equality intentionally ignores it.
    std::string debugName;
    std::string computeShaderPath;
    std::vector<VkDescriptorSetLayout> descriptorLayouts;
    std::vector<VkPushConstantRange> pushConstants;
};

} // namespace vkr
