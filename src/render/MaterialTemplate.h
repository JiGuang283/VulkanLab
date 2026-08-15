#pragma once

#include "core/PipelineConfig.h"

#include <vulkan/vulkan.h>

namespace vkr {

class MaterialTemplate {
  public:
    MaterialTemplate(PipelineConfig config,
                     VkDescriptorSetLayout descriptorSetLayout);
    ~MaterialTemplate() = default;

    MaterialTemplate(const MaterialTemplate &) = delete;
    MaterialTemplate &operator=(const MaterialTemplate &) = delete;

    VkDescriptorSetLayout descriptorSetLayout() const {
        return descriptorSetLayout_;
    }

    const PipelineConfig &pipelineConfig() const { return config_; }

  private:
    PipelineConfig        config_;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
};

} // namespace vkr
