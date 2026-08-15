#pragma once

#include "render/frame/RenderSettings.h"

#include <string>

namespace vkr {

struct RenderFeatureAvailability {
    bool supported = false;
    std::string unavailableReason;
};

struct RenderFeatureSupport {
    RenderFeatureAvailability bloom;
    RenderFeatureAvailability occlusionCulling;
    RenderFeatureAvailability surfaceData;
    RenderFeatureAvailability depthPyramid;
    RenderFeatureAvailability colorPyramid;
    RenderFeatureAvailability ssao;
    RenderFeatureAvailability cacao;
    RenderFeatureAvailability gtao;
    RenderFeatureAvailability taa;
    RenderFeatureAvailability ssr;
    RenderFeatureAvailability ssgi;
    RenderFeatureAvailability ddgi;
};

struct RenderFeatureRuntimeState {
    AmbientOcclusionMode activeAmbientOcclusion = AmbientOcclusionMode::Off;
    GlobalIlluminationMode activeGlobalIllumination =
        GlobalIlluminationMode::AmbientOrIbl;
    bool bloomActive = false;
    bool occlusionCullingActive = false;
    bool surfaceDataActive = false;
    bool taaActive = false;
    bool ssrActive = false;
    bool ssgiActive = false;
    bool ddgiActive = false;
};

} // namespace vkr
