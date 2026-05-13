#include "MaterialTemplate.h"

#include "MaterialTextureSlot.h"
#include "core/Device.h"
#include "core/VulkanCheck.h"

#include <array>
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
    std::array<VkDescriptorSetLayoutBinding, kMaterialTextureSlotCount>
        bindings{};
    for (size_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding = static_cast<uint32_t>(i);
        bindings[i].descriptorCount = 1;
        bindings[i].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].pImmutableSamplers = nullptr;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &layoutInfo,
                                         nullptr, &descriptorSetLayout_));
}

} // namespace vkr
