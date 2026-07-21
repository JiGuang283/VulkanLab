#include "Pipeline.h"
#include "Device.h"
#include "VulkanCheck.h"

#include <fstream>

namespace vkr {

std::vector<char> Pipeline::readFile(const std::string &filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);

    if (!file.is_open()) {
        throw std::runtime_error("failed to open file: " + filename);
    }

    size_t            fileSize = (size_t)file.tellg();
    std::vector<char> buffer(fileSize);

    file.seekg(0);
    file.read(buffer.data(), fileSize);

    file.close();

    return buffer;
}

VkShaderModule Pipeline::createShaderModule(const std::vector<char> &code) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t *>(code.data());

    VkShaderModule shaderModule;
    VK_CHECK(vkCreateShaderModule(device_->logicalDevice(), &createInfo,
                                  nullptr, &shaderModule));

    return shaderModule;
}

Pipeline::Pipeline(Device &device, VkRenderPass renderPass,
                   const PipelineConfig &config)
    : device_(&device) {

    auto vertShaderCode = readFile(config.vertShaderPath);
    std::vector<char> fragShaderCode;
    if (!config.fragShaderPath.empty())
        fragShaderCode = readFile(config.fragShaderPath);

    VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
    VkShaderModule fragShaderModule =
        fragShaderCode.empty() ? VK_NULL_HANDLE
                               : createShaderModule(fragShaderCode);

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    std::vector<VkPipelineShaderStageCreateInfo> shaderStages = {
        vertShaderStageInfo};
    if (fragShaderModule != VK_NULL_HANDLE) {
        VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
        fragShaderStageInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragShaderStageInfo.module = fragShaderModule;
        fragShaderStageInfo.pName = "main";
        shaderStages.push_back(fragShaderStageInfo);
    }

    // 顶点输入
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    vertexInputInfo.vertexBindingDescriptionCount =
        static_cast<uint32_t>(config.vertexLayout.bindings.size());
    vertexInputInfo.pVertexBindingDescriptions =
        config.vertexLayout.bindings.empty()
            ? nullptr
            : config.vertexLayout.bindings.data();
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(config.vertexLayout.attributes.size());
    vertexInputInfo.pVertexAttributeDescriptions =
        config.vertexLayout.attributes.data();

    // 输入装配
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = config.topology;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // 视口（动态状态，这里仅占位）
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // 光栅化
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = config.polygonMode;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = config.cullMode;
    rasterizer.frontFace = config.frontFace;
    rasterizer.depthBiasEnable = config.depthBiasEnable ? VK_TRUE : VK_FALSE;

    // 多重采样
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_TRUE;
    multisampling.minSampleShading = .2f;
    multisampling.rasterizationSamples = config.msaaSamples;

    // 深度与模版测试
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = config.depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = config.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = config.depthCompare;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.minDepthBounds = 0.0f;
    depthStencil.maxDepthBounds = 1.0f;
    depthStencil.stencilTestEnable = VK_FALSE;
    depthStencil.front = {};
    depthStencil.back = {};

    // 颜色混合
    std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments;
    colorBlendAttachments.reserve(config.colorBlendAttachments.size());
    for (const auto &source : config.colorBlendAttachments) {
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = source.colorWriteMask;
        attachment.blendEnable = source.blendEnable ? VK_TRUE : VK_FALSE;
        attachment.srcColorBlendFactor = source.srcColorBlendFactor;
        attachment.dstColorBlendFactor = source.dstColorBlendFactor;
        attachment.colorBlendOp = source.colorBlendOp;
        attachment.srcAlphaBlendFactor = source.srcAlphaBlendFactor;
        attachment.dstAlphaBlendFactor = source.dstAlphaBlendFactor;
        attachment.alphaBlendOp = source.alphaBlendOp;
        colorBlendAttachments.push_back(attachment);
    }

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.logicOp = VK_LOGIC_OP_COPY;
    colorBlending.attachmentCount =
        static_cast<uint32_t>(colorBlendAttachments.size());
    colorBlending.pAttachments = colorBlendAttachments.empty()
                                     ? nullptr
                                     : colorBlendAttachments.data();
    colorBlending.blendConstants[0] = 0.0f;
    colorBlending.blendConstants[1] = 0.0f;
    colorBlending.blendConstants[2] = 0.0f;
    colorBlending.blendConstants[3] = 0.0f;

    // 动态状态
    std::vector<VkDynamicState> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR};
    if (config.depthBiasEnable)
        dynamicStates.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount =
        static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount =
        static_cast<uint32_t>(config.descriptorLayouts.size());
    pipelineLayoutInfo.pSetLayouts = config.descriptorLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount =
        static_cast<uint32_t>(config.pushConstants.size());
    pipelineLayoutInfo.pPushConstantRanges = config.pushConstants.data();

    VK_CHECK(vkCreatePipelineLayout(device_->logicalDevice(),
                                    &pipelineLayoutInfo, nullptr,
                                    &pipelineLayout_));

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = config.subpass;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    VK_CHECK(vkCreateGraphicsPipelines(device_->logicalDevice(), VK_NULL_HANDLE,
                                       1, &pipelineInfo, nullptr, &pipeline_));

    if (fragShaderModule != VK_NULL_HANDLE)
        vkDestroyShaderModule(device_->logicalDevice(), fragShaderModule,
                              nullptr);
    vkDestroyShaderModule(device_->logicalDevice(), vertShaderModule, nullptr);
}

Pipeline::~Pipeline() {
    if (device_) {
        VkDevice d = device_->logicalDevice();
        if (pipeline_ != VK_NULL_HANDLE)
            vkDestroyPipeline(d, pipeline_, nullptr);
        if (pipelineLayout_ != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(d, pipelineLayout_, nullptr);
    }
}

} // namespace vkr
