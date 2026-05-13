#pragma once

#include "core/PipelineConfig.h"

#include <vulkan/vulkan.h>

namespace vkr {

class Device;

class MaterialTemplate {
  public:
    MaterialTemplate(Device &device, PipelineConfig config);
    ~MaterialTemplate();

    MaterialTemplate(const MaterialTemplate &) = delete;
    MaterialTemplate &operator=(const MaterialTemplate &) = delete;

    VkDescriptorSetLayout descriptorSetLayout() const {
        return descriptorSetLayout_;
    }

    const PipelineConfig &pipelineConfig() const { return config_; }

  private:
    void createDescriptorSetLayout();

    Device               *device_ = nullptr;
    PipelineConfig        config_;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
};

} // namespace vkr
