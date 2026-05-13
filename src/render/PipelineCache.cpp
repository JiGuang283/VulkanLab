#include "PipelineCache.h"

#include "core/Pipeline.h"

namespace vkr {

PipelineCache::PipelineCache(Device &device) : device_(&device) {}

PipelineCache::~PipelineCache() = default;

void PipelineCache::clear() {
    pipelines_.clear();
}

Pipeline &PipelineCache::getOrCreate(const PipelineKey &key,
                                     const PipelineConfig &config) {
    auto it = pipelines_.find(key);
    if (it != pipelines_.end())
        return *it->second;

    auto pipeline =
        std::make_unique<Pipeline>(*device_, key.renderPass, config);
    auto *result = pipeline.get();
    pipelines_.emplace(key, std::move(pipeline));
    return *result;
}

} // namespace vkr
