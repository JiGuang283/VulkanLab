#pragma once

#include "DerivedEnvironmentManifest.h"

#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

struct PreparedEnvironmentSubresource {
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevel = 0;
    uint32_t arrayLayer = 0;
};

struct PreparedEnvironmentImage {
    EnvironmentMapKind kind = EnvironmentMapKind::Radiance;
    std::vector<uint8_t> bytes;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::vector<PreparedEnvironmentSubresource> subresources;
};

struct PreparedEnvironmentData {
    std::string environmentId;
    std::string displayName;
    std::string profileId;
    PreparedEnvironmentImage radiance;
    PreparedEnvironmentImage irradiance;
    PreparedEnvironmentImage prefilteredSpecular;
    PreparedEnvironmentImage brdfLut;
};

} // namespace vkr
