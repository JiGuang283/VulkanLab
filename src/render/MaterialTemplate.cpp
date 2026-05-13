#include "MaterialTemplate.h"

#include "core/Device.h"
#include "core/VulkanCheck.h"

#include <utility>

namespace vkr {

MaterialTemplate::MaterialTemplate(Device &device, PipelineConfig config)
    : device_(&device), config_(std::move(config)) {
    createDescriptorSetLayout();
    config_.descriptorLayouts.push_back(descriptorSetLayout_);
}

MaterialTemplate::~MaterialTemplate() {
    if (descriptorSetLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_->logicalDevice(),
                                     descriptorSetLayout_, nullptr);
}

void MaterialTemplate::createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding samplerLayoutBinding{};
    samplerLayoutBinding.binding = 0;
    samplerLayoutBinding.descriptorCount = 1;
    samplerLayoutBinding.descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerLayoutBinding.pImmutableSamplers = nullptr;
    samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &samplerLayoutBinding;

    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &layoutInfo,
                                         nullptr, &descriptorSetLayout_));
}

} // namespace vkr
