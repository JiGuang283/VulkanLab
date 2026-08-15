#include "render/material/MaterialSystem.h"

#include "core/Buffer.h"
#include "core/DescriptorAllocator.h"
#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/UploadContext.h"
#include "core/VulkanCheck.h"
#include "render/material/MaterialInstance.h"
#include "render/material/Texture.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace vkr {
namespace {

std::shared_ptr<Texture> makeSolidTexture(Device &device,
                                          UploadContext &upload,
                                          std::array<uint8_t, 4> rgba,
                                          VkFormat format,
                                          std::string debugName) {
    TextureCreateInfo info{};
    info.pixels = rgba.data();
    info.width = 1;
    info.height = 1;
    info.generateMipmaps = false;
    info.format = format;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.debugName = std::move(debugName);
    return std::make_shared<Texture>(device, upload, info);
}

GpuMaterial makeGpuMaterial(
    const MaterialParams &params,
    const MaterialTextureSlotArray<TextureSlotHandle> &textures) {
    GpuMaterial result{};
    result.baseColorFactor = params.baseColorFactor;
    result.emissiveMetallic = {
        params.emissiveFactor * params.emissiveStrength,
        params.metallicFactor};
    result.roughnessAlphaOcclusionNormal = {
        params.roughnessFactor, params.alphaCutoff,
        params.occlusionStrength, params.normalScale};
    result.transmissionVolume = {
        params.transmissionFactor, params.thicknessFactor,
        params.attenuationDistance, 0.0f};
    result.attenuationColor = {params.attenuationColor, 0.0f};
    result.textureIndices0 = {
        textures[indexOf(MaterialTextureSlot::BaseColor)].index,
        textures[indexOf(MaterialTextureSlot::Normal)].index,
        textures[indexOf(MaterialTextureSlot::MetallicRoughness)].index,
        textures[indexOf(MaterialTextureSlot::Occlusion)].index};
    result.textureIndices1 = {
        textures[indexOf(MaterialTextureSlot::Emissive)].index,
        params.occlusionTexCoord, static_cast<uint32_t>(params.alphaMode),
        params.doubleSided ? 1u : 0u};
    return result;
}

} // namespace

MaterialSystem::MaterialSystem(Device &device,
                               DescriptorAllocator &descriptorAllocator,
                               MaterialBindingMode requested,
                               bool shaderManifestSupported)
    : device_(&device), descriptorAllocator_(&descriptorAllocator) {
    const MaterialBindingDeviceSupport &deviceSupport =
        device.materialBindingSupport();
    status_.requested = requested;
    status_.deviceSupported = deviceSupport.supported;
    status_.shaderManifestSupported = shaderManifestSupported;
    status_.active = MaterialBindingMode::Legacy;
    if (requested != MaterialBindingMode::Legacy && deviceSupport.supported &&
        shaderManifestSupported) {
        status_.active = MaterialBindingMode::Bindless;
    } else if (requested == MaterialBindingMode::Bindless) {
        throw std::runtime_error(
            !deviceSupport.supported
                ? "Bindless material binding is unavailable: " +
                      deviceSupport.reason
                : "Shader manifest does not contain bindless material variants");
    } else if (requested == MaterialBindingMode::Auto) {
        status_.fallbackReason =
            !deviceSupport.supported
                ? deviceSupport.reason
                : "shader manifest does not contain bindless material variants";
    }

    status_.textureCapacity =
        status_.active == MaterialBindingMode::Bindless
            ? deviceSupport.textureCapacity
            : 8192u;
    status_.materialCapacity = deviceSupport.materialCapacity;
    if (status_.materialCapacity < 4096)
        throw std::runtime_error(
            "Material storage buffer capacity is below 4096");

    textureRecords_.resize(status_.textureCapacity);
    materialRecords_.resize(status_.materialCapacity);
    constexpr VmaAllocationCreateFlags mappedFlags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT;
    materialBuffer_ = std::make_unique<Buffer>(
        device, sizeof(GpuMaterial) * status_.materialCapacity,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, mappedFlags,
        "Material/GlobalGpuMaterialTable");
    mappedMaterials_ =
        static_cast<GpuMaterial *>(materialBuffer_->map());
    std::memset(mappedMaterials_, 0,
                sizeof(GpuMaterial) * status_.materialCapacity);

    createLayouts();
    if (status_.active == MaterialBindingMode::Bindless)
        createBindlessSet();
    createFallbackResources();
}

MaterialSystem::~MaterialSystem() {
    if (!device_)
        return;
    for (MaterialRecord &record : materialRecords_) {
        if (record.legacySet != VK_NULL_HANDLE)
            descriptorAllocator_->free(record.legacySet);
    }
    if (bindlessPool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(device_->logicalDevice(), bindlessPool_,
                                nullptr);
    if (layout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_->logicalDevice(), layout_,
                                     nullptr);
    fallbackTextures_ = {};
    materialBuffer_.reset();
}

void MaterialSystem::createLayouts() {
    if (status_.active == MaterialBindingMode::Bindless) {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                       status_.textureCapacity,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        std::array<VkDescriptorBindingFlags, 2> flags{
            0,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
                VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
        VkDescriptorSetLayoutBindingFlagsCreateInfo flagInfo{};
        flagInfo.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flagInfo.bindingCount = static_cast<uint32_t>(flags.size());
        flagInfo.pBindingFlags = flags.data();
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.flags =
            VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        info.pNext = &flagInfo;
        VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                             nullptr, &layout_));
    } else {
        std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        for (uint32_t slot = 0; slot < kMaterialTextureSlotCount; ++slot) {
            bindings[slot + 1] = {
                slot + 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        VK_CHECK(vkCreateDescriptorSetLayout(device_->logicalDevice(), &info,
                                             nullptr, &layout_));
    }
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, layout_,
        status_.active == MaterialBindingMode::Bindless
            ? "Material/BindlessLayout"
            : "Material/LegacyLayout");
}

void MaterialSystem::createBindlessSet() {
    std::array<VkDescriptorPoolSize, 2> sizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         status_.textureCapacity},
    }};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    VK_CHECK(vkCreateDescriptorPool(device_->logicalDevice(), &poolInfo,
                                    nullptr, &bindlessPool_));
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = bindlessPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout_;
    VK_CHECK(vkAllocateDescriptorSets(device_->logicalDevice(), &allocInfo,
                                      &bindlessSet_));
    const VkDescriptorBufferInfo bufferInfo{
        materialBuffer_->handle(), 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = bindlessSet_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(device_->logicalDevice(), 1, &write, 0, nullptr);
    ++status_.descriptorWrites;
    device_->debugUtils().setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_POOL,
                                        bindlessPool_,
                                        "Material/BindlessPool");
    device_->debugUtils().setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET,
                                        bindlessSet_,
                                        "Material/BindlessSet");
}

void MaterialSystem::createFallbackResources() {
    UploadContext upload(*device_, nullptr, 1024,
                         "Material/FallbackUpload");
    fallbackTextures_[0] = makeSolidTexture(
        *device_, upload, {255, 255, 255, 255},
        VK_FORMAT_R8G8B8A8_SRGB, "Material/Fallback/WhiteSrgb");
    fallbackTextures_[1] = makeSolidTexture(
        *device_, upload, {255, 255, 255, 255},
        VK_FORMAT_R8G8B8A8_UNORM, "Material/Fallback/WhiteLinear");
    fallbackTextures_[2] = makeSolidTexture(
        *device_, upload, {0, 0, 0, 255}, VK_FORMAT_R8G8B8A8_SRGB,
        "Material/Fallback/BlackSrgb");
    fallbackTextures_[3] = makeSolidTexture(
        *device_, upload, {128, 128, 255, 255},
        VK_FORMAT_R8G8B8A8_UNORM, "Material/Fallback/FlatNormal");
    upload.finish();

    for (uint32_t index = 0; index < fallbackTextures_.size(); ++index) {
        TextureRecord &record = textureRecords_[index];
        record.generation = 1;
        record.references = std::numeric_limits<uint32_t>::max();
        record.active = true;
        record.texture = fallbackTextures_[index];
        textureLookup_[record.texture.get()] = index;
        if (status_.active == MaterialBindingMode::Bindless)
            writeBindlessTexture(index, *record.texture);
    }
    status_.activeTextures = static_cast<uint32_t>(fallbackTextures_.size());
    status_.textureHighWaterMark = status_.activeTextures;

    MaterialRecord &fallback = materialRecords_[0];
    fallback.generation = 1;
    fallback.active = true;
    fallback.textures = {
        TextureSlotHandle{0, 1}, TextureSlotHandle{3, 1},
        TextureSlotHandle{1, 1}, TextureSlotHandle{1, 1},
        TextureSlotHandle{2, 1}};
    MaterialParams params{};
    writeMaterial(0, makeGpuMaterial(params, fallback.textures));
    if (status_.active == MaterialBindingMode::Legacy) {
        const MaterialTextureSlotArray<std::shared_ptr<Texture>> textures{
            fallbackTextures_[0], fallbackTextures_[3],
            fallbackTextures_[1], fallbackTextures_[1],
            fallbackTextures_[2]};
        fallback.legacySet =
            createLegacySet(0, textures, "Fallback");
    }
    status_.activeMaterials = 1;
    status_.materialHighWaterMark = 1;
}

uint32_t MaterialSystem::allocateTextureIndex() {
    if (!freeTextureSlots_.empty()) {
        const uint32_t index = freeTextureSlots_.back();
        freeTextureSlots_.pop_back();
        ++status_.textureSlotReuses;
        return index;
    }
    for (uint32_t index = 4; index < textureRecords_.size(); ++index) {
        if (textureRecords_[index].generation == 0)
            return index;
    }
    ++status_.textureCapacityFailures;
    throw std::runtime_error("bindless_texture_capacity_exhausted");
}

uint32_t MaterialSystem::allocateMaterialIndex() {
    if (!freeMaterialSlots_.empty()) {
        const uint32_t index = freeMaterialSlots_.back();
        freeMaterialSlots_.pop_back();
        ++status_.materialSlotReuses;
        return index;
    }
    for (uint32_t index = 1; index < materialRecords_.size(); ++index) {
        if (materialRecords_[index].generation == 0)
            return index;
    }
    ++status_.materialCapacityFailures;
    throw std::runtime_error("material_capacity_exhausted");
}

TextureSlotHandle MaterialSystem::acquireTexture(
    const std::shared_ptr<Texture> &texture) {
    if (!texture)
        throw std::invalid_argument("Material texture cannot be null");
    const auto found = textureLookup_.find(texture.get());
    if (found != textureLookup_.end()) {
        TextureRecord &record = textureRecords_[found->second];
        if (record.retiring) {
            record.retiring = false;
            record.active = true;
            --status_.retiringTextures;
            ++status_.activeTextures;
        }
        if (record.references != std::numeric_limits<uint32_t>::max())
            ++record.references;
        return {found->second, record.generation};
    }
    const uint32_t index = allocateTextureIndex();
    TextureRecord &record = textureRecords_[index];
    record.generation = std::max(1u, record.generation + 1u);
    record.references = 1;
    record.active = true;
    record.retiring = false;
    record.texture = texture;
    textureLookup_[texture.get()] = index;
    if (status_.active == MaterialBindingMode::Bindless)
        writeBindlessTexture(index, *texture);
    ++status_.activeTextures;
    status_.textureHighWaterMark =
        std::max(status_.textureHighWaterMark, status_.activeTextures);
    return {index, record.generation};
}

MaterialHandle MaterialSystem::registerMaterial(
    const MaterialParams &params,
    const MaterialTextureSlotArray<std::shared_ptr<Texture>> &textures,
    std::string_view debugName) {
    MaterialTextureSlotArray<TextureSlotHandle> textureHandles{};
    size_t acquired = 0;
    uint32_t materialIndex = std::numeric_limits<uint32_t>::max();
    try {
        for (; acquired < textures.size(); ++acquired)
            textureHandles[acquired] = acquireTexture(textures[acquired]);
        materialIndex = allocateMaterialIndex();
        MaterialRecord &record = materialRecords_[materialIndex];
        record.generation = std::max(1u, record.generation + 1u);
        record.active = true;
        record.retiring = false;
        record.textures = textureHandles;
        writeMaterial(materialIndex, makeGpuMaterial(params, textureHandles));
        if (status_.active == MaterialBindingMode::Legacy) {
            record.legacySet =
                createLegacySet(materialIndex, textures, debugName);
        }
        ++status_.activeMaterials;
        status_.materialHighWaterMark =
            std::max(status_.materialHighWaterMark,
                     status_.activeMaterials);
        return {materialIndex, record.generation};
    } catch (...) {
        if (materialIndex != std::numeric_limits<uint32_t>::max()) {
            MaterialRecord &record = materialRecords_[materialIndex];
            if (record.legacySet != VK_NULL_HANDLE)
                descriptorAllocator_->free(record.legacySet);
            record.legacySet = VK_NULL_HANDLE;
            record.active = false;
            record.retiring = false;
            record.textures = {};
            freeMaterialSlots_.push_back(materialIndex);
        }
        for (size_t index = 0; index < acquired; ++index)
            releaseTexture(textureHandles[index], completedSerial_);
        collectGarbage(completedSerial_);
        throw;
    }
}

void MaterialSystem::releaseMaterial(MaterialHandle handle) {
    if (handle.index == 0 || handle.index >= materialRecords_.size())
        return;
    MaterialRecord &record = materialRecords_[handle.index];
    if (!record.active || record.generation != handle.generation)
        return;
    record.active = false;
    record.retiring = true;
    record.retireAfterSerial = lastSubmittedSerial_;
    --status_.activeMaterials;
    ++status_.retiringMaterials;
}

void MaterialSystem::releaseTexture(TextureSlotHandle handle,
                                    uint64_t retireSerial) {
    if (handle.index < fallbackTextures_.size() ||
        handle.index >= textureRecords_.size())
        return;
    TextureRecord &record = textureRecords_[handle.index];
    if (!record.active || record.generation != handle.generation ||
        record.references == 0)
        return;
    --record.references;
    if (record.references == 0) {
        record.active = false;
        record.retiring = true;
        record.retireAfterSerial = retireSerial;
        --status_.activeTextures;
        ++status_.retiringTextures;
    }
}

void MaterialSystem::updateSubmissionSerials(uint64_t lastSubmitted,
                                             uint64_t completed) {
    lastSubmittedSerial_ = lastSubmitted;
    completedSerial_ = completed;
    collectGarbage(completed);
}

void MaterialSystem::collectGarbage(uint64_t completedSerial) {
    for (uint32_t index = 1; index < materialRecords_.size(); ++index) {
        MaterialRecord &record = materialRecords_[index];
        if (!record.retiring || record.retireAfterSerial > completedSerial)
            continue;
        if (record.legacySet != VK_NULL_HANDLE) {
            descriptorAllocator_->free(record.legacySet);
            record.legacySet = VK_NULL_HANDLE;
        }
        for (TextureSlotHandle texture : record.textures)
            releaseTexture(texture, record.retireAfterSerial);
        record.retiring = false;
        freeMaterialSlots_.push_back(index);
        --status_.retiringMaterials;
    }
    for (uint32_t index = 4; index < textureRecords_.size(); ++index) {
        TextureRecord &record = textureRecords_[index];
        if (!record.retiring || record.retireAfterSerial > completedSerial)
            continue;
        textureLookup_.erase(record.texture.get());
        record.texture.reset();
        record.retiring = false;
        freeTextureSlots_.push_back(index);
        --status_.retiringTextures;
    }
}

void MaterialSystem::bindGlobal(VkCommandBuffer commandBuffer,
                                VkPipelineLayout layout) const {
    if (status_.active != MaterialBindingMode::Bindless)
        return;
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            layout, 1, 1, &bindlessSet_, 0, nullptr);
}

void MaterialSystem::bindMaterial(VkCommandBuffer commandBuffer,
                                  VkPipelineLayout layout,
                                  MaterialHandle handle) const {
    if (status_.active == MaterialBindingMode::Bindless)
        return;
    if (handle.index >= materialRecords_.size())
        handle = {0, 1};
    const MaterialRecord &record = materialRecords_[handle.index];
    if (record.generation != handle.generation ||
        record.legacySet == VK_NULL_HANDLE)
        throw std::runtime_error("Legacy material descriptor is unavailable");
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            layout, 1, 1, &record.legacySet, 0, nullptr);
}

std::shared_ptr<Texture>
MaterialSystem::fallbackTexture(MaterialTextureSlot slot) const {
    switch (slot) {
    case MaterialTextureSlot::BaseColor:
        return fallbackTextures_[0];
    case MaterialTextureSlot::MetallicRoughness:
    case MaterialTextureSlot::Occlusion:
        return fallbackTextures_[1];
    case MaterialTextureSlot::Emissive:
        return fallbackTextures_[2];
    case MaterialTextureSlot::Normal:
        return fallbackTextures_[3];
    case MaterialTextureSlot::Count:
        break;
    }
    return fallbackTextures_[0];
}

VkDescriptorSet MaterialSystem::createLegacySet(
    uint32_t materialIndex,
    const MaterialTextureSlotArray<std::shared_ptr<Texture>> &textures,
    std::string_view debugName) {
    VkDescriptorSet set = descriptorAllocator_->allocate(
        layout_, {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                   static_cast<uint32_t>(kMaterialTextureSlotCount)}},
        "Material/" + std::string(debugName) + "/DescriptorSet");
    const VkDescriptorBufferInfo bufferInfo{
        materialBuffer_->handle(), 0, VK_WHOLE_SIZE};
    std::array<VkDescriptorImageInfo, kMaterialTextureSlotCount> images{};
    std::array<VkWriteDescriptorSet, 1 + kMaterialTextureSlotCount> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &bufferInfo;
    for (uint32_t slot = 0; slot < kMaterialTextureSlotCount; ++slot) {
        images[slot] = {textures[slot]->sampler(),
                        textures[slot]->imageView(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[slot + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[slot + 1].dstSet = set;
        writes[slot + 1].dstBinding = slot + 1;
        writes[slot + 1].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[slot + 1].descriptorCount = 1;
        writes[slot + 1].pImageInfo = &images[slot];
    }
    vkUpdateDescriptorSets(device_->logicalDevice(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
    status_.descriptorWrites += writes.size();
    (void)materialIndex;
    return set;
}

void MaterialSystem::writeMaterial(uint32_t index,
                                   const GpuMaterial &material) {
    mappedMaterials_[index] = material;
    materialBuffer_->flush(sizeof(GpuMaterial) * index,
                           sizeof(GpuMaterial));
}

void MaterialSystem::writeBindlessTexture(uint32_t index,
                                          const Texture &texture) {
    const VkDescriptorImageInfo imageInfo{
        texture.sampler(), texture.imageView(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = bindlessSet_;
    write.dstBinding = 1;
    write.dstArrayElement = index;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device_->logicalDevice(), 1, &write, 0, nullptr);
    ++status_.descriptorWrites;
}

const GpuMaterial *MaterialSystem::gpuMaterial(MaterialHandle handle) const {
    if (handle.index >= materialRecords_.size())
        return nullptr;
    const MaterialRecord &record = materialRecords_[handle.index];
    return record.generation == handle.generation
               ? &mappedMaterials_[handle.index]
               : nullptr;
}

} // namespace vkr
