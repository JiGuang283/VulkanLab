#include "PipelineCache.h"

#include "render/pipeline/ComputePipeline.h"
#include "render/pipeline/Pipeline.h"
#include "diagnostics/Profiling.h"

namespace vkr {

PipelineCache::PipelineCache(Device &device) : device_(&device) {}

PipelineCache::~PipelineCache() = default;

void PipelineCache::clear() {
    computePipelines_.clear();
    pipelines_.clear();
}

Pipeline &PipelineCache::getOrCreate(PipelineRenderingSignature rendering,
                                     PipelineConfig config) {
    PipelineKey key{std::move(rendering), std::move(config)};
    auto it = pipelines_.find(key);
    if (it != pipelines_.end())
        return *it->second;

    VKL_PROFILE_ZONE("Create Dynamic Graphics Pipeline");
    VKL_PROFILE_TEXT(key.config.debugName);
    auto pipeline =
        std::make_unique<Pipeline>(*device_, key.rendering, key.config);
    auto *result = pipeline.get();
    pipelines_.emplace(std::move(key), std::move(pipeline));
    return *result;
}

ComputePipeline &
PipelineCache::getOrCreateCompute(ComputePipelineConfig config) {
    ComputePipelineKey key{std::move(config)};
    auto it = computePipelines_.find(key);
    if (it != computePipelines_.end())
        return *it->second;

    VKL_PROFILE_ZONE("Create Compute Pipeline");
    VKL_PROFILE_TEXT(key.config.debugName);
    auto pipeline = std::make_unique<ComputePipeline>(*device_, key.config);
    auto *result = pipeline.get();
    computePipelines_.emplace(std::move(key), std::move(pipeline));
    return *result;
}

} // namespace vkr
