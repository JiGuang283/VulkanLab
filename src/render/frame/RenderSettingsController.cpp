#include "RenderSettingsController.h"

#include "render/features/shadows_visibility/DirectionalShadow.h"
#include "render/features/shadows_visibility/PunctualShadow.h"

#include <algorithm>
#include <cmath>
#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>
#include <initializer_list>
#include <type_traits>

namespace vkr {
namespace {

[[noreturn]] void unsupported(const char *code, const char *label,
                              const RenderFeatureAvailability &availability) {
    throw RenderSettingsError(
        code, std::string(label) + " is unavailable" +
                  (availability.unavailableReason.empty()
                       ? std::string{"."}
                       : ": " + availability.unavailableReason));
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

template <typename Enum>
void requireEnumInRange(const std::optional<Enum> &value, Enum last,
                        const char *label) {
    static_assert(std::is_enum_v<Enum>);
    if (!value)
        return;
    using Value = std::underlying_type_t<Enum>;
    const Value raw = static_cast<Value>(*value);
    if (raw < 0 || raw > static_cast<Value>(last)) {
        throw RenderSettingsError(
            "invalid_render_setting",
            std::string(label) + " contains an invalid enum value.");
    }
}

} // namespace

RenderSettingsController::RenderSettingsController(
    const std::filesystem::path &manifestPath)
    : shaderRegistry_(ShaderRegistry::load(manifestPath)),
      currentShaderVariantId_(shaderRegistry_.defaultVariant().id) {}

const ShaderVariant &RenderSettingsController::currentShaderVariant() const {
    const ShaderVariant *variant =
        shaderRegistry_.findVariant(currentShaderVariantId_);
    if (!variant)
        throw std::logic_error("current shader variant is not registered");
    return *variant;
}

void RenderSettingsController::setShaderVariant(const std::string &id) {
    const ShaderVariant *variant = shaderRegistry_.findVariant(id);
    if (!variant || variant->id != id)
        throw RenderSettingsError("invalid_shader", "Invalid shader ID.");
    currentShaderVariantId_ = variant->id;
}

void RenderSettingsController::configure(RenderFeatureSupport support,
                                         RenderSettingsCallbacks callbacks) {
    support_ = std::move(support);
    callbacks_ = std::move(callbacks);
    configured_ = true;
}

void RenderSettingsController::updateRuntimeState(
    RenderFeatureRuntimeState state) {
    runtime_ = std::move(state);
}

void RenderSettingsController::validatePatch(
    const RenderSettingsPatch &patch) const {
    for (const auto &[label, value] :
         std::initializer_list<
             std::pair<const char *, const std::optional<float> *>>{
             {"shadowReceiverBias", &patch.shadowReceiverBias},
             {"pointShadowReceiverBiasWorld",
              &patch.pointShadowReceiverBiasWorld},
             {"shadowConstantBias", &patch.shadowConstantBias},
             {"shadowSlopeBias", &patch.shadowSlopeBias},
             {"pointShadowDistance", &patch.pointShadowDistance},
             {"spotShadowDistance", &patch.spotShadowDistance},
             {"exposureEv", &patch.exposureEv},
             {"bloomThreshold", &patch.bloomThreshold},
             {"bloomSoftKnee", &patch.bloomSoftKnee},
             {"bloomIntensity", &patch.bloomIntensity},
             {"environmentIntensity", &patch.environmentIntensity},
             {"environmentRotationRadians",
              &patch.environmentRotationRadians},
             {"surfaceMotionDebugScale", &patch.surfaceMotionDebugScale},
             {"ssaoRadius", &patch.ssaoRadius},
             {"ssaoBias", &patch.ssaoBias},
             {"ssaoIntensity", &patch.ssaoIntensity},
             {"ssaoPower", &patch.ssaoPower},
             {"cacaoRadius", &patch.cacaoRadius},
             {"cacaoIntensity", &patch.cacaoIntensity},
             {"cacaoPower", &patch.cacaoPower},
             {"gtaoRadius", &patch.gtaoRadius},
             {"gtaoFalloff", &patch.gtaoFalloff},
             {"gtaoIntensity", &patch.gtaoIntensity},
             {"gtaoPower", &patch.gtaoPower},
             {"gtaoTemporalWeight", &patch.gtaoTemporalWeight},
             {"taaHistoryWeight", &patch.taaHistoryWeight},
             {"taaSharpness", &patch.taaSharpness},
             {"ssrMaxDistance", &patch.ssrMaxDistance},
             {"ssrThickness", &patch.ssrThickness},
             {"ssrMaxRoughness", &patch.ssrMaxRoughness},
             {"ssrIntensity", &patch.ssrIntensity},
             {"ssrHistoryWeight", &patch.ssrHistoryWeight},
             {"ssgiMaxDistance", &patch.ssgiMaxDistance},
             {"ssgiThickness", &patch.ssgiThickness},
             {"ssgiIntensity", &patch.ssgiIntensity},
             {"ssgiRadianceClamp", &patch.ssgiRadianceClamp},
             {"ssgiHistoryWeight", &patch.ssgiHistoryWeight},
             {"ddgiRadianceClamp", &patch.ddgiRadianceClamp},
             {"shadowDistance", &patch.shadowDistance},
             {"maxDrawDistance", &patch.maxDrawDistance},
             {"minProjectedSizePixels", &patch.minProjectedSizePixels},
             {"occlusionDepthBias", &patch.occlusionDepthBias}}) {
        if (*value && !std::isfinite(**value)) {
            throw RenderSettingsError(
                "invalid_render_setting",
                std::string(label) + " must be finite.");
        }
    }

    requireEnumInRange(patch.toneMapper, ToneMapper::Aces, "toneMapper");
    requireEnumInRange(patch.surfaceDebugView,
                       SurfaceDebugView::HistoryValidity,
                       "surfaceDebugView");
    requireEnumInRange(patch.ambientOcclusionMode,
                       AmbientOcclusionMode::Gtao,
                       "ambientOcclusionMode");
    requireEnumInRange(patch.ssaoQuality, SsaoQuality::High, "ssaoQuality");
    requireEnumInRange(patch.cacaoQuality, CacaoQuality::Highest,
                       "cacaoQuality");
    requireEnumInRange(patch.cacaoResolution, CacaoResolution::Half,
                       "cacaoResolution");
    requireEnumInRange(patch.gtaoQuality, GtaoQuality::High, "gtaoQuality");
    requireEnumInRange(patch.temporalAntiAliasingMode,
                       TemporalAntiAliasingMode::Taa,
                       "temporalAntiAliasingMode");
    requireEnumInRange(patch.reflectionMode, ReflectionMode::Ssr,
                       "reflectionMode");
    requireEnumInRange(patch.ssrQuality, SsrQuality::High, "ssrQuality");
    requireEnumInRange(patch.globalIlluminationMode,
                       GlobalIlluminationMode::SsgiDdgi,
                       "globalIlluminationMode");
    requireEnumInRange(patch.ssgiQuality, SsgiQuality::High, "ssgiQuality");
    requireEnumInRange(patch.ddgiDebugView, DdgiDebugView::Classification,
                       "ddgiDebugView");
    requireEnumInRange(patch.screenSpaceDebugView,
                       ScreenSpaceDebugView::SsgiRejection,
                       "screenSpaceDebugView");

    if (!configured_)
        return;

    if (patch.bloomEnabled && *patch.bloomEnabled &&
        !support_.bloom.supported) {
        unsupported("bloom_unsupported", "Compute Bloom", support_.bloom);
    }
    if (patch.occlusionCullingEnabled && *patch.occlusionCullingEnabled &&
        !support_.occlusionCulling.supported) {
        unsupported("occlusion_unsupported", "GPU occlusion culling",
                    support_.occlusionCulling);
    }
    if (patch.surfaceDebugView &&
        *patch.surfaceDebugView != SurfaceDebugView::None &&
        !support_.surfaceData.supported) {
        unsupported("surface_data_unsupported", "Surface data",
                    support_.surfaceData);
    }
    if (patch.ambientOcclusionMode) {
        switch (*patch.ambientOcclusionMode) {
        case AmbientOcclusionMode::Off:
            break;
        case AmbientOcclusionMode::Ssao:
            if (!support_.ssao.supported)
                unsupported("ssao_unsupported", "SSAO", support_.ssao);
            break;
        case AmbientOcclusionMode::Cacao:
            if (!support_.cacao.supported)
                unsupported("cacao_unsupported", "FidelityFX CACAO",
                            support_.cacao);
            break;
        case AmbientOcclusionMode::Gtao:
            if (!support_.gtao.supported)
                unsupported("gtao_unsupported", "GTAO", support_.gtao);
            break;
        }
    }
    if (patch.temporalAntiAliasingMode &&
        *patch.temporalAntiAliasingMode == TemporalAntiAliasingMode::Taa &&
        !support_.taa.supported) {
        unsupported("taa_unsupported", "TAA", support_.taa);
    }
    if (patch.reflectionMode && *patch.reflectionMode == ReflectionMode::Ssr &&
        !support_.ssr.supported) {
        unsupported("ssr_unsupported", "SSR", support_.ssr);
    }
    if (patch.globalIlluminationMode) {
        const GlobalIlluminationMode mode = *patch.globalIlluminationMode;
        if ((mode == GlobalIlluminationMode::Ssgi ||
             mode == GlobalIlluminationMode::SsgiDdgi) &&
            !support_.ssgi.supported) {
            unsupported("ssgi_unsupported", "SSGI", support_.ssgi);
        }
        if ((mode == GlobalIlluminationMode::Ddgi ||
             mode == GlobalIlluminationMode::SsgiDdgi) &&
            !support_.ddgi.supported) {
            unsupported("ddgi_unsupported", "DDGI", support_.ddgi);
        }
    }
    if (patch.surfaceDebugView && patch.screenSpaceDebugView &&
        *patch.surfaceDebugView != SurfaceDebugView::None &&
        *patch.screenSpaceDebugView != ScreenSpaceDebugView::None) {
        throw RenderSettingsError(
            "conflicting_debug_views",
            "Surface and screen-space debug views cannot be active together.");
    }
    if (!patch.screenSpaceDebugView)
        return;

    const ScreenSpaceDebugView view = *patch.screenSpaceDebugView;
    const bool supported =
        view == ScreenSpaceDebugView::None ||
        (view == ScreenSpaceDebugView::NearestDepth &&
         support_.depthPyramid.supported) ||
        (view == ScreenSpaceDebugView::SceneColor &&
         support_.colorPyramid.supported) ||
        ((view == ScreenSpaceDebugView::SsaoRaw ||
          view == ScreenSpaceDebugView::SsaoFiltered) &&
         support_.ssao.supported) ||
        (view == ScreenSpaceDebugView::CacaoOutput &&
         support_.cacao.supported) ||
        (isGtaoDebug(view) && support_.gtao.supported) ||
        (isTaaDebug(view) && support_.taa.supported) ||
        (isSsrDebug(view) && support_.ssr.supported) ||
        (isSsgiDebug(view) && support_.ssgi.supported);
    if (!supported) {
        throw RenderSettingsError(
            "screen_space_unsupported",
            "The requested screen-space debug resource is unavailable.");
    }
}

void RenderSettingsController::normalize(RenderSettings &next) {
    next.shadowReceiverBias =
        glm::clamp(next.shadowReceiverBias, 0.0f, 0.05f);
    next.pointShadowReceiverBiasWorld =
        glm::clamp(next.pointShadowReceiverBiasWorld, 0.0f, 1.0f);
    next.shadowConstantBias =
        glm::clamp(next.shadowConstantBias, 0.0f, 10.0f);
    next.shadowSlopeBias = glm::clamp(next.shadowSlopeBias, 0.0f, 10.0f);
    next.maxPointShadowLights =
        std::min(next.maxPointShadowLights, kMaxPointShadowLights);
    next.maxSpotShadowLights =
        std::min(next.maxSpotShadowLights, kMaxSpotShadowLights);
    next.pointShadowDistance =
        glm::clamp(next.pointShadowDistance, kMinPunctualShadowDistance,
                   kMaxPunctualShadowDistance);
    next.spotShadowDistance =
        glm::clamp(next.spotShadowDistance, kMinPunctualShadowDistance,
                   kMaxPunctualShadowDistance);
    next.exposureEv = glm::clamp(next.exposureEv, -10.0f, 10.0f);
    next.bloomThreshold = glm::clamp(next.bloomThreshold, 0.0f, 20.0f);
    next.bloomSoftKnee = glm::clamp(next.bloomSoftKnee, 0.0f, 1.0f);
    next.bloomIntensity = glm::clamp(next.bloomIntensity, 0.0f, 5.0f);
    next.environmentIntensity =
        glm::clamp(next.environmentIntensity, 0.0f, 100.0f);
    next.environmentRotationRadians =
        std::remainder(next.environmentRotationRadians, glm::two_pi<float>());
    next.surfaceMotionDebugScale =
        glm::clamp(next.surfaceMotionDebugScale, 0.1f, 1024.0f);
    next.ssaoRadius = glm::clamp(next.ssaoRadius, 0.05f, 10.0f);
    next.ssaoBias = glm::clamp(next.ssaoBias, 0.0f, 0.2f);
    next.ssaoIntensity = glm::clamp(next.ssaoIntensity, 0.0f, 4.0f);
    next.ssaoPower = glm::clamp(next.ssaoPower, 0.25f, 4.0f);
    next.cacao.radius = glm::clamp(next.cacao.radius, 0.05f, 10.0f);
    next.cacao.intensity = glm::clamp(next.cacao.intensity, 0.0f, 4.0f);
    next.cacao.power = glm::clamp(next.cacao.power, 0.25f, 4.0f);
    next.gtao.radius = glm::clamp(next.gtao.radius, 0.05f, 10.0f);
    next.gtao.falloff = glm::clamp(next.gtao.falloff, 0.0f, 0.99f);
    next.gtao.intensity = glm::clamp(next.gtao.intensity, 0.0f, 4.0f);
    next.gtao.power = glm::clamp(next.gtao.power, 0.25f, 4.0f);
    next.gtao.temporalWeight =
        glm::clamp(next.gtao.temporalWeight, 0.0f, 0.99f);
    next.taaHistoryWeight =
        glm::clamp(next.taaHistoryWeight, 0.0f, 0.99f);
    next.taaSharpness = glm::clamp(next.taaSharpness, 0.0f, 1.0f);
    next.ssrMaxDistance = glm::clamp(next.ssrMaxDistance, 0.1f, 1000.0f);
    next.ssrThickness = glm::clamp(next.ssrThickness, 0.001f, 10.0f);
    next.ssrMaxRoughness = glm::clamp(next.ssrMaxRoughness, 0.0f, 1.0f);
    next.ssrIntensity = glm::clamp(next.ssrIntensity, 0.0f, 4.0f);
    next.ssrHistoryWeight =
        glm::clamp(next.ssrHistoryWeight, 0.0f, 0.99f);
    next.ssgiMaxDistance =
        glm::clamp(next.ssgiMaxDistance, 0.05f, 1000.0f);
    next.ssgiThickness = glm::clamp(next.ssgiThickness, 0.001f, 10.0f);
    next.ssgiIntensity = glm::clamp(next.ssgiIntensity, 0.0f, 4.0f);
    next.ssgiRadianceClamp =
        glm::clamp(next.ssgiRadianceClamp, 0.1f, 100.0f);
    next.ssgiHistoryWeight =
        glm::clamp(next.ssgiHistoryWeight, 0.0f, 0.99f);
    next.ddgi.radianceClamp =
        glm::clamp(next.ddgi.radianceClamp, 0.1f, 100.0f);
    next.screenSpaceDebugMip = std::min(next.screenSpaceDebugMip, 31u);
    next.culling.shadowDistance =
        glm::clamp(next.culling.shadowDistance,
                   kMinDirectionalShadowDistance,
                   kMaxDirectionalShadowDistance);
    next.culling.maxDrawDistance =
        glm::clamp(next.culling.maxDrawDistance, 0.1f, 1000000.0f);
    next.culling.minProjectedSizePixels =
        glm::clamp(next.culling.minProjectedSizePixels, 0.0f, 256.0f);
    next.culling.occlusionMinCandidates =
        std::min(next.culling.occlusionMinCandidates, 65536u);
    next.culling.occlusionDepthBias =
        glm::clamp(next.culling.occlusionDepthBias, 0.0f, 0.05f);
}

void RenderSettingsController::apply(const RenderSettingsPatch &patch) {
    validatePatch(patch);
    RenderSettings next = settings_;
    applyRenderSettingsPatch(next, patch);
    if (patch.surfaceDebugView &&
        *patch.surfaceDebugView != SurfaceDebugView::None) {
        next.screenSpaceDebugView = ScreenSpaceDebugView::None;
    }
    if (patch.screenSpaceDebugView &&
        *patch.screenSpaceDebugView != ScreenSpaceDebugView::None) {
        next.surfaceDebugView = SurfaceDebugView::None;
    }
    normalize(next);

    if (configured_ && next.cacao.resolution != settings_.cacao.resolution &&
        support_.cacao.supported && callbacks_.reconfigureCacao) {
        callbacks_.reconfigureCacao(next.cacao.resolution);
    }
    settings_ = next;
}

RenderSettings RenderSettingsController::activeSettings() const {
    RenderSettings active = settings_;
    const ShaderVariant &shader = currentShaderVariant();
    active.bloomEnabled = active.bloomEnabled && support_.bloom.supported &&
                          shader.supportsBloom;
    active.culling.occlusionEnabled =
        active.culling.occlusionEnabled && support_.occlusionCulling.supported;
    if (!support_.surfaceData.supported)
        active.surfaceDebugView = SurfaceDebugView::None;

    const bool screenSpaceShader = shader.supportsScreenSpace;
    if (!screenSpaceShader ||
        (active.ambientOcclusionMode == AmbientOcclusionMode::Ssao &&
         !support_.ssao.supported) ||
        (active.ambientOcclusionMode == AmbientOcclusionMode::Cacao &&
         !support_.cacao.supported) ||
        (active.ambientOcclusionMode == AmbientOcclusionMode::Gtao &&
         !support_.gtao.supported)) {
        active.ambientOcclusionMode = AmbientOcclusionMode::Off;
    }
    if (!support_.taa.supported)
        active.temporalAntiAliasingMode = TemporalAntiAliasingMode::Off;
    if (!screenSpaceShader || !support_.ssr.supported)
        active.reflectionMode = ReflectionMode::IblOnly;

    const bool ssgi = screenSpaceShader && support_.ssgi.supported;
    const bool ddgi = shader.supportsDdgi && support_.ddgi.supported;
    switch (active.globalIlluminationMode) {
    case GlobalIlluminationMode::AmbientOrIbl:
        break;
    case GlobalIlluminationMode::Ssgi:
        if (!ssgi)
            active.globalIlluminationMode = GlobalIlluminationMode::AmbientOrIbl;
        break;
    case GlobalIlluminationMode::Ddgi:
        if (!ddgi)
            active.globalIlluminationMode = GlobalIlluminationMode::AmbientOrIbl;
        break;
    case GlobalIlluminationMode::SsgiDdgi:
        active.globalIlluminationMode =
            ssgi && ddgi
                ? GlobalIlluminationMode::SsgiDdgi
                : ssgi ? GlobalIlluminationMode::Ssgi
                       : ddgi ? GlobalIlluminationMode::Ddgi
                              : GlobalIlluminationMode::AmbientOrIbl;
        break;
    }
    return active;
}

RenderSettingsSnapshot RenderSettingsController::snapshot() const {
    RenderSettingsSnapshot result;
    result.requested = settings_;
    result.active = activeSettings();
    result.support = support_;
    result.runtime = runtime_;
    const ShaderVariant &shader = currentShaderVariant();
    result.shaderVariantId = shader.id;
    result.shaderDisplayName = shader.displayName;
    return result;
}

} // namespace vkr
