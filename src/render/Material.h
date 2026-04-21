#pragma once

#include "Renderer.h"
#include "Texture.h"
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
    float                    metallicFactor  = 1.0f;
    float                    roughnessFactor = 1.0f;
    float                    alphaCutoff     = 0.5f;
    bool                     doubleSided     = false;
};

class Material {
  public:
    /// The caller provides a PipelineConfig **without** descriptorLayouts —
    /// Material creates its own VkDescriptorSetLayout and appends it to the
    /// stored config_.  The Application can then fetch the completed config
    /// via pipelineConfig() to construct its Pipeline object.
    Material(Device &device, Renderer &renderer, const Texture &texture,
             const PipelineConfig &config);
    Material(Device &device, Renderer &renderer, MaterialParams params,
             const PipelineConfig &config);
    ~Material();

    Material(const Material &) = delete;
    Material &operator=(const Material &) = delete;

    /// Bind per-material descriptor set for the given in-flight frame.
    /// The pipeline layout must come from the Pipeline object created with
    /// this Material's pipelineConfig().
    void bindDescriptors(VkCommandBuffer cmd, VkPipelineLayout layout,
                         uint32_t frameIndex) const;

    VkDescriptorSetLayout descriptorSetLayout() const {
        return descriptorSetLayout_;
    }

    const PipelineConfig &pipelineConfig() const { return config_; }

    const MaterialParams &params() const { return params_; }

  private:
    void createDescriptorSetLayout();
    void createDescriptorPool();
    void createDescriptorSets(const Texture &texture);

    Device   *device_ = nullptr;
    Renderer *renderer_ = nullptr;

    PipelineConfig               config_;
    MaterialParams               params_;
    VkDescriptorSetLayout        descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool             descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_;
};

} // namespace vkr
