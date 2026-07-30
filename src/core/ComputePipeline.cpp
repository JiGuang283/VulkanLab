#include "ComputePipeline.h"

#include "Device.h"
#include "GpuDebugUtils.h"
#include "VulkanCheck.h"

#include <fstream>
#include <stdexcept>

namespace vkr {

std::vector<char> ComputePipeline::readFile(const std::string &filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("failed to open file: " + filename);

    const size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(fileSize));
    return buffer;
}

VkShaderModule
ComputePipeline::createShaderModule(const std::vector<char> &code) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t *>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device_->logicalDevice(), &info, nullptr,
                                  &module));
    return module;
}

ComputePipeline::ComputePipeline(Device &device,
                                 const ComputePipelineConfig &config)
    : device_(&device) {
    if (config.computeShaderPath.empty())
        throw std::invalid_argument("compute shader path is empty");

    const auto shaderCode = readFile(config.computeShaderPath);
    const VkShaderModule shaderModule = createShaderModule(shaderCode);
    const std::string pipelineName =
        config.debugName.empty() ? "Pipeline/Compute/Unnamed"
                                 : config.debugName;
    device.debugUtils().setObjectName(VK_OBJECT_TYPE_SHADER_MODULE,
                                      shaderModule,
                                      pipelineName + "/ComputeShader");

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount =
        static_cast<uint32_t>(config.descriptorLayouts.size());
    layoutInfo.pSetLayouts = config.descriptorLayouts.data();
    layoutInfo.pushConstantRangeCount =
        static_cast<uint32_t>(config.pushConstants.size());
    layoutInfo.pPushConstantRanges = config.pushConstants.data();
    VK_CHECK(vkCreatePipelineLayout(device.logicalDevice(), &layoutInfo,
                                    nullptr, &pipelineLayout_));
    device.debugUtils().setObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                                      pipelineLayout_,
                                      pipelineName + "/Layout");

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shaderModule;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stage;
    pipelineInfo.layout = pipelineLayout_;
    VK_CHECK(vkCreateComputePipelines(device.logicalDevice(), VK_NULL_HANDLE,
                                      1, &pipelineInfo, nullptr,
                                      &pipeline_));
    device.debugUtils().setObjectName(VK_OBJECT_TYPE_PIPELINE, pipeline_,
                                      pipelineName);
    vkDestroyShaderModule(device.logicalDevice(), shaderModule, nullptr);
}

ComputePipeline::~ComputePipeline() {
    if (!device_)
        return;
    if (pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_->logicalDevice(), pipeline_, nullptr);
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_->logicalDevice(), pipelineLayout_,
                                nullptr);
    }
}

} // namespace vkr
