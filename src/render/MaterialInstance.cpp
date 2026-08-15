#include "MaterialInstance.h"

#include "MaterialTemplate.h"

#include <cassert>
#include <string>
#include <utility>

namespace vkr {

MaterialInstance::MaterialInstance(
    MaterialSystem &materialSystem,
    std::shared_ptr<MaterialTemplate> materialTemplate,
    MaterialTextureSet textures, MaterialParams params)
    : materialSystem_(&materialSystem),
      materialTemplate_(std::move(materialTemplate)),
      textures_(std::move(textures)), params_(std::move(params)) {
    assert(materialTemplate_ && "MaterialInstance requires a MaterialTemplate");
    for (const auto &texture : textures_)
        assert(texture && "MaterialInstance texture slot must be populated");
    materialHandle_ = materialSystem_->registerMaterial(
        params_, textures_, params_.debugName);
}

MaterialInstance::~MaterialInstance() {
    if (materialSystem_)
        materialSystem_->releaseMaterial(materialHandle_);
}

MaterialTextureSet MaterialInstance::makeTextureSet(
    std::shared_ptr<Texture> baseColor,
    const MaterialSystem &materialSystem) {
    MaterialTextureSet textures{};
    for (size_t i = 0; i < textures.size(); ++i) {
        textures[i] = materialSystem.fallbackTexture(
            static_cast<MaterialTextureSlot>(i));
    }
    textures[indexOf(MaterialTextureSlot::BaseColor)] =
        baseColor ? std::move(baseColor)
                  : materialSystem.fallbackTexture(MaterialTextureSlot::BaseColor);
    return textures;
}

void MaterialInstance::bindDescriptors(VkCommandBuffer cmd,
                                       VkPipelineLayout layout,
                                       uint32_t frameIndex) const {
    (void)frameIndex;
    materialSystem_->bindMaterial(cmd, layout, materialHandle_);
}

VkDescriptorSetLayout MaterialInstance::descriptorSetLayout() const {
    return materialTemplate_->descriptorSetLayout();
}

const MaterialTemplate &MaterialInstance::materialTemplate() const {
    return *materialTemplate_;
}

} // namespace vkr
