#pragma once

#include <string>

namespace vkr {

struct RendererShaderPaths {
    std::string shadowVert;
    std::string shadowMaskFrag;
    std::string fullscreenVert;
    std::string toneMapFrag;
};

} // namespace vkr
