#pragma once

#include "core/PipelineConfig.h"
#include "render/PipelineKey.h"

#include <memory>
#include <unordered_map>

namespace vkr {

class Device;
class Pipeline;

class PipelineCache {
  public:
    explicit PipelineCache(Device &device);
    ~PipelineCache();

    PipelineCache(const PipelineCache &) = delete;
    PipelineCache &operator=(const PipelineCache &) = delete;

    void clear();

    Pipeline &getOrCreate(const PipelineKey &key,
                          const PipelineConfig &config);

  private:
    Device *device_ = nullptr;
    std::unordered_map<PipelineKey, std::unique_ptr<Pipeline>, PipelineKeyHash>
        pipelines_;
};

} // namespace vkr
