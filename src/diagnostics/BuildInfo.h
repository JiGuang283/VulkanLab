#pragma once

#include <string_view>

namespace vkr {

struct BuildFeatureInfo {
    bool editorUi = false;
    bool runtimeControl = false;
    bool capture = false;
    bool assetAuthoring = false;
    bool validation = false;
    bool gpuDebugUtils = false;
    bool gpuProfiling = false;
    bool assetTool = false;
    bool controlTool = false;
    bool renderTest = false;
};

struct BuildInfo {
    std::string_view revision;
    bool dirty = false;
    std::string_view configuration;
    std::string_view compiler;
    std::string_view vulkanSdk;
    std::string_view glslc;
    BuildFeatureInfo features;
};

const BuildInfo &currentBuildInfo();

} // namespace vkr
