#pragma once

#include "render/Vertex.h"

#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

struct VertexLayout {
    VkVertexInputBindingDescription                binding{};
    std::vector<VkVertexInputAttributeDescription> attributes;
};

struct PipelineConfig {
    std::string vertShaderPath;
    std::string fragShaderPath;

    VertexLayout vertexLayout;

    VkPolygonMode   polygonMode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
    VkFrontFace     frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    bool        depthTest = true;
    bool        depthWrite = true;
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS;

    bool blendEnable = false;

    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;

    std::vector<VkDescriptorSetLayout> descriptorLayouts;
    std::vector<VkPushConstantRange>   pushConstants;
};

inline VertexLayout defaultVertexLayout() {
    VertexLayout layout;
    layout.binding = Vertex::getBindingDescription();
    auto attrs = Vertex::getAttributeDescriptions();
    layout.attributes.assign(attrs.begin(), attrs.end());
    return layout;
}

} // namespace vkr
