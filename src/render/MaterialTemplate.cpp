#include "MaterialTemplate.h"

#include <stdexcept>
#include <utility>

namespace vkr {

MaterialTemplate::MaterialTemplate(
    PipelineConfig config, VkDescriptorSetLayout descriptorSetLayout)
    : config_(std::move(config)), descriptorSetLayout_(descriptorSetLayout) {
    if (descriptorSetLayout_ == VK_NULL_HANDLE)
        throw std::invalid_argument("Material descriptor layout is null");
    config_.descriptorLayouts.push_back(descriptorSetLayout_);
}

} // namespace vkr
