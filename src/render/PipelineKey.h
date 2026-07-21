#pragma once

#include "render/RenderCommand.h"
#include "render/ShaderVariant.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vulkan/vulkan.h>

namespace vkr {

class MaterialTemplate;

enum class PassId : uint32_t {
    DirectionalShadow = 0,
    MainForward = 1,
    ToneMap = 2,
};

struct PipelineKey {
    const MaterialTemplate *materialTemplate = nullptr;
    PassId                 pass = PassId::MainForward;
    ShaderVariantId        shaderVariant = ShaderVariantId::LegacyForward;
    RenderQueueType        queue = RenderQueueType::Opaque;
    VkCullModeFlags        cullMode = VK_CULL_MODE_BACK_BIT;
    VkRenderPass           renderPass = VK_NULL_HANDLE;
    uint32_t               subpass = 0;
    VkSampleCountFlagBits  samples = VK_SAMPLE_COUNT_1_BIT;
    bool                   alphaMasked = false;

    bool operator==(const PipelineKey &rhs) const {
        return materialTemplate == rhs.materialTemplate && pass == rhs.pass &&
               shaderVariant == rhs.shaderVariant &&
               queue == rhs.queue && cullMode == rhs.cullMode &&
               renderPass == rhs.renderPass && subpass == rhs.subpass &&
               samples == rhs.samples && alphaMasked == rhs.alphaMasked;
    }
};

struct PipelineKeyHash {
    size_t operator()(const PipelineKey &key) const {
        size_t seed = 0;
        combine(seed, key.materialTemplate);
        combine(seed, static_cast<uint32_t>(key.pass));
        combine(seed, static_cast<uint32_t>(key.shaderVariant));
        combine(seed, static_cast<uint32_t>(key.queue));
        combine(seed, key.cullMode);
        combine(seed, key.renderPass);
        combine(seed, key.subpass);
        combine(seed, key.samples);
        combine(seed, key.alphaMasked);
        return seed;
    }

  private:
    template <typename T>
    static void combine(size_t &seed, const T &value) {
        seed ^= std::hash<T>{}(value) + 0x9e3779b9u + (seed << 6) +
                (seed >> 2);
    }
};

} // namespace vkr
