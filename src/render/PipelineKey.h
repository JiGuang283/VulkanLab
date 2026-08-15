#pragma once

#include "render/pipeline/PipelineConfig.h"

#include <cstddef>
#include <functional>
#include <utility>
#include <vulkan/vulkan.h>

namespace vkr {

namespace pipeline_key_detail {

inline bool equal(const VkVertexInputBindingDescription &lhs,
                  const VkVertexInputBindingDescription &rhs) {
    return lhs.binding == rhs.binding && lhs.stride == rhs.stride &&
           lhs.inputRate == rhs.inputRate;
}

inline bool equal(const VkVertexInputAttributeDescription &lhs,
                  const VkVertexInputAttributeDescription &rhs) {
    return lhs.location == rhs.location && lhs.binding == rhs.binding &&
           lhs.format == rhs.format && lhs.offset == rhs.offset;
}

inline bool equal(const ColorBlendAttachmentConfig &lhs,
                  const ColorBlendAttachmentConfig &rhs) {
    return lhs.blendEnable == rhs.blendEnable &&
           lhs.colorWriteMask == rhs.colorWriteMask &&
           lhs.srcColorBlendFactor == rhs.srcColorBlendFactor &&
           lhs.dstColorBlendFactor == rhs.dstColorBlendFactor &&
           lhs.colorBlendOp == rhs.colorBlendOp &&
           lhs.srcAlphaBlendFactor == rhs.srcAlphaBlendFactor &&
           lhs.dstAlphaBlendFactor == rhs.dstAlphaBlendFactor &&
           lhs.alphaBlendOp == rhs.alphaBlendOp;
}

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
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!equalValue(lhs[i], rhs[i]))
            return false;
    }
    return true;
}

inline bool equal(const PipelineConfig &lhs, const PipelineConfig &rhs) {
    return lhs.vertShaderPath == rhs.vertShaderPath &&
           lhs.fragShaderPath == rhs.fragShaderPath &&
           vectorEqual(lhs.vertexLayout.bindings, rhs.vertexLayout.bindings,
                       [](const auto &a, const auto &b) {
                           return equal(a, b);
                       }) &&
           vectorEqual(lhs.vertexLayout.attributes,
                       rhs.vertexLayout.attributes,
                       [](const auto &a, const auto &b) {
                           return equal(a, b);
                       }) &&
           lhs.polygonMode == rhs.polygonMode &&
           lhs.cullMode == rhs.cullMode && lhs.frontFace == rhs.frontFace &&
           lhs.topology == rhs.topology &&
           lhs.depthTest == rhs.depthTest &&
           lhs.depthWrite == rhs.depthWrite &&
           lhs.depthCompare == rhs.depthCompare &&
           lhs.depthBiasEnable == rhs.depthBiasEnable &&
           vectorEqual(lhs.colorBlendAttachments,
                       rhs.colorBlendAttachments,
                       [](const auto &a, const auto &b) {
                           return equal(a, b);
                       }) &&
           lhs.msaaSamples == rhs.msaaSamples &&
           lhs.subpass == rhs.subpass &&
           lhs.descriptorLayouts == rhs.descriptorLayouts &&
           vectorEqual(lhs.pushConstants, rhs.pushConstants,
                       [](const auto &a, const auto &b) {
                           return equal(a, b);
                       });
}

template <typename T>
inline void combine(size_t &seed, const T &value) {
    seed ^= std::hash<T>{}(value) + static_cast<size_t>(0x9e3779b9u) +
            (seed << 6) + (seed >> 2);
}

template <typename T, typename HashValue>
inline void combineVector(size_t &seed, const std::vector<T> &values,
                          HashValue hashValue) {
    combine(seed, values.size());
    for (const T &value : values)
        hashValue(seed, value);
}

inline size_t hash(const PipelineConfig &config) {
    size_t seed = 0;
    combine(seed, config.vertShaderPath);
    combine(seed, config.fragShaderPath);
    combineVector(seed, config.vertexLayout.bindings,
                  [](size_t &target,
                     const VkVertexInputBindingDescription &binding) {
                      combine(target, binding.binding);
                      combine(target, binding.stride);
                      combine(target, binding.inputRate);
                  });
    combineVector(seed, config.vertexLayout.attributes,
                  [](size_t &target,
                     const VkVertexInputAttributeDescription &attribute) {
                      combine(target, attribute.location);
                      combine(target, attribute.binding);
                      combine(target, attribute.format);
                      combine(target, attribute.offset);
                  });
    combine(seed, config.polygonMode);
    combine(seed, config.cullMode);
    combine(seed, config.frontFace);
    combine(seed, config.topology);
    combine(seed, config.depthTest);
    combine(seed, config.depthWrite);
    combine(seed, config.depthCompare);
    combine(seed, config.depthBiasEnable);
    combineVector(seed, config.colorBlendAttachments,
                  [](size_t &target,
                     const ColorBlendAttachmentConfig &attachment) {
                      combine(target, attachment.blendEnable);
                      combine(target, attachment.colorWriteMask);
                      combine(target, attachment.srcColorBlendFactor);
                      combine(target, attachment.dstColorBlendFactor);
                      combine(target, attachment.colorBlendOp);
                      combine(target, attachment.srcAlphaBlendFactor);
                      combine(target, attachment.dstAlphaBlendFactor);
                      combine(target, attachment.alphaBlendOp);
                  });
    combine(seed, config.msaaSamples);
    combine(seed, config.subpass);
    combineVector(seed, config.descriptorLayouts,
                  [](size_t &target, VkDescriptorSetLayout layout) {
                      combine(target, layout);
                  });
    combineVector(seed, config.pushConstants,
                  [](size_t &target, const VkPushConstantRange &range) {
                      combine(target, range.stageFlags);
                      combine(target, range.offset);
                      combine(target, range.size);
                  });
    return seed;
}

inline size_t hash(const PipelineRenderingSignature &signature) {
    size_t seed = 0;
    combineVector(seed, signature.colorAttachmentFormats,
                  [](size_t &target, VkFormat format) {
                      combine(target, format);
                  });
    combine(seed, signature.depthAttachmentFormat);
    combine(seed, signature.stencilAttachmentFormat);
    combine(seed, signature.samples);
    combine(seed, signature.viewMask);
    return seed;
}

} // namespace pipeline_key_detail

struct PipelineKey {
    PipelineRenderingSignature rendering{};
    PipelineConfig config{};

    PipelineKey() = default;
    PipelineKey(PipelineRenderingSignature signature,
                PipelineConfig pipelineConfig)
        : rendering(std::move(signature)), config(std::move(pipelineConfig)) {}

    bool operator==(const PipelineKey &rhs) const {
        return rendering == rhs.rendering &&
               pipeline_key_detail::equal(config, rhs.config);
    }
};

struct PipelineKeyHash {
    size_t operator()(const PipelineKey &key) const {
        size_t seed = pipeline_key_detail::hash(key.config);
        pipeline_key_detail::combine(seed,
                                     pipeline_key_detail::hash(key.rendering));
        return seed;
    }
};

} // namespace vkr
