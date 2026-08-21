#include "MaterialTemplate.h"

#include <stdexcept>
#include <utility>

namespace vkr {

MaterialTemplate::MaterialTemplate(
    PipelineConfig config, VkDescriptorSetLayout descriptorSetLayout,
    MaterialShaderFamilyHandle shaderFamily)
    : config_(std::move(config)), descriptorSetLayout_(descriptorSetLayout),
      shaderFamily_(shaderFamily) {
    if (descriptorSetLayout_ == VK_NULL_HANDLE)
        throw std::invalid_argument("Material descriptor layout is null");
    if (!shaderFamily_.valid())
        throw std::invalid_argument("Material shader family is invalid");
    config_.descriptorLayouts.push_back(descriptorSetLayout_);
}

} // namespace vkr
