#include "PipelineCache.h"

#include "core/Pipeline.h"

#include <sstream>

namespace vkr {

namespace {
template <typename T>
void appendHandle(std::ostringstream &out, T handle) {
    out << handle;
}
} // namespace

PipelineCache::PipelineCache(Device &device, VkRenderPass renderPass)
    : device_(&device), renderPass_(renderPass) {}

PipelineCache::~PipelineCache() = default;

void PipelineCache::setRenderPass(VkRenderPass renderPass) {
    renderPass_ = renderPass;
}

void PipelineCache::clear() {
    pipelines_.clear();
}

Pipeline &PipelineCache::getOrCreate(const PipelineConfig &config) {
    const auto key = makeKey(config);
    auto       it = pipelines_.find(key);
    if (it != pipelines_.end())
        return *it->second;

    auto pipeline = std::make_unique<Pipeline>(*device_, renderPass_, config);
    auto *result = pipeline.get();
    pipelines_.emplace(key, std::move(pipeline));
    return *result;
}

std::string PipelineCache::makeKey(const PipelineConfig &config) const {
    std::ostringstream out;
    out << "rp=";
    appendHandle(out, renderPass_);
    out << "|vs=" << config.vertShaderPath;
    out << "|fs=" << config.fragShaderPath;
    out << "|poly=" << config.polygonMode;
    out << "|cull=" << config.cullMode;
    out << "|front=" << config.frontFace;
    out << "|depth=" << config.depthTest << ',' << config.depthWrite << ','
        << config.depthCompare;
    out << "|blend=" << config.blendEnable;
    out << "|msaa=" << config.msaaSamples;

    out << "|binding=" << config.vertexLayout.binding.binding << ','
        << config.vertexLayout.binding.stride << ','
        << config.vertexLayout.binding.inputRate;
    out << "|attrs=" << config.vertexLayout.attributes.size();
    for (const auto &attr : config.vertexLayout.attributes) {
        out << ':' << attr.location << ',' << attr.binding << ','
            << attr.format << ',' << attr.offset;
    }

    out << "|sets=" << config.descriptorLayouts.size();
    for (auto layout : config.descriptorLayouts) {
        out << ':';
        appendHandle(out, layout);
    }

    out << "|push=" << config.pushConstants.size();
    for (const auto &range : config.pushConstants) {
        out << ':' << range.stageFlags << ',' << range.offset << ','
            << range.size;
    }

    return out.str();
}

} // namespace vkr
