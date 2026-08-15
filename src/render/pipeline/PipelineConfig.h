#pragma once

#include "render/Vertex.h"

#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

struct VertexLayout {
    std::vector<VkVertexInputBindingDescription>   bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
};

struct ColorBlendAttachmentConfig {
    bool blendEnable = false;
    VkColorComponentFlags colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkBlendFactor srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    VkBlendFactor dstColorBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkBlendOp colorBlendOp = VK_BLEND_OP_ADD;
    VkBlendFactor srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    VkBlendOp alphaBlendOp = VK_BLEND_OP_ADD;
};

struct PipelineRenderingSignature {
    std::vector<VkFormat> colorAttachmentFormats;
    VkFormat depthAttachmentFormat = VK_FORMAT_UNDEFINED;
    VkFormat stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t viewMask = 0;

    bool operator==(const PipelineRenderingSignature &rhs) const {
        return colorAttachmentFormats == rhs.colorAttachmentFormats &&
               depthAttachmentFormat == rhs.depthAttachmentFormat &&
               stencilAttachmentFormat == rhs.stencilAttachmentFormat &&
               samples == rhs.samples && viewMask == rhs.viewMask;
    }
};

struct PipelineConfig {
    // Diagnostic-only label. Pipeline cache equality intentionally ignores it.
    std::string debugName;
    std::string vertShaderPath;
    std::string fragShaderPath;

    VertexLayout vertexLayout;

    VkPolygonMode   polygonMode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
    VkFrontFace     frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    bool        depthTest = true;
    bool        depthWrite = true;
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS;
    bool        depthBiasEnable = false;

    std::vector<ColorBlendAttachmentConfig> colorBlendAttachments = {
        ColorBlendAttachmentConfig{}};

    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t subpass = 0;

    std::vector<VkDescriptorSetLayout> descriptorLayouts;
    std::vector<VkPushConstantRange>   pushConstants;
};

inline VertexLayout defaultVertexLayout() {
    VertexLayout layout;
    layout.bindings.push_back(Vertex::getBindingDescription());
    auto attrs = Vertex::getAttributeDescriptions();
    layout.attributes.assign(attrs.begin(), attrs.end());
    return layout;
}

} // namespace vkr
