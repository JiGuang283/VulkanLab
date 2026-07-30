#pragma once

#include "core/ComputePipelineConfig.h"

#include <cstddef>
#include <functional>
#include <utility>

namespace vkr {

namespace compute_pipeline_key_detail {

inline bool equal(const VkPushConstantRange &lhs,
                  const VkPushConstantRange &rhs) {
    return lhs.stageFlags == rhs.stageFlags && lhs.offset == rhs.offset &&
           lhs.size == rhs.size;
}

template <typename T, typename Equal>
bool vectorEqual(const std::vector<T> &lhs, const std::vector<T> &rhs,
                 Equal equalValue) {
    if (lhs.size() != rhs.size())
        return false;
    for (size_t index = 0; index < lhs.size(); ++index) {
        if (!equalValue(lhs[index], rhs[index]))
            return false;
    }
    return true;
}

template <typename T>
inline void combine(size_t &seed, const T &value) {
    seed ^= std::hash<T>{}(value) + static_cast<size_t>(0x9e3779b9u) +
            (seed << 6) + (seed >> 2);
}

inline bool equal(const ComputePipelineConfig &lhs,
                  const ComputePipelineConfig &rhs) {
    return lhs.computeShaderPath == rhs.computeShaderPath &&
           lhs.descriptorLayouts == rhs.descriptorLayouts &&
           vectorEqual(lhs.pushConstants, rhs.pushConstants,
                       [](const auto &left, const auto &right) {
                           return equal(left, right);
                       });
}

inline size_t hash(const ComputePipelineConfig &config) {
    size_t seed = 0;
    combine(seed, config.computeShaderPath);
    combine(seed, config.descriptorLayouts.size());
    for (VkDescriptorSetLayout layout : config.descriptorLayouts)
        combine(seed, layout);
    combine(seed, config.pushConstants.size());
    for (const VkPushConstantRange &range : config.pushConstants) {
        combine(seed, range.stageFlags);
        combine(seed, range.offset);
        combine(seed, range.size);
    }
    return seed;
}

} // namespace compute_pipeline_key_detail

struct ComputePipelineKey {
    ComputePipelineConfig config{};

    ComputePipelineKey() = default;
    explicit ComputePipelineKey(ComputePipelineConfig pipelineConfig)
        : config(std::move(pipelineConfig)) {}

    bool operator==(const ComputePipelineKey &rhs) const {
        return compute_pipeline_key_detail::equal(config, rhs.config);
    }
};

struct ComputePipelineKeyHash {
    size_t operator()(const ComputePipelineKey &key) const {
        return compute_pipeline_key_detail::hash(key.config);
    }
};

} // namespace vkr
