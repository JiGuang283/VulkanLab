#include "PipelineConfigBuilder.h"

#include <utility>

namespace vkr {

PipelineConfigBuilder &PipelineConfigBuilder::shaders(std::string vertPath,
                                                      std::string fragPath) {
    config_.vertShaderPath = std::move(vertPath);
    config_.fragShaderPath = std::move(fragPath);
    return *this;
}

PipelineConfigBuilder &
PipelineConfigBuilder::vertexLayout(VertexLayout layout) {
    config_.vertexLayout = std::move(layout);
    return *this;
}

PipelineConfigBuilder &PipelineConfigBuilder::defaultVertexLayout() {
    config_.vertexLayout = vkr::defaultVertexLayout();
    return *this;
}

PipelineConfigBuilder &
PipelineConfigBuilder::rasterization(VkCullModeFlags cullMode,
                                     VkFrontFace     frontFace,
                                     VkPolygonMode   polygonMode) {
    config_.cullMode = cullMode;
    config_.frontFace = frontFace;
    config_.polygonMode = polygonMode;
    return *this;
}

PipelineConfigBuilder &PipelineConfigBuilder::depth(bool test, bool write,
                                                    VkCompareOp compare) {
    config_.depthTest = test;
    config_.depthWrite = write;
    config_.depthCompare = compare;
    return *this;
}

PipelineConfigBuilder &PipelineConfigBuilder::blending(bool enable) {
    if (config_.colorBlendAttachments.empty())
        config_.colorBlendAttachments.resize(1);
    for (auto &attachment : config_.colorBlendAttachments)
        attachment.blendEnable = enable;
    return *this;
}

PipelineConfigBuilder &PipelineConfigBuilder::depthBias(bool enable) {
    config_.depthBiasEnable = enable;
    return *this;
}

PipelineConfigBuilder &
PipelineConfigBuilder::colorAttachmentCount(uint32_t count) {
    config_.colorBlendAttachments.resize(count);
    return *this;
}

PipelineConfigBuilder &
PipelineConfigBuilder::topology(VkPrimitiveTopology topology) {
    config_.topology = topology;
    return *this;
}

PipelineConfigBuilder &
PipelineConfigBuilder::msaa(VkSampleCountFlagBits samples) {
    config_.msaaSamples = samples;
    return *this;
}

PipelineConfigBuilder &PipelineConfigBuilder::subpass(uint32_t subpass) {
    config_.subpass = subpass;
    return *this;
}

PipelineConfigBuilder &
PipelineConfigBuilder::descriptorLayout(VkDescriptorSetLayout layout) {
    config_.descriptorLayouts.push_back(layout);
    return *this;
}

PipelineConfigBuilder &
PipelineConfigBuilder::pushConstant(VkPushConstantRange range) {
    config_.pushConstants.push_back(range);
    return *this;
}

PipelineConfig PipelineConfigBuilder::build() const {
    return config_;
}

} // namespace vkr
