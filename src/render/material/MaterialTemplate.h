#pragma once

#include "render/pipeline/PipelineConfig.h"
#include "render/shader/ShaderTypes.h"

#include <vulkan/vulkan.h>

namespace vkr {

class MaterialTemplate {
  public:
    MaterialTemplate(PipelineConfig config,
                     VkDescriptorSetLayout descriptorSetLayout,
                     MaterialShaderFamilyHandle shaderFamily);
    ~MaterialTemplate() = default;

    MaterialTemplate(const MaterialTemplate &) = delete;
    MaterialTemplate &operator=(const MaterialTemplate &) = delete;

    VkDescriptorSetLayout descriptorSetLayout() const {
        return descriptorSetLayout_;
    }

    const PipelineConfig &pipelineConfig() const { return config_; }
    MaterialShaderFamilyHandle shaderFamily() const { return shaderFamily_; }

  private:
    PipelineConfig        config_;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    MaterialShaderFamilyHandle shaderFamily_{};
};

} // namespace vkr
