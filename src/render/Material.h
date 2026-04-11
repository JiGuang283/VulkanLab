#pragma once

#include "Renderer.h"
#include "Texture.h"
#include "core/Pipeline.h"

#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Material {
  public:
    Material(Device &device, Renderer &renderer, const Texture &texture,
             const std::string &vertShader, const std::string &fragShader);
    ~Material();

    Material(const Material &) = delete;
    Material &operator=(const Material &) = delete;

    void bind(VkCommandBuffer cmd, uint32_t frameIndex) const;

    VkPipelineLayout pipelineLayout() const { return pipeline_->layout(); }

  private:
    void createDescriptorSetLayout();
    void createDescriptorPool();
    void createDescriptorSets(const Texture &texture);

    Device   *device_ = nullptr;
    Renderer *renderer_ = nullptr;

    std::unique_ptr<Pipeline>    pipeline_;
    VkDescriptorSetLayout        descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool             descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_;
};

} // namespace vkr
