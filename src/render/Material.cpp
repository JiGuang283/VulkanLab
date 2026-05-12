#include "Material.h"
#include "core/VulkanCheck.h"

#include <cassert>
#include <utility>

namespace vkr {

Material::Material(Device &device, DescriptorAllocator &descriptorAllocator,
                   const Texture &texture, const PipelineConfig &config)
    : device_(&device), descriptorAllocator_(&descriptorAllocator),
      config_(config) {
    createDescriptorSetLayout();

    // Material contributes set 1. Renderer-owned global descriptors are
    // inserted at set 0 when Application creates the pipeline.
    config_.descriptorLayouts.push_back(descriptorSetLayout_);

    createDescriptorSets(texture);
    // params_ keeps default factors; baseColor stays null because the legacy
    // ctor does not own the Texture. Scene::render only reads factors.
}

Material::Material(Device &device, DescriptorAllocator &descriptorAllocator,
                   MaterialParams params, const PipelineConfig &config)
    : device_(&device), descriptorAllocator_(&descriptorAllocator), config_(config),
      params_(std::move(params)) {
    assert(params_.baseColor && "MaterialParams.baseColor must not be null");
    createDescriptorSetLayout();
    config_.descriptorLayouts.push_back(descriptorSetLayout_);
    createDescriptorSets(*params_.baseColor);
}

Material::~Material() {
    VkDevice d = device_->logicalDevice();
    vkDestroyDescriptorSetLayout(d, descriptorSetLayout_, nullptr);
}

void Material::bindDescriptors(VkCommandBuffer cmd, VkPipelineLayout layout,
                               uint32_t frameIndex) const {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1,
                            &descriptorSets_[frameIndex], 0, nullptr);
}

void Material::createDescriptorSetLayout() {
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

void Material::createDescriptorSets(const Texture &texture) {
    descriptorSets_.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto &set : descriptorSets_)
        set = descriptorAllocator_->allocate(descriptorSetLayout_);

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
