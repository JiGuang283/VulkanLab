#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan.h>

namespace vkr {

enum class PreparedTextureDataKind { RawBaseLevel, PrebuiltMipChain };

struct PreparedMipLevel {
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct PreparedImage {
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    PreparedTextureDataKind kind = PreparedTextureDataKind::RawBaseLevel;
    std::vector<PreparedMipLevel> mipLevels;
};

} // namespace vkr
