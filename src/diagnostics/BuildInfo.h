#pragma once

#include <string_view>

namespace vkr {

struct BuildInfo {
    std::string_view revision;
    bool dirty = false;
    std::string_view configuration;
    std::string_view compiler;
    std::string_view vulkanSdk;
    std::string_view glslc;
};

const BuildInfo &currentBuildInfo();

} // namespace vkr
