#pragma once

#include "Texture.h"
#include "core/DescriptorAllocator.h"
#include "core/FrameSync.h"
#include "core/PipelineConfig.h"

#include <glm/glm.hpp>

#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

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
    /// Material owns only the material texture descriptor set layout. Global
    /// frame descriptors are owned and bound separately by Renderer.
    Material(Device &device, DescriptorAllocator &descriptorAllocator,
             const Texture &texture, const PipelineConfig &config);
    Material(Device &device, DescriptorAllocator &descriptorAllocator,
             MaterialParams params,
             const PipelineConfig &config);
    ~Material();

    Material(const Material &) = delete;
    Material &operator=(const Material &) = delete;

    /// Bind the material descriptor set at set = 1. Set 0 is reserved for
    /// Renderer-owned frame/global descriptors.
    void bindDescriptors(VkCommandBuffer cmd, VkPipelineLayout layout,
                         uint32_t frameIndex) const;

    VkDescriptorSetLayout descriptorSetLayout() const {
        return descriptorSetLayout_;
    }

    const PipelineConfig &pipelineConfig() const { return config_; }

    const MaterialParams &params() const { return params_; }

  private:
    void createDescriptorSetLayout();
    void createDescriptorSets(const Texture &texture);

    Device              *device_ = nullptr;
    DescriptorAllocator *descriptorAllocator_ = nullptr;

    PipelineConfig               config_;
    MaterialParams               params_;
    VkDescriptorSetLayout        descriptorSetLayout_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_;
};

} // namespace vkr
