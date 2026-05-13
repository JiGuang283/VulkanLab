#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vulkan/vulkan.h>

namespace vkr {

class MaterialTemplate;

enum class PassId : uint32_t {
    MainForward = 0,
};

struct PipelineKey {
    const MaterialTemplate *materialTemplate = nullptr;
    PassId                 pass = PassId::MainForward;
    VkRenderPass           renderPass = VK_NULL_HANDLE;
    uint32_t               subpass = 0;
    VkSampleCountFlagBits  samples = VK_SAMPLE_COUNT_1_BIT;

    bool operator==(const PipelineKey &rhs) const {
        return materialTemplate == rhs.materialTemplate && pass == rhs.pass &&
               renderPass == rhs.renderPass && subpass == rhs.subpass &&
               samples == rhs.samples;
    }
};

struct PipelineKeyHash {
    size_t operator()(const PipelineKey &key) const {
        size_t seed = 0;
        combine(seed, key.materialTemplate);
        combine(seed, static_cast<uint32_t>(key.pass));
        combine(seed, key.renderPass);
        combine(seed, key.subpass);
        combine(seed, key.samples);
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
