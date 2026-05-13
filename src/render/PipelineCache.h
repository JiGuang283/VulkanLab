#pragma once

#include "core/PipelineConfig.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class Pipeline;

class PipelineCache {
  public:
    PipelineCache(Device &device, VkRenderPass renderPass);
    ~PipelineCache();

    PipelineCache(const PipelineCache &) = delete;
    PipelineCache &operator=(const PipelineCache &) = delete;

    void setRenderPass(VkRenderPass renderPass);
    void clear();

    Pipeline &getOrCreate(const PipelineConfig &config);

  private:
    std::string makeKey(const PipelineConfig &config) const;

    Device       *device_ = nullptr;
    VkRenderPass  renderPass_ = VK_NULL_HANDLE;
    std::unordered_map<std::string, std::unique_ptr<Pipeline>> pipelines_;
};

} // namespace vkr
