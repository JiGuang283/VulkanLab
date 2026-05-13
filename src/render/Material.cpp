#include "Material.h"
#include "MaterialTemplate.h"

#include "core/VulkanCheck.h"

#include <cassert>
#include <utility>

namespace vkr {

Material::Material(Device &device, DescriptorAllocator &descriptorAllocator,
                   std::shared_ptr<MaterialTemplate> materialTemplate,
                   const Texture &texture)
    : device_(&device), descriptorAllocator_(&descriptorAllocator),
      materialTemplate_(std::move(materialTemplate)) {
    assert(materialTemplate_ && "Material requires a MaterialTemplate");
    createDescriptorSets(texture);
    // params_ keeps default factors; baseColor stays null because the legacy
    // ctor does not own the Texture. Scene::render only reads factors.
}

Material::Material(Device &device, DescriptorAllocator &descriptorAllocator,
                   std::shared_ptr<MaterialTemplate> materialTemplate,
                   MaterialParams params)
    : device_(&device), descriptorAllocator_(&descriptorAllocator),
      materialTemplate_(std::move(materialTemplate)),
      params_(std::move(params)) {
    assert(materialTemplate_ && "Material requires a MaterialTemplate");
    assert(params_.baseColor && "MaterialParams.baseColor must not be null");
    createDescriptorSets(*params_.baseColor);
}

Material::~Material() = default;

void Material::bindDescriptors(VkCommandBuffer cmd, VkPipelineLayout layout,
                               uint32_t frameIndex) const {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1,
                            &descriptorSets_[frameIndex], 0, nullptr);
}

VkDescriptorSetLayout Material::descriptorSetLayout() const {
    return materialTemplate_->descriptorSetLayout();
}

const MaterialTemplate &Material::materialTemplate() const {
    return *materialTemplate_;
}

void Material::createDescriptorSets(const Texture &texture) {
    descriptorSets_.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto &set : descriptorSets_)
        set = descriptorAllocator_->allocate(
            materialTemplate_->descriptorSetLayout());

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = texture.imageView();
        imageInfo.sampler = texture.sampler();

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = descriptorSets_[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(device_->logicalDevice(), 1, &descriptorWrite, 0,
                               nullptr);
    }
}

} // namespace vkr
