#pragma once

#include "core/MaterialBindingMode.h"

#include <array>
#include <string>

namespace vkr {

class ShaderRegistry;

struct ShadowPrograms {
    std::string directionalVertex;
    std::string directionalMaskFragment;
    std::string punctualVertex;
    std::string pointFragment;
    std::string pointMaskFragment;
    std::string spotMaskFragment;

    static ShadowPrograms resolve(const ShaderRegistry &registry,
                                  MaterialBindingMode materialMode);
};

struct SurfaceVisibilityPrograms {
    std::string surfaceVertex;
    std::array<std::string, 4> surfaceOpaqueFragments;
    std::array<std::string, 4> surfaceMaskFragments;
    std::string hiZInitCompute;
    std::string hiZReduceCompute;
    std::string occlusionCompute;

    static SurfaceVisibilityPrograms
    resolve(const ShaderRegistry &registry, MaterialBindingMode materialMode);
};

struct ScreenSpacePrograms {
    std::string depthInitCompute;
    std::string depthReduceCompute;
    std::string colorInitCompute;
    std::string colorReduceCompute;
    std::string ssaoTraceCompute;
    std::string ssaoBlurCompute;
    std::string cacaoNormalAdapterCompute;
    std::string gtaoTraceCompute;
    std::string gtaoTemporalCompute;
    std::string ssrTraceCompute;
    std::string ssrTemporalCompute;
    std::string ssrBlurCompute;
    std::string ssgiTraceCompute;
    std::string ssgiTemporalCompute;
    std::string ssgiFilterCompute;
    std::string lightingCompositeCompute;
    std::string taaResolveCompute;

    static ScreenSpacePrograms resolve(const ShaderRegistry &registry);
};

struct AtmosphereGiPrograms {
    std::string transmittanceCompute;
    std::string multipleScatteringCompute;
    std::string skyViewCompute;
    std::string aerialPerspectiveCompute;
    std::string atmosphereSkyFragment;
    std::string ddgiTraceCompute;
    std::string ddgiUpdateCompute;

    static AtmosphereGiPrograms resolve(const ShaderRegistry &registry);
};

struct PostProcessPrograms {
    std::string fullscreenVertex;
    std::string skyboxFragment;
    std::string toneMapFragment;
    std::string presentFragment;
    std::string bloomDownsampleCompute;
    std::string bloomUpsampleCompute;

    static PostProcessPrograms resolve(const ShaderRegistry &registry);
};

struct RendererProgramCatalog {
    ShadowPrograms shadows;
    SurfaceVisibilityPrograms surfaceVisibility;
    ScreenSpacePrograms screenSpace;
    AtmosphereGiPrograms atmosphereGi;
    PostProcessPrograms postProcess;

    static RendererProgramCatalog resolve(const ShaderRegistry &registry,
                                          MaterialBindingMode materialMode);
};

} // namespace vkr
