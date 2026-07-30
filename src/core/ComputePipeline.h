#pragma once

#include "ComputePipelineConfig.h"

#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;

class ComputePipeline {
  public:
    ComputePipeline(Device &device, const ComputePipelineConfig &config);
    ~ComputePipeline();

    ComputePipeline(const ComputePipeline &) = delete;
    ComputePipeline &operator=(const ComputePipeline &) = delete;

    VkPipeline handle() const { return pipeline_; }
    VkPipelineLayout layout() const { return pipelineLayout_; }

  private:
    static std::vector<char> readFile(const std::string &path);
    VkShaderModule createShaderModule(const std::vector<char> &code);

    Device *device_ = nullptr;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
};

} // namespace vkr
