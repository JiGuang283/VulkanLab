#pragma once

#include <string>

namespace vkr {

struct RendererShaderPaths {
    std::string shadowVert;
    std::string shadowMaskFrag;
    std::string surfacePrepassVert;
    std::string surfacePrepassOpaqueFrag;
    std::string surfacePrepassMaskFrag;
    std::string visibilityHiZInitComp;
    std::string visibilityHiZReduceComp;
    std::string visibilityOcclusionComp;
    std::string screenDepthInitComp;
    std::string screenDepthReduceComp;
    std::string screenColorInitComp;
    std::string screenColorReduceComp;
    std::string ssaoTraceComp;
    std::string ssaoBlurComp;
    std::string cacaoNormalAdapterComp;
    std::string gtaoTraceComp;
    std::string gtaoTemporalComp;
    std::string ssrTraceComp;
    std::string ssrTemporalComp;
    std::string ssrBlurComp;
    std::string ssgiTraceComp;
    std::string ssgiTemporalComp;
    std::string ssgiFilterComp;
    std::string reflectionCompositeComp;
    std::string taaResolveComp;
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
