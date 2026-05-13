#pragma once

#include "MaterialTextureSlot.h"
#include "Texture.h"
#include "core/DescriptorAllocator.h"
#include "core/FrameSync.h"

#include <glm/glm.hpp>

#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class FallbackTextures;
class MaterialTemplate;

struct MaterialParams {
    glm::vec4 baseColorFactor{1.0f};
    glm::vec3 emissiveFactor{0.0f};
    float     metallicFactor = 1.0f;
    float     roughnessFactor = 1.0f;
    float     alphaCutoff = 0.5f;
    bool      doubleSided = false;
};

using MaterialTextureSet =
    MaterialTextureSlotArray<std::shared_ptr<Texture>>;

class MaterialInstance {
  public:
    MaterialInstance(Device &device, DescriptorAllocator &descriptorAllocator,
                     std::shared_ptr<MaterialTemplate> materialTemplate,
                     MaterialTextureSet textures,
                     MaterialParams params = {});
    ~MaterialInstance();

    MaterialInstance(const MaterialInstance &) = delete;
    MaterialInstance &operator=(const MaterialInstance &) = delete;

    static MaterialTextureSet makeTextureSet(
        std::shared_ptr<Texture> baseColor,
        const FallbackTextures &fallbackTextures);

    void bindDescriptors(VkCommandBuffer cmd, VkPipelineLayout layout,
                         uint32_t frameIndex) const;

    VkDescriptorSetLayout descriptorSetLayout() const;

    const MaterialTemplate &materialTemplate() const;
    const MaterialParams &params() const { return params_; }

  private:
    void createDescriptorSets();

    Device              *device_ = nullptr;
    DescriptorAllocator *descriptorAllocator_ = nullptr;

    std::shared_ptr<MaterialTemplate> materialTemplate_;
    MaterialTextureSet textures_{};
    MaterialParams params_;
    std::vector<VkDescriptorSet> descriptorSets_;
};

} // namespace vkr
