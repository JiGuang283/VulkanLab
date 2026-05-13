#pragma once

#include "Texture.h"
#include "core/DescriptorAllocator.h"
#include "core/FrameSync.h"

#include <glm/glm.hpp>

#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class MaterialTemplate;

struct MaterialParams {
    std::shared_ptr<Texture> baseColor;
    glm::vec4                baseColorFactor{1.0f};
    glm::vec3                emissiveFactor{0.0f};
    float                    metallicFactor = 1.0f;
    float                    roughnessFactor = 1.0f;
    float                    alphaCutoff = 0.5f;
    bool                     doubleSided = false;
};

class Material {
  public:
    Material(Device &device, DescriptorAllocator &descriptorAllocator,
             std::shared_ptr<MaterialTemplate> materialTemplate,
             const Texture &texture);
    Material(Device &device, DescriptorAllocator &descriptorAllocator,
             std::shared_ptr<MaterialTemplate> materialTemplate,
             MaterialParams params);
    ~Material();

    Material(const Material &) = delete;
    Material &operator=(const Material &) = delete;

    /// Bind the material descriptor set at set = 1. Set 0 is reserved for
    /// Renderer-owned frame/global descriptors.
    void bindDescriptors(VkCommandBuffer cmd, VkPipelineLayout layout,
                         uint32_t frameIndex) const;

    VkDescriptorSetLayout descriptorSetLayout() const;

    const MaterialTemplate &materialTemplate() const;

    const MaterialParams &params() const { return params_; }

  private:
    void createDescriptorSets(const Texture &texture);

    Device              *device_ = nullptr;
    DescriptorAllocator *descriptorAllocator_ = nullptr;

    std::shared_ptr<MaterialTemplate> materialTemplate_;
    MaterialParams               params_;
    std::vector<VkDescriptorSet> descriptorSets_;
};

} // namespace vkr
