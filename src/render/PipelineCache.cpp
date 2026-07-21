#include "PipelineCache.h"

#include "core/Pipeline.h"

namespace vkr {

PipelineCache::PipelineCache(Device &device) : device_(&device) {}

PipelineCache::~PipelineCache() = default;

void PipelineCache::clear() {
    pipelines_.clear();
}

Pipeline &PipelineCache::getOrCreate(VkRenderPass renderPass,
                                     PipelineConfig config) {
    PipelineKey key{renderPass, std::move(config)};
    auto it = pipelines_.find(key);
    if (it != pipelines_.end())
        return *it->second;

    auto pipeline =
        std::make_unique<Pipeline>(*device_, key.renderPass, key.config);
    auto *result = pipeline.get();
    pipelines_.emplace(std::move(key), std::move(pipeline));
    return *result;
}

} // namespace vkr
