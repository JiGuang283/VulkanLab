#pragma once

#include "MaterialTextureSlot.h"
#include "MaterialSystem.h"
#include "Texture.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class MaterialTemplate;

enum class AlphaMode {
    Opaque,
    Mask,
    Blend,
};

struct MaterialParams {
    std::string debugName;
    glm::vec4 baseColorFactor{1.0f};
    glm::vec3 emissiveFactor{0.0f};
    float     metallicFactor = 1.0f;
    float     roughnessFactor = 1.0f;
    float     alphaCutoff = 0.5f;
    AlphaMode alphaMode = AlphaMode::Opaque;
    float     transmissionFactor = 0.0f;
    float     emissiveStrength = 1.0f;
    float     occlusionStrength = 1.0f;
    uint32_t  occlusionTexCoord = 0;
    float     normalScale = 1.0f;
    float     thicknessFactor = 0.0f;
    glm::vec3 attenuationColor{1.0f};
    float     attenuationDistance = 0.0f;
    bool      doubleSided = false;
};

using MaterialTextureSet =
    MaterialTextureSlotArray<std::shared_ptr<Texture>>;

class MaterialInstance {
  public:
    MaterialInstance(MaterialSystem &materialSystem,
                     std::shared_ptr<MaterialTemplate> materialTemplate,
                     MaterialTextureSet textures,
                     MaterialParams params = {});
    ~MaterialInstance();

    MaterialInstance(const MaterialInstance &) = delete;
    MaterialInstance &operator=(const MaterialInstance &) = delete;

    static MaterialTextureSet makeTextureSet(
        std::shared_ptr<Texture> baseColor,
        const MaterialSystem &materialSystem);

    void bindDescriptors(VkCommandBuffer cmd, VkPipelineLayout layout,
                         uint32_t frameIndex) const;

    VkDescriptorSetLayout descriptorSetLayout() const;

    const MaterialTemplate &materialTemplate() const;
    const MaterialParams &params() const { return params_; }
    const MaterialTextureSet &textures() const { return textures_; }
    MaterialHandle materialHandle() const { return materialHandle_; }
    uint32_t materialIndex() const { return materialHandle_.index; }

  private:
    MaterialSystem *materialSystem_ = nullptr;

    std::shared_ptr<MaterialTemplate> materialTemplate_;
    MaterialTextureSet textures_{};
    MaterialParams params_;
    MaterialHandle materialHandle_{};
};

} // namespace vkr
