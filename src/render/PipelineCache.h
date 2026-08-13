#pragma once

#include "core/ComputePipelineConfig.h"
#include "core/PipelineConfig.h"
#include "render/ComputePipelineKey.h"
#include "render/PipelineKey.h"

#include <memory>
#include <unordered_map>

namespace vkr {

class Device;
class ComputePipeline;
class Pipeline;

class PipelineCache {
  public:
    explicit PipelineCache(Device &device);
    ~PipelineCache();

    PipelineCache(const PipelineCache &) = delete;
    PipelineCache &operator=(const PipelineCache &) = delete;

    void clear();

    Pipeline &getOrCreate(PipelineRenderingSignature rendering,
                          PipelineConfig config);
    ComputePipeline &getOrCreateCompute(ComputePipelineConfig config);
    size_t graphicsPipelineCount() const { return pipelines_.size(); }
    size_t computePipelineCount() const { return computePipelines_.size(); }

  private:
    Device *device_ = nullptr;
    std::unordered_map<PipelineKey, std::unique_ptr<Pipeline>, PipelineKeyHash>
        pipelines_;
    std::unordered_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>,
                       ComputePipelineKeyHash>
        computePipelines_;
};

} // namespace vkr
