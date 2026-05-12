#include "Material.h"
#include "Renderer.h"
#include "core/VulkanCheck.h"

#include <array>
#include <cassert>
#include <utility>

namespace vkr {

Material::Material(Device &device, Renderer &renderer,
                   DescriptorAllocator &descriptorAllocator,
                   const Texture &texture, const PipelineConfig &config)
    : device_(&device), renderer_(&renderer),
      descriptorAllocator_(&descriptorAllocator), config_(config) {
    createDescriptorSetLayout();

    // Material owns the descriptor set layout used by this pipeline, so append
    // it to the config it exposes to whoever will build the Pipeline.
    config_.descriptorLayouts.push_back(descriptorSetLayout_);

    createDescriptorSets(texture);
    // params_ keeps default factors; baseColor stays null because the legacy
    // ctor does not own the Texture. Scene::render only reads factors.
}

Material::Material(Device &device, Renderer &renderer,
                   DescriptorAllocator &descriptorAllocator,
                   MaterialParams params, const PipelineConfig &config)
    : device_(&device), renderer_(&renderer),
      descriptorAllocator_(&descriptorAllocator), config_(config),
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
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                            &descriptorSets_[frameIndex], 0, nullptr);
}

void Material::createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding uboLayoutBinding{};
    uboLayoutBinding.binding = 0;
    uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboLayoutBinding.descriptorCount = 1;
    uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    uboLayoutBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutBinding samplerLayoutBinding{};
    samplerLayoutBinding.binding = 1;
    samplerLayoutBinding.descriptorCount = 1;
    samplerLayoutBinding.descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerLayoutBinding.pImmutableSamplers = nullptr;
    samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    std::array<VkDescriptorSetLayoutBinding, 2> bindings = {
        uboLayoutBinding, samplerLayoutBinding};

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &layoutInfo,
                                         nullptr, &descriptorSetLayout_));
}

void Material::createDescriptorSets(const Texture &texture) {
    descriptorSets_.resize(MAX_FRAMES_IN_FLIGHT);
    for (auto &set : descriptorSets_)
        set = descriptorAllocator_->allocate(descriptorSetLayout_);

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = renderer_->uniformBufferHandle(i);
        bufferInfo.offset = 0;
        bufferInfo.range = renderer_->uniformBufferSize();

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = texture.imageView();
        imageInfo.sampler = texture.sampler();

        std::array<VkWriteDescriptorSet, 2> descriptorWrites{};
        descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[0].dstSet = descriptorSets_[i];
        descriptorWrites[0].dstBinding = 0;
        descriptorWrites[0].dstArrayElement = 0;
        descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrites[0].descriptorCount = 1;
        descriptorWrites[0].pBufferInfo = &bufferInfo;

        descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[1].dstSet = descriptorSets_[i];
        descriptorWrites[1].dstBinding = 1;
        descriptorWrites[1].dstArrayElement = 0;
        descriptorWrites[1].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrites[1].descriptorCount = 1;
        descriptorWrites[1].pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(device_->logicalDevice(),
                               static_cast<uint32_t>(descriptorWrites.size()),
                               descriptorWrites.data(), 0, nullptr);
    }
}

} // namespace vkr
