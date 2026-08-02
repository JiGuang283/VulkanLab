#pragma once

#include <string>

namespace vkr {

struct RendererShaderPaths {
    std::string shadowVert;
    std::string shadowMaskFrag;
    std::string fullscreenVert;
    std::string toneMapFrag;
    std::string presentFrag;
    std::string skyboxFrag;
    std::string bloomDownsampleComp;
    std::string bloomUpsampleComp;
    std::string atmosphereTransmittanceComp;
    std::string atmosphereMultipleScatteringComp;
    std::string atmosphereSkyViewComp;
    std::string atmosphereAerialPerspectiveComp;
    std::string atmosphereSkyFrag;
};

} // namespace vkr
