#include "MaterialInstance.h"

#include "FallbackTextures.h"
#include "MaterialTemplate.h"
#include "core/VulkanCheck.h"

#include <cassert>
#include <string>
#include <utility>

namespace vkr {

MaterialInstance::MaterialInstance(
    Device &device, DescriptorAllocator &descriptorAllocator,
    std::shared_ptr<MaterialTemplate> materialTemplate,
    MaterialTextureSet textures, MaterialParams params)
    : device_(&device), descriptorAllocator_(&descriptorAllocator),
      materialTemplate_(std::move(materialTemplate)),
      textures_(std::move(textures)), params_(std::move(params)) {
    assert(materialTemplate_ && "MaterialInstance requires a MaterialTemplate");
    for (const auto &texture : textures_)
        assert(texture && "MaterialInstance texture slot must be populated");
    try {
        createDescriptorSets();
    } catch (...) {
        for (VkDescriptorSet set : descriptorSets_)
            descriptorAllocator_->free(set);
        descriptorSets_.clear();
        throw;
    }
}

MaterialInstance::~MaterialInstance() {
    if (!descriptorAllocator_)
        return;
    for (VkDescriptorSet set : descriptorSets_)
        descriptorAllocator_->free(set);
}

MaterialTextureSet MaterialInstance::makeTextureSet(
    std::shared_ptr<Texture> baseColor,
    const FallbackTextures &fallbackTextures) {
    MaterialTextureSet textures{};
    for (size_t i = 0; i < textures.size(); ++i) {
        textures[i] = fallbackTextures.textureFor(
            static_cast<MaterialTextureSlot>(i));
    }
    textures[indexOf(MaterialTextureSlot::BaseColor)] =
        baseColor ? std::move(baseColor)
                  : fallbackTextures.textureFor(MaterialTextureSlot::BaseColor);
    return textures;
}

void MaterialInstance::bindDescriptors(VkCommandBuffer cmd,
                                       VkPipelineLayout layout,
                                       uint32_t frameIndex) const {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1,
                            &descriptorSets_[frameIndex], 0, nullptr);
}

VkDescriptorSetLayout MaterialInstance::descriptorSetLayout() const {
    return materialTemplate_->descriptorSetLayout();
}

const MaterialTemplate &MaterialInstance::materialTemplate() const {
    return *materialTemplate_;
}

void MaterialInstance::createDescriptorSets() {
    descriptorSets_.resize(MAX_FRAMES_IN_FLIGHT);
    for (uint32_t frame = 0; frame < descriptorSets_.size(); ++frame) {
        descriptorSets_[frame] = descriptorAllocator_->allocate(
            materialTemplate_->descriptorSetLayout(),
            {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              static_cast<uint32_t>(kMaterialTextureSlotCount)}},
            "Material/" + params_.debugName + "/DescriptorSet/Frame" +
                std::to_string(frame));
    }

    for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame) {
        std::array<VkDescriptorImageInfo, kMaterialTextureSlotCount>
            imageInfos{};
        std::array<VkWriteDescriptorSet, kMaterialTextureSlotCount> writes{};

        for (size_t slot = 0; slot < kMaterialTextureSlotCount; ++slot) {
            const auto &texture = textures_[slot];
            imageInfos[slot].imageLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfos[slot].imageView = texture->imageView();
            imageInfos[slot].sampler = texture->sampler();

            writes[slot].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[slot].dstSet = descriptorSets_[frame];
            writes[slot].dstBinding = static_cast<uint32_t>(slot);
            writes[slot].dstArrayElement = 0;
            writes[slot].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[slot].descriptorCount = 1;
            writes[slot].pImageInfo = &imageInfos[slot];
        }

        vkUpdateDescriptorSets(
            device_->logicalDevice(), static_cast<uint32_t>(writes.size()),
            writes.data(), 0, nullptr);
    }
}

} // namespace vkr
