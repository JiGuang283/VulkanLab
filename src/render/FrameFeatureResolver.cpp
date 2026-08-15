#include "render/FrameFeatureResolver.h"

#include "render/DirectionalShadow.h"
#include "render/RenderSettings.h"
#include "render/ShaderVariant.h"

#include <stdexcept>

namespace vkr {
namespace {

bool isSsaoDebug(ScreenSpaceDebugView view) {
    return view == ScreenSpaceDebugView::SsaoRaw ||
           view == ScreenSpaceDebugView::SsaoFiltered;
}

bool isCacaoDebug(ScreenSpaceDebugView view) {
    return view == ScreenSpaceDebugView::CacaoOutput;
}

bool isGtaoDebug(ScreenSpaceDebugView view) {
    return view == ScreenSpaceDebugView::GtaoRaw ||
           view == ScreenSpaceDebugView::GtaoTemporal ||
           view == ScreenSpaceDebugView::GtaoFiltered ||
           view == ScreenSpaceDebugView::GtaoRejection ||
           view == ScreenSpaceDebugView::GtaoHistoryWeight;
}

bool isTaaDebug(ScreenSpaceDebugView view) {
    return view == ScreenSpaceDebugView::TaaHistory ||
           view == ScreenSpaceDebugView::TaaRejection ||
           view == ScreenSpaceDebugView::TaaHistoryWeight;
}

bool isSsrDebug(ScreenSpaceDebugView view) {
    return view == ScreenSpaceDebugView::SsrRaw ||
           view == ScreenSpaceDebugView::SsrTemporal ||
           view == ScreenSpaceDebugView::SsrFiltered ||
           view == ScreenSpaceDebugView::SsrConfidence ||
           view == ScreenSpaceDebugView::SsrRejection;
}

bool isSsgiDebug(ScreenSpaceDebugView view) {
    return view == ScreenSpaceDebugView::SsgiRaw ||
           view == ScreenSpaceDebugView::SsgiTemporal ||
           view == ScreenSpaceDebugView::SsgiFiltered ||
           view == ScreenSpaceDebugView::SsgiConfidence ||
           view == ScreenSpaceDebugView::SsgiVariance ||
           view == ScreenSpaceDebugView::SsgiRejection;
}

} // namespace

FrameFeatureResolution
resolveFrameFeatures(const FrameFeatureResolveInput &input) {
    if (!input.settings || !input.shaderVariant || !input.support)
        throw std::invalid_argument("frame feature input is incomplete");

    const RenderSettings &settings = *input.settings;
    const ShaderVariant &shader = *input.shaderVariant;
    const RenderFeatureSupport &support = *input.support;
    FrameFeatureResolution result;
    FrameRenderFeatures &features = result.features;

    const ScreenSpaceDebugView debug = settings.screenSpaceDebugView;
    const bool ssaoRequired =
        support.ssao.supported &&
        ((shader.supportsScreenSpace &&
          settings.ambientOcclusionMode == AmbientOcclusionMode::Ssao) ||
         isSsaoDebug(debug));
    const bool cacaoRequired =
        support.cacao.supported &&
        ((shader.supportsScreenSpace &&
          settings.ambientOcclusionMode == AmbientOcclusionMode::Cacao) ||
         isCacaoDebug(debug));
    const bool gtaoRequired =
        support.gtao.supported &&
        ((shader.supportsScreenSpace &&
          settings.ambientOcclusionMode == AmbientOcclusionMode::Gtao) ||
         isGtaoDebug(debug));
    const bool taaRequired =
        support.taa.supported &&
        (settings.temporalAntiAliasingMode == TemporalAntiAliasingMode::Taa ||
         isTaaDebug(debug));
    const bool ssrRequired =
        support.ssr.supported &&
        ((shader.supportsScreenSpace &&
          settings.reflectionMode == ReflectionMode::Ssr) ||
         isSsrDebug(debug));
    const bool ssgiRequested =
        settings.globalIlluminationMode == GlobalIlluminationMode::Ssgi ||
        settings.globalIlluminationMode == GlobalIlluminationMode::SsgiDdgi;
    const bool ssgiRequired =
        support.ssgi.supported &&
        ((shader.supportsScreenSpace &&
          settings.ddgi.debugView == DdgiDebugView::None && ssgiRequested) ||
         isSsgiDebug(debug));
    const bool ddgiRequested =
        settings.globalIlluminationMode == GlobalIlluminationMode::Ddgi ||
        settings.globalIlluminationMode == GlobalIlluminationMode::SsgiDdgi;
    const bool ddgiRequired = support.ddgi.supported && shader.supportsDdgi &&
                              input.ddgiSceneActive && ddgiRequested;

    features.ssaoActive =
        ssaoRequired && settings.ambientOcclusionMode == AmbientOcclusionMode::Ssao;
    features.cacaoActive =
        cacaoRequired && settings.ambientOcclusionMode == AmbientOcclusionMode::Cacao;
    features.gtaoActive =
        gtaoRequired && settings.ambientOcclusionMode == AmbientOcclusionMode::Gtao;
    features.taaActive =
        taaRequired && settings.temporalAntiAliasingMode ==
                           TemporalAntiAliasingMode::Taa;
    features.ssrActive =
        ssrRequired && shader.supportsScreenSpace &&
        settings.reflectionMode == ReflectionMode::Ssr;
    features.ssgiActive = ssgiRequired && shader.supportsScreenSpace &&
                          settings.ddgi.debugView == DdgiDebugView::None &&
                          ssgiRequested;
    features.ddgiActive = ddgiRequired;

    features.atmosphereRequired = input.atmosphereActive;
    features.transparentRequired = input.transparentVisible;
    features.directionalShadowRequired = input.directionalShadowActive;
    features.pointShadowRequired = input.pointShadowLightCount > 0;
    features.spotShadowRequired = input.spotShadowLightCount > 0;
    features.directionalShadowCascadeCount =
        input.directionalShadowActive
            ? input.directionalShadowCascadeCount
            : 0;
    features.pointShadowLightCount = input.pointShadowLightCount;
    features.spotShadowLightCount = input.spotShadowLightCount;

    const bool occlusionWorkRequired =
        support.occlusionCulling.supported &&
        settings.culling.occlusionEnabled &&
        input.occlusionCandidates >= settings.culling.occlusionMinCandidates;
    const SurfaceDebugView surfaceDebug = settings.surfaceDebugView;
    features.surfaceAlbedoRequired = ssgiRequired;
    features.surfaceMotionRequired =
        surfaceDebug == SurfaceDebugView::Motion || gtaoRequired ||
        ssrRequired || ssgiRequired || taaRequired;
    features.surfaceNormalsRequired =
        surfaceDebug != SurfaceDebugView::None || ssaoRequired ||
        cacaoRequired || gtaoRequired || ssrRequired || ssgiRequired ||
        taaRequired;
    features.surfaceDataRequired =
        support.surfaceData.supported &&
        (features.transparentRequired || occlusionWorkRequired ||
         features.surfaceNormalsRequired || features.surfaceMotionRequired ||
         features.surfaceAlbedoRequired ||
         debug == ScreenSpaceDebugView::NearestDepth);
    features.hiZRequired = occlusionWorkRequired;
    features.occlusionRequired = occlusionWorkRequired;
    features.screenDepthPyramidRequired =
        support.depthPyramid.supported &&
        (debug == ScreenSpaceDebugView::NearestDepth || gtaoRequired ||
         ssrRequired || ssgiRequired);
    features.sceneColorPyramidRequired =
        support.colorPyramid.supported &&
        (debug == ScreenSpaceDebugView::SceneColor || ssrRequired ||
         ssgiRequired);
    features.ssaoRequired = ssaoRequired;
    features.cacaoRequired = cacaoRequired;
    features.gtaoRequired = gtaoRequired;
    features.taaRequired = taaRequired;
    features.ssrRequired = ssrRequired;
    features.ssgiRequired = ssgiRequired;
    features.lightingCompositeRequired =
        features.ssrActive || features.ssgiActive;
    features.ddgiRequired = ddgiRequired;
    features.bloomRequired = settings.bloomEnabled &&
                             support.bloom.supported && shader.supportsBloom;
    features.captureRequired = input.captureRequested;
    features.captureSource =
        input.captureRequested ? input.captureSource : std::nullopt;

    result.runtime.activeAmbientOcclusion =
        features.gtaoActive
            ? AmbientOcclusionMode::Gtao
            : features.cacaoActive
                  ? AmbientOcclusionMode::Cacao
                  : features.ssaoActive ? AmbientOcclusionMode::Ssao
                                        : AmbientOcclusionMode::Off;
    result.runtime.activeGlobalIllumination =
        features.ssgiActive && features.ddgiActive
            ? GlobalIlluminationMode::SsgiDdgi
            : features.ssgiActive
                  ? GlobalIlluminationMode::Ssgi
                  : features.ddgiActive
                        ? GlobalIlluminationMode::Ddgi
                        : GlobalIlluminationMode::AmbientOrIbl;
    result.runtime.bloomActive = features.bloomRequired;
    result.runtime.occlusionCullingActive = features.occlusionRequired;
    result.runtime.surfaceDataActive = features.surfaceDataRequired;
    result.runtime.taaActive = features.taaActive;
    result.runtime.ssrActive = features.ssrActive;
    result.runtime.ssgiActive = features.ssgiActive;
    result.runtime.ddgiActive = features.ddgiActive;
    return result;
}

} // namespace vkr
