#pragma once
#include "PipelineConfig.h"

#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;

class Pipeline {
  public:
    Pipeline(Device &device, const PipelineRenderingSignature &rendering,
             const PipelineConfig &config);
    ~Pipeline();

    Pipeline(const Pipeline &) = delete;
    Pipeline &operator=(const Pipeline &) = delete;

    VkPipeline       handle() const { return pipeline_; }
    VkPipelineLayout layout() const { return pipelineLayout_; }

  private:
    static std::vector<char> readFile(const std::string &path);
    VkShaderModule           createShaderModule(const std::vector<char> &code);

    Device          *device_ = nullptr;
    VkPipeline       pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
};

} // namespace vkr
