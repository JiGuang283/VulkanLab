#include "render/frame/FrameFeatureResolver.h"

#include "render/features/shadows_visibility/DirectionalShadow.h"
#include "render/features/lighting/ClusteredLighting.h"
#include "render/frame/RenderSettings.h"
#include "render/shader/ShaderTypes.h"

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
    if (!input.settings || !input.viewMode || !input.support ||
        !input.resources)
        throw std::invalid_argument("frame feature input is incomplete");

    const RenderSettings &settings = *input.settings;
    const ViewMode &viewMode = *input.viewMode;
    const RenderFeatureSupport &support = *input.support;
    FrameFeatureResolution result;
    FrameRenderFeatures &features = result.features;

    RenderPathCapabilities pathCapabilities{};
    pathCapabilities.forward = true;
    pathCapabilities.deferred = support.gBuffer.supported &&
                                support.deferredLighting.supported;
    pathCapabilities.multisampledOpaque =
        input.resources->hdrMsaaColor.valid();
    pathCapabilities.forwardTransparent = true;
    pathCapabilities.gBuffer = support.gBuffer.supported;
    features.renderPath = resolveRenderPath(
        settings.renderPath, pathCapabilities, viewMode.supportsDeferred);
    features.forwardOpaqueRequired =
        features.renderPath.active == RenderPathMode::Forward;

    const ScreenSpaceDebugView debug = settings.screenSpaceDebugView;
    const bool ssaoRequired =
        support.ssao.supported &&
        ((viewMode.supportsScreenSpace &&
          settings.ambientOcclusionMode == AmbientOcclusionMode::Ssao) ||
         isSsaoDebug(debug));
    const bool cacaoRequired =
        support.cacao.supported &&
        ((viewMode.supportsScreenSpace &&
          settings.ambientOcclusionMode == AmbientOcclusionMode::Cacao) ||
         isCacaoDebug(debug));
    const bool gtaoRequired =
        support.gtao.supported &&
        ((viewMode.supportsScreenSpace &&
          settings.ambientOcclusionMode == AmbientOcclusionMode::Gtao) ||
         isGtaoDebug(debug));
    const bool taaRequired =
        support.taa.supported &&
        (settings.temporalAntiAliasingMode == TemporalAntiAliasingMode::Taa ||
         isTaaDebug(debug));
    const bool ssrRequired =
        support.ssr.supported &&
        ((viewMode.supportsScreenSpace &&
          settings.reflectionMode == ReflectionMode::Ssr) ||
         isSsrDebug(debug));
    const bool ssgiRequested =
        settings.globalIlluminationMode == GlobalIlluminationMode::Ssgi ||
        settings.globalIlluminationMode == GlobalIlluminationMode::SsgiDdgi;
    const bool ssgiRequired =
        support.ssgi.supported &&
        ((viewMode.supportsScreenSpace &&
          settings.ddgi.debugView == DdgiDebugView::None && ssgiRequested) ||
         isSsgiDebug(debug));
    const bool ddgiRequested =
        settings.globalIlluminationMode == GlobalIlluminationMode::Ddgi ||
        settings.globalIlluminationMode == GlobalIlluminationMode::SsgiDdgi;
    const bool ddgiRequired = support.ddgi.supported && viewMode.supportsDdgi &&
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
        ssrRequired && viewMode.supportsScreenSpace &&
        settings.reflectionMode == ReflectionMode::Ssr;
    features.ssgiActive = ssgiRequired && viewMode.supportsScreenSpace &&
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
    features.punctualLightCount = input.punctualLightCount;
    features.clusteredLightingRequired =
        support.clusteredLighting.supported &&
        input.punctualLightCount >= kClusteredLightingMinPunctualLights;

    const bool occlusionWorkRequired =
        support.occlusionCulling.supported &&
        settings.culling.occlusionEnabled &&
        input.occlusionCandidates >= settings.culling.occlusionMinCandidates;
    const SurfaceDebugView surfaceDebug = settings.surfaceDebugView;
    features.deferredLightingRequired =
        support.deferredLighting.supported &&
        (features.renderPath.active == RenderPathMode::Deferred ||
         settings.deferredLightingDebugView !=
             DeferredLightingDebugView::None);
    features.gBufferRequired =
        support.gBuffer.supported &&
        (features.renderPath.active == RenderPathMode::Deferred ||
         settings.gBufferDebugView != GBufferDebugView::None ||
         features.deferredLightingRequired);
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
        (occlusionWorkRequired || features.surfaceNormalsRequired ||
         features.surfaceMotionRequired || features.surfaceAlbedoRequired ||
         debug == ScreenSpaceDebugView::NearestDepth);
    features.hiZRequired = occlusionWorkRequired;
    features.occlusionRequired = occlusionWorkRequired;
    features.screenDepthPyramidRequired =
        support.depthPyramid.supported &&
        (debug == ScreenSpaceDebugView::NearestDepth || gtaoRequired ||
         ssrRequired || ssgiRequired);
    features.depthHierarchyRequired =
        features.hiZRequired || features.screenDepthPyramidRequired;
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
                             support.bloom.supported && viewMode.supportsBloom;
    features.captureRequired = input.captureRequested;
    features.captureSource =
        input.captureRequested ? input.captureSource : std::nullopt;
    features.opaqueProducts = opaqueRenderProducts(
        features.renderPath.active, *input.resources,
        features.surfaceDataRequired);

    result.runtime.activeAmbientOcclusion =
        features.gtaoActive
            ? AmbientOcclusionMode::Gtao
            : features.cacaoActive
                  ? AmbientOcclusionMode::Cacao
                  : features.ssaoActive ? AmbientOcclusionMode::Ssao
                                        : AmbientOcclusionMode::Off;
    result.runtime.renderPath = features.renderPath;
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
    result.runtime.gBufferActive = features.gBufferRequired;
    result.runtime.deferredLightingActive =
        features.deferredLightingRequired;
    result.runtime.clusteredLightingActive =
        features.clusteredLightingRequired;
    result.runtime.taaActive = features.taaActive;
    result.runtime.ssrActive = features.ssrActive;
    result.runtime.ssgiActive = features.ssgiActive;
    result.runtime.ddgiActive = features.ddgiActive;
    return result;
}

} // namespace vkr
