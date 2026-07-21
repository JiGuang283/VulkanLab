#pragma once

#include "PipelineConfig.h"

namespace vkr {

class PipelineConfigBuilder {
  public:
    PipelineConfigBuilder &shaders(std::string vertPath, std::string fragPath);
    PipelineConfigBuilder &vertexLayout(VertexLayout layout);
    PipelineConfigBuilder &defaultVertexLayout();
    PipelineConfigBuilder &
    rasterization(VkCullModeFlags cullMode, VkFrontFace frontFace,
                  VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL);
    PipelineConfigBuilder &depth(bool test, bool write,
                                 VkCompareOp compare = VK_COMPARE_OP_LESS);
    PipelineConfigBuilder &depthBias(bool enable);
    PipelineConfigBuilder &blending(bool enable);
    PipelineConfigBuilder &colorAttachmentCount(uint32_t count);
    PipelineConfigBuilder &topology(VkPrimitiveTopology topology);
    PipelineConfigBuilder &msaa(VkSampleCountFlagBits samples);
    PipelineConfigBuilder &subpass(uint32_t subpass);
    PipelineConfigBuilder &descriptorLayout(VkDescriptorSetLayout layout);
    PipelineConfigBuilder &pushConstant(VkPushConstantRange range);

    PipelineConfig build() const;

  private:
    PipelineConfig config_;
};

} // namespace vkr
