#include "render/shader/RendererProgramCatalog.h"

#include "render/shader/ShaderRegistry.h"

#include <cstdint>
#include <stdexcept>

namespace vkr {

ShadowPrograms ShadowPrograms::resolve(const ShaderRegistry &registry) {
    ShadowPrograms result;

    const ShaderProgram &shadowOpaque = registry.program("shadow.opaque");
    const ShaderProgram &pointOpaque =
        registry.program("shadow.point-opaque");
    const ShaderProgram &spotOpaque = registry.program("shadow.spot-opaque");
    result.directionalVertex = shadowOpaque.vertSpvPath;
    result.punctualVertex = pointOpaque.vertSpvPath;
    result.pointFragment = pointOpaque.fragSpvPath;
    if (spotOpaque.vertSpvPath != result.punctualVertex) {
        throw std::runtime_error(
            "point and spot shadow programs must share the punctual vertex shader");
    }
    return result;
}

SurfaceVisibilityPrograms SurfaceVisibilityPrograms::resolve(
    const ShaderRegistry &registry) {
    SurfaceVisibilityPrograms result;
    result.depthHierarchyInitCompute =
        registry.program("visibility.depth-hierarchy-init").computeSpvPath;
    result.depthHierarchyReduceCompute =
        registry.program("visibility.depth-hierarchy-reduce").computeSpvPath;
    result.hiZInitCompute =
        registry.program("visibility.hiz-init").computeSpvPath;
    result.hiZReduceCompute =
        registry.program("visibility.hiz-reduce").computeSpvPath;
    result.occlusionCompute =
        registry.program("visibility.occlusion-cull").computeSpvPath;
    return result;
}

ScreenSpacePrograms ScreenSpacePrograms::resolve(
    const ShaderRegistry &registry) {
    ScreenSpacePrograms result;
    result.depthInitCompute =
        registry.program("screenspace.depth-init").computeSpvPath;
    result.depthReduceCompute =
        registry.program("screenspace.depth-reduce").computeSpvPath;
    result.colorInitCompute =
        registry.program("screenspace.color-init").computeSpvPath;
    result.colorReduceCompute =
        registry.program("screenspace.color-reduce").computeSpvPath;
    result.ssaoTraceCompute =
        registry.program("screenspace.ssao-trace").computeSpvPath;
    result.ssaoBlurCompute =
        registry.program("screenspace.ssao-blur").computeSpvPath;
    result.cacaoNormalAdapterCompute =
        registry.program("screenspace.cacao-normal-adapter").computeSpvPath;
    result.gtaoTraceCompute =
        registry.program("screenspace.gtao-trace").computeSpvPath;
    result.gtaoTemporalCompute =
        registry.program("screenspace.gtao-temporal").computeSpvPath;
    result.ssrTraceCompute =
        registry.program("screenspace.ssr-trace").computeSpvPath;
    result.ssrTemporalCompute =
        registry.program("screenspace.ssr-temporal").computeSpvPath;
    result.ssrBlurCompute =
        registry.program("screenspace.ssr-blur").computeSpvPath;
    result.ssgiTraceCompute =
        registry.program("screenspace.ssgi-trace").computeSpvPath;
    result.ssgiTemporalCompute =
        registry.program("screenspace.ssgi-temporal").computeSpvPath;
    result.ssgiFilterCompute =
        registry.program("screenspace.ssgi-filter").computeSpvPath;
    result.lightingCompositeCompute =
        registry.program("screenspace.reflection-composite").computeSpvPath;
    result.taaResolveCompute =
        registry.program("postprocess.taa-resolve").computeSpvPath;
    return result;
}

AtmosphereGiPrograms AtmosphereGiPrograms::resolve(
    const ShaderRegistry &registry) {
    AtmosphereGiPrograms result;
    result.transmittanceCompute =
        registry.program("atmosphere.transmittance").computeSpvPath;
    result.multipleScatteringCompute =
        registry.program("atmosphere.multiple-scattering").computeSpvPath;
    result.skyViewCompute =
        registry.program("atmosphere.sky-view").computeSpvPath;
    result.aerialPerspectiveCompute =
        registry.program("atmosphere.aerial-perspective").computeSpvPath;
    result.atmosphereSkyFragment =
        registry.program("atmosphere.sky").fragSpvPath;
    result.ddgiTraceCompute =
        registry.program("gi.ddgi-trace").computeSpvPath;
    result.ddgiUpdateCompute =
        registry.program("gi.ddgi-update").computeSpvPath;
    return result;
}

DeferredPrograms DeferredPrograms::resolve(const ShaderRegistry &registry) {
    DeferredPrograms result;
    result.lightingCompute =
        registry.program("deferred.lighting").computeSpvPath;
    result.clusterBuildCompute =
        registry.program("lighting.cluster-build").computeSpvPath;
    return result;
}

PostProcessPrograms PostProcessPrograms::resolve(
    const ShaderRegistry &registry) {
    PostProcessPrograms result;
    const ShaderProgram &toneMap = registry.program("postprocess.tonemap");
    result.fullscreenVertex = toneMap.vertSpvPath;
    result.toneMapFragment = toneMap.fragSpvPath;
    result.presentFragment = registry.program("postprocess.present").fragSpvPath;
    result.skyboxFragment = registry.program("skybox").fragSpvPath;
    result.bloomDownsampleCompute =
        registry.program("postprocess.bloom-downsample").computeSpvPath;
    result.bloomUpsampleCompute =
        registry.program("postprocess.bloom-upsample").computeSpvPath;
    return result;
}

RendererProgramCatalog RendererProgramCatalog::resolve(
    const ShaderRegistry &registry, MaterialBindingMode) {
    return {ShadowPrograms::resolve(registry),
            SurfaceVisibilityPrograms::resolve(registry),
            ScreenSpacePrograms::resolve(registry),
            AtmosphereGiPrograms::resolve(registry),
            DeferredPrograms::resolve(registry),
            PostProcessPrograms::resolve(registry)};
}

} // namespace vkr
