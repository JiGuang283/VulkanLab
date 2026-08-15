#include "render/shader/RendererProgramCatalog.h"

#include "render/shader/ShaderRegistry.h"

#include <cstdint>
#include <stdexcept>

namespace vkr {

ShadowPrograms ShadowPrograms::resolve(
    const ShaderRegistry &registry, MaterialBindingMode materialMode) {
    ShadowPrograms result;
    const auto materialFragment = [materialMode](const ShaderProgram &program)
        -> const std::string & {
        return program.fragmentSpvPath(materialMode);
    };

    const ShaderProgram &shadowOpaque = registry.program("shadow.opaque");
    const ShaderProgram &shadowMask = registry.program("shadow.mask");
    const ShaderProgram &pointOpaque =
        registry.program("shadow.point-opaque");
    const ShaderProgram &pointMask = registry.program("shadow.point-mask");
    const ShaderProgram &spotOpaque = registry.program("shadow.spot-opaque");
    const ShaderProgram &spotMask = registry.program("shadow.spot-mask");
    result.directionalVertex = shadowOpaque.vertSpvPath;
    result.directionalMaskFragment = materialFragment(shadowMask);
    result.punctualVertex = pointOpaque.vertSpvPath;
    result.pointFragment = pointOpaque.fragSpvPath;
    result.pointMaskFragment = materialFragment(pointMask);
    if (spotOpaque.vertSpvPath != result.punctualVertex) {
        throw std::runtime_error(
            "point and spot shadow programs must share the punctual vertex shader");
    }
    result.spotMaskFragment = materialFragment(spotMask);
    return result;
}

SurfaceVisibilityPrograms SurfaceVisibilityPrograms::resolve(
    const ShaderRegistry &registry, MaterialBindingMode materialMode) {
    SurfaceVisibilityPrograms result;
    const ShaderProgram &surfaceOpaque =
        registry.program("surface.prepass-opaque");
    const ShaderProgram &surfaceMask =
        registry.program("surface.prepass-mask");
    result.surfaceVertex = surfaceOpaque.vertSpvPath;
    for (uint32_t attachments = 0; attachments <= 3; ++attachments) {
        result.surfaceOpaqueFragments[attachments] =
            surfaceOpaque.fragmentSpvPath(materialMode, attachments);
        result.surfaceMaskFragments[attachments] =
            surfaceMask.fragmentSpvPath(materialMode, attachments);
    }
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
    const ShaderRegistry &registry, MaterialBindingMode materialMode) {
    return {ShadowPrograms::resolve(registry, materialMode),
            SurfaceVisibilityPrograms::resolve(registry, materialMode),
            ScreenSpacePrograms::resolve(registry),
            AtmosphereGiPrograms::resolve(registry),
            PostProcessPrograms::resolve(registry)};
}

} // namespace vkr
