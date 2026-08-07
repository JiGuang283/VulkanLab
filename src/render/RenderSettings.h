#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace vkr {

enum class SurfaceDebugView {
    None,
    Normal,
    Roughness,
    Motion,
    HistoryValidity,
};

enum class AmbientOcclusionMode {
    Off,
    Ssao,
    Cacao,
    Gtao,
};

enum class TemporalAntiAliasingMode {
    Off,
    Taa,
};

enum class ReflectionMode {
    IblOnly,
    Ssr,
};

inline const char *reflectionModeName(ReflectionMode mode) {
    return mode == ReflectionMode::Ssr ? "ssr" : "ibl-only";
}

inline std::optional<ReflectionMode>
reflectionModeFromName(std::string_view name) {
    if (name == "ibl-only")
        return ReflectionMode::IblOnly;
    if (name == "ssr")
        return ReflectionMode::Ssr;
    return std::nullopt;
}

enum class SsrQuality {
    Low,
    Medium,
    High,
};

inline const char *ssrQualityName(SsrQuality quality) {
    switch (quality) {
    case SsrQuality::Low: return "low";
    case SsrQuality::Medium: return "medium";
    case SsrQuality::High: return "high";
    }
    return "medium";
}

inline std::optional<SsrQuality> ssrQualityFromName(std::string_view name) {
    if (name == "low") return SsrQuality::Low;
    if (name == "medium") return SsrQuality::Medium;
    if (name == "high") return SsrQuality::High;
    return std::nullopt;
}

enum class GlobalIlluminationMode {
    AmbientOrIbl,
    Ssgi,
};

inline const char *globalIlluminationModeName(GlobalIlluminationMode mode) {
    return mode == GlobalIlluminationMode::Ssgi ? "ssgi"
                                                : "ambient-or-ibl";
}

inline std::optional<GlobalIlluminationMode>
globalIlluminationModeFromName(std::string_view name) {
    if (name == "ambient-or-ibl")
        return GlobalIlluminationMode::AmbientOrIbl;
    if (name == "ssgi")
        return GlobalIlluminationMode::Ssgi;
    return std::nullopt;
}

enum class SsgiQuality {
    Low,
    Medium,
    High,
};

inline const char *ssgiQualityName(SsgiQuality quality) {
    switch (quality) {
    case SsgiQuality::Low: return "low";
    case SsgiQuality::Medium: return "medium";
    case SsgiQuality::High: return "high";
    }
    return "medium";
}

inline std::optional<SsgiQuality> ssgiQualityFromName(std::string_view name) {
    if (name == "low") return SsgiQuality::Low;
    if (name == "medium") return SsgiQuality::Medium;
    if (name == "high") return SsgiQuality::High;
    return std::nullopt;
}

inline const char *
temporalAntiAliasingModeName(TemporalAntiAliasingMode mode) {
    return mode == TemporalAntiAliasingMode::Taa ? "taa" : "off";
}

inline std::optional<TemporalAntiAliasingMode>
temporalAntiAliasingModeFromName(std::string_view name) {
    if (name == "off")
        return TemporalAntiAliasingMode::Off;
    if (name == "taa")
        return TemporalAntiAliasingMode::Taa;
    return std::nullopt;
}

inline const char *ambientOcclusionModeName(AmbientOcclusionMode mode) {
    switch (mode) {
    case AmbientOcclusionMode::Off:
        return "off";
    case AmbientOcclusionMode::Ssao:
        return "ssao";
    case AmbientOcclusionMode::Cacao:
        return "cacao";
    case AmbientOcclusionMode::Gtao:
        return "gtao";
    }
    return "off";
}

inline std::optional<AmbientOcclusionMode>
ambientOcclusionModeFromName(std::string_view name) {
    if (name == "off")
        return AmbientOcclusionMode::Off;
    if (name == "ssao")
        return AmbientOcclusionMode::Ssao;
    if (name == "cacao")
        return AmbientOcclusionMode::Cacao;
    if (name == "gtao")
        return AmbientOcclusionMode::Gtao;
    return std::nullopt;
}

enum class GtaoQuality {
    Low,
    Medium,
    High,
};

inline const char *gtaoQualityName(GtaoQuality quality) {
    switch (quality) {
    case GtaoQuality::Low:
        return "low";
    case GtaoQuality::Medium:
        return "medium";
    case GtaoQuality::High:
        return "high";
    }
    return "medium";
}

inline std::optional<GtaoQuality> gtaoQualityFromName(std::string_view name) {
    if (name == "low")
        return GtaoQuality::Low;
    if (name == "medium")
        return GtaoQuality::Medium;
    if (name == "high")
        return GtaoQuality::High;
    return std::nullopt;
}

struct GtaoSettings {
    GtaoQuality quality = GtaoQuality::Medium;
    float radius = 1.0f;
    float falloff = 0.85f;
    float intensity = 1.0f;
    float power = 1.5f;
    float temporalWeight = 0.9f;
};

enum class CacaoQuality {
    Lowest,
    Low,
    Medium,
    High,
    Highest,
};

inline const char *cacaoQualityName(CacaoQuality quality) {
    switch (quality) {
    case CacaoQuality::Lowest:
        return "lowest";
    case CacaoQuality::Low:
        return "low";
    case CacaoQuality::Medium:
        return "medium";
    case CacaoQuality::High:
        return "high";
    case CacaoQuality::Highest:
        return "highest";
    }
    return "high";
}

inline std::optional<CacaoQuality> cacaoQualityFromName(std::string_view name) {
    if (name == "lowest")
        return CacaoQuality::Lowest;
    if (name == "low")
        return CacaoQuality::Low;
    if (name == "medium")
        return CacaoQuality::Medium;
    if (name == "high")
        return CacaoQuality::High;
    if (name == "highest")
        return CacaoQuality::Highest;
    return std::nullopt;
}

enum class CacaoResolution {
    Native,
    Half,
};

inline const char *cacaoResolutionName(CacaoResolution resolution) {
    return resolution == CacaoResolution::Native ? "native" : "half";
}

inline std::optional<CacaoResolution>
cacaoResolutionFromName(std::string_view name) {
    if (name == "native")
        return CacaoResolution::Native;
    if (name == "half")
        return CacaoResolution::Half;
    return std::nullopt;
}

struct CacaoSettings {
    CacaoQuality quality = CacaoQuality::High;
    CacaoResolution resolution = CacaoResolution::Half;
    float radius = 1.2f;
    float intensity = 1.0f;
    float power = 1.5f;
};

enum class SsaoQuality {
    Low,
    Medium,
    High,
};

inline const char *ssaoQualityName(SsaoQuality quality) {
    switch (quality) {
    case SsaoQuality::Low:
        return "low";
    case SsaoQuality::Medium:
        return "medium";
    case SsaoQuality::High:
        return "high";
    }
    return "medium";
}

inline std::optional<SsaoQuality> ssaoQualityFromName(std::string_view name) {
    if (name == "low")
        return SsaoQuality::Low;
    if (name == "medium")
        return SsaoQuality::Medium;
    if (name == "high")
        return SsaoQuality::High;
    return std::nullopt;
}

enum class ScreenSpaceDebugView {
    None,
    NearestDepth,
    SceneColor,
    SsaoRaw,
    SsaoFiltered,
    CacaoOutput,
    GtaoRaw,
    GtaoTemporal,
    GtaoFiltered,
    GtaoRejection,
    GtaoHistoryWeight,
    TaaHistory,
    TaaRejection,
    TaaHistoryWeight,
    SsrRaw,
    SsrTemporal,
    SsrFiltered,
    SsrConfidence,
    SsrRejection,
    SsgiRaw,
    SsgiTemporal,
    SsgiFiltered,
    SsgiConfidence,
    SsgiVariance,
    SsgiRejection,
};

inline const char *screenSpaceDebugViewName(ScreenSpaceDebugView view) {
    switch (view) {
    case ScreenSpaceDebugView::None:
        return "none";
    case ScreenSpaceDebugView::NearestDepth:
        return "nearest-depth";
    case ScreenSpaceDebugView::SceneColor:
        return "scene-color";
    case ScreenSpaceDebugView::SsaoRaw:
        return "ssao-raw";
    case ScreenSpaceDebugView::SsaoFiltered:
        return "ssao-filtered";
    case ScreenSpaceDebugView::CacaoOutput:
        return "cacao-output";
    case ScreenSpaceDebugView::GtaoRaw:
        return "gtao-raw";
    case ScreenSpaceDebugView::GtaoTemporal:
        return "gtao-temporal";
    case ScreenSpaceDebugView::GtaoFiltered:
        return "gtao-filtered";
    case ScreenSpaceDebugView::GtaoRejection:
        return "gtao-rejection";
    case ScreenSpaceDebugView::GtaoHistoryWeight:
        return "gtao-history-weight";
    case ScreenSpaceDebugView::TaaHistory:
        return "taa-history";
    case ScreenSpaceDebugView::TaaRejection:
        return "taa-rejection";
    case ScreenSpaceDebugView::TaaHistoryWeight:
        return "taa-history-weight";
    case ScreenSpaceDebugView::SsrRaw:
        return "ssr-raw";
    case ScreenSpaceDebugView::SsrTemporal:
        return "ssr-temporal";
    case ScreenSpaceDebugView::SsrFiltered:
        return "ssr-filtered";
    case ScreenSpaceDebugView::SsrConfidence:
        return "ssr-confidence";
    case ScreenSpaceDebugView::SsrRejection:
        return "ssr-rejection";
    case ScreenSpaceDebugView::SsgiRaw:
        return "ssgi-raw";
    case ScreenSpaceDebugView::SsgiTemporal:
        return "ssgi-temporal";
    case ScreenSpaceDebugView::SsgiFiltered:
        return "ssgi-filtered";
    case ScreenSpaceDebugView::SsgiConfidence:
        return "ssgi-confidence";
    case ScreenSpaceDebugView::SsgiVariance:
        return "ssgi-variance";
    case ScreenSpaceDebugView::SsgiRejection:
        return "ssgi-rejection";
    }
    return "none";
}

inline std::optional<ScreenSpaceDebugView>
screenSpaceDebugViewFromName(std::string_view name) {
    if (name == "none")
        return ScreenSpaceDebugView::None;
    if (name == "nearest-depth")
        return ScreenSpaceDebugView::NearestDepth;
    if (name == "scene-color")
        return ScreenSpaceDebugView::SceneColor;
    if (name == "ssao-raw")
        return ScreenSpaceDebugView::SsaoRaw;
    if (name == "ssao-filtered")
        return ScreenSpaceDebugView::SsaoFiltered;
    if (name == "cacao-output")
        return ScreenSpaceDebugView::CacaoOutput;
    if (name == "gtao-raw")
        return ScreenSpaceDebugView::GtaoRaw;
    if (name == "gtao-temporal")
        return ScreenSpaceDebugView::GtaoTemporal;
    if (name == "gtao-filtered")
        return ScreenSpaceDebugView::GtaoFiltered;
    if (name == "gtao-rejection")
        return ScreenSpaceDebugView::GtaoRejection;
    if (name == "gtao-history-weight")
        return ScreenSpaceDebugView::GtaoHistoryWeight;
    if (name == "taa-history")
        return ScreenSpaceDebugView::TaaHistory;
    if (name == "taa-rejection")
        return ScreenSpaceDebugView::TaaRejection;
    if (name == "taa-history-weight")
        return ScreenSpaceDebugView::TaaHistoryWeight;
    if (name == "ssr-raw")
        return ScreenSpaceDebugView::SsrRaw;
    if (name == "ssr-temporal")
        return ScreenSpaceDebugView::SsrTemporal;
    if (name == "ssr-filtered")
        return ScreenSpaceDebugView::SsrFiltered;
    if (name == "ssr-confidence")
        return ScreenSpaceDebugView::SsrConfidence;
    if (name == "ssr-rejection")
        return ScreenSpaceDebugView::SsrRejection;
    if (name == "ssgi-raw")
        return ScreenSpaceDebugView::SsgiRaw;
    if (name == "ssgi-temporal")
        return ScreenSpaceDebugView::SsgiTemporal;
    if (name == "ssgi-filtered")
        return ScreenSpaceDebugView::SsgiFiltered;
    if (name == "ssgi-confidence")
        return ScreenSpaceDebugView::SsgiConfidence;
    if (name == "ssgi-variance")
        return ScreenSpaceDebugView::SsgiVariance;
    if (name == "ssgi-rejection")
        return ScreenSpaceDebugView::SsgiRejection;
    return std::nullopt;
}

inline const char *surfaceDebugViewName(SurfaceDebugView view) {
    switch (view) {
    case SurfaceDebugView::None:
        return "none";
    case SurfaceDebugView::Normal:
        return "normal";
    case SurfaceDebugView::Roughness:
        return "roughness";
    case SurfaceDebugView::Motion:
        return "motion";
    case SurfaceDebugView::HistoryValidity:
        return "history-validity";
    }
    return "none";
}

inline std::optional<SurfaceDebugView>
surfaceDebugViewFromName(std::string_view name) {
    if (name == "none")
        return SurfaceDebugView::None;
    if (name == "normal")
        return SurfaceDebugView::Normal;
    if (name == "roughness")
        return SurfaceDebugView::Roughness;
    if (name == "motion")
        return SurfaceDebugView::Motion;
    if (name == "history-validity")
        return SurfaceDebugView::HistoryValidity;
    return std::nullopt;
}

struct CullingSettings {
    bool  frustumEnabled = true;
    bool  shadowCullingEnabled = true;
    float shadowDistance = 200.0f;
    bool  distanceEnabled = false;
    float maxDrawDistance = 1000.0f;
    bool  smallObjectEnabled = false;
    float minProjectedSizePixels = 1.0f;
    bool  occlusionEnabled = true;
    float occlusionDepthBias = 0.0005f;
};

enum class ToneMapper {
    PassThrough,
    Reinhard,
    Aces,
};

inline const char *toneMapperName(ToneMapper toneMapper) {
    switch (toneMapper) {
    case ToneMapper::PassThrough:
        return "passthrough";
    case ToneMapper::Reinhard:
        return "reinhard";
    case ToneMapper::Aces:
        return "aces";
    }
    return "aces";
}

inline std::optional<ToneMapper> toneMapperFromName(std::string_view name) {
    if (name == "passthrough")
        return ToneMapper::PassThrough;
    if (name == "reinhard")
        return ToneMapper::Reinhard;
    if (name == "aces")
        return ToneMapper::Aces;
    return std::nullopt;
}

struct RenderSettings {
    bool       shadowsEnabled = true;
    float      shadowReceiverBias = 0.0015f;
    float      shadowConstantBias = 1.25f;
    float      shadowSlopeBias = 1.75f;
    float      exposureEv = 0.0f;
    ToneMapper toneMapper = ToneMapper::Aces;
    bool       bloomEnabled = false;
    float      bloomThreshold = 1.0f;
    float      bloomSoftKnee = 0.5f;
    float      bloomIntensity = 0.1f;
    bool       iblEnabled = false;
    bool       skyboxEnabled = false;
    float      environmentIntensity = 1.0f;
    float environmentRotationRadians = 0.0f;
    SurfaceDebugView surfaceDebugView = SurfaceDebugView::None;
    float surfaceMotionDebugScale = 32.0f;
    AmbientOcclusionMode ambientOcclusionMode = AmbientOcclusionMode::Off;
    SsaoQuality ssaoQuality = SsaoQuality::Medium;
    float ssaoRadius = 0.5f;
    float ssaoBias = 0.025f;
    float ssaoIntensity = 1.0f;
    float ssaoPower = 1.5f;
    CacaoSettings cacao{};
    GtaoSettings gtao{};
    TemporalAntiAliasingMode temporalAntiAliasingMode =
        TemporalAntiAliasingMode::Off;
    float taaHistoryWeight = 0.9f;
    float taaSharpness = 0.1f;
    ReflectionMode reflectionMode = ReflectionMode::IblOnly;
    SsrQuality ssrQuality = SsrQuality::Medium;
    float ssrMaxDistance = 50.0f;
    float ssrThickness = 0.2f;
    float ssrMaxRoughness = 0.8f;
    float ssrIntensity = 1.0f;
    float ssrHistoryWeight = 0.9f;
    GlobalIlluminationMode globalIlluminationMode =
        GlobalIlluminationMode::AmbientOrIbl;
    SsgiQuality ssgiQuality = SsgiQuality::Medium;
    float ssgiMaxDistance = 12.0f;
    float ssgiThickness = 0.25f;
    float ssgiIntensity = 1.0f;
    float ssgiRadianceClamp = 10.0f;
    float ssgiHistoryWeight = 0.9f;
    ScreenSpaceDebugView screenSpaceDebugView = ScreenSpaceDebugView::None;
    uint32_t screenSpaceDebugMip = 0;
    CullingSettings culling{};
};

struct RenderSettingsPatch {
    std::optional<bool>       shadowsEnabled;
    std::optional<float>      shadowReceiverBias;
    std::optional<float>      shadowConstantBias;
    std::optional<float>      shadowSlopeBias;
    std::optional<float>      exposureEv;
    std::optional<ToneMapper> toneMapper;
    std::optional<bool>       bloomEnabled;
    std::optional<float>      bloomThreshold;
    std::optional<float>      bloomSoftKnee;
    std::optional<float>      bloomIntensity;
    std::optional<bool>       iblEnabled;
    std::optional<bool>       skyboxEnabled;
    std::optional<float>      environmentIntensity;
    std::optional<float> environmentRotationRadians;
    std::optional<SurfaceDebugView> surfaceDebugView;
    std::optional<float> surfaceMotionDebugScale;
    std::optional<AmbientOcclusionMode> ambientOcclusionMode;
    std::optional<SsaoQuality> ssaoQuality;
    std::optional<float> ssaoRadius;
    std::optional<float> ssaoBias;
    std::optional<float> ssaoIntensity;
    std::optional<float> ssaoPower;
    std::optional<CacaoQuality> cacaoQuality;
    std::optional<CacaoResolution> cacaoResolution;
    std::optional<float> cacaoRadius;
    std::optional<float> cacaoIntensity;
    std::optional<float> cacaoPower;
    std::optional<GtaoQuality> gtaoQuality;
    std::optional<float> gtaoRadius;
    std::optional<float> gtaoFalloff;
    std::optional<float> gtaoIntensity;
    std::optional<float> gtaoPower;
    std::optional<float> gtaoTemporalWeight;
    std::optional<TemporalAntiAliasingMode> temporalAntiAliasingMode;
    std::optional<float> taaHistoryWeight;
    std::optional<float> taaSharpness;
    std::optional<ReflectionMode> reflectionMode;
    std::optional<SsrQuality> ssrQuality;
    std::optional<float> ssrMaxDistance;
    std::optional<float> ssrThickness;
    std::optional<float> ssrMaxRoughness;
    std::optional<float> ssrIntensity;
    std::optional<float> ssrHistoryWeight;
    std::optional<GlobalIlluminationMode> globalIlluminationMode;
    std::optional<SsgiQuality> ssgiQuality;
    std::optional<float> ssgiMaxDistance;
    std::optional<float> ssgiThickness;
    std::optional<float> ssgiIntensity;
    std::optional<float> ssgiRadianceClamp;
    std::optional<float> ssgiHistoryWeight;
    std::optional<ScreenSpaceDebugView> screenSpaceDebugView;
    std::optional<uint32_t> screenSpaceDebugMip;
    std::optional<bool>  frustumCullingEnabled;
    std::optional<bool>  shadowCullingEnabled;
    std::optional<float> shadowDistance;
    std::optional<bool>  distanceCullingEnabled;
    std::optional<float> maxDrawDistance;
    std::optional<bool>  smallObjectCullingEnabled;
    std::optional<float> minProjectedSizePixels;
    std::optional<bool>  occlusionCullingEnabled;
    std::optional<float> occlusionDepthBias;
};

inline void applyRenderSettingsPatch(RenderSettings &settings,
                                     const RenderSettingsPatch &patch) {
    if (patch.shadowsEnabled)
        settings.shadowsEnabled = *patch.shadowsEnabled;
    if (patch.shadowReceiverBias)
        settings.shadowReceiverBias = *patch.shadowReceiverBias;
    if (patch.shadowConstantBias)
        settings.shadowConstantBias = *patch.shadowConstantBias;
    if (patch.shadowSlopeBias)
        settings.shadowSlopeBias = *patch.shadowSlopeBias;
    if (patch.exposureEv)
        settings.exposureEv = *patch.exposureEv;
    if (patch.toneMapper)
        settings.toneMapper = *patch.toneMapper;
    if (patch.bloomEnabled)
        settings.bloomEnabled = *patch.bloomEnabled;
    if (patch.bloomThreshold)
        settings.bloomThreshold = *patch.bloomThreshold;
    if (patch.bloomSoftKnee)
        settings.bloomSoftKnee = *patch.bloomSoftKnee;
    if (patch.bloomIntensity)
        settings.bloomIntensity = *patch.bloomIntensity;
    if (patch.iblEnabled)
        settings.iblEnabled = *patch.iblEnabled;
    if (patch.skyboxEnabled)
        settings.skyboxEnabled = *patch.skyboxEnabled;
    if (patch.environmentIntensity)
        settings.environmentIntensity = *patch.environmentIntensity;
    if (patch.environmentRotationRadians) {
        settings.environmentRotationRadians =
            *patch.environmentRotationRadians;
    }
    if (patch.surfaceDebugView)
        settings.surfaceDebugView = *patch.surfaceDebugView;
    if (patch.surfaceMotionDebugScale)
        settings.surfaceMotionDebugScale = *patch.surfaceMotionDebugScale;
    if (patch.ambientOcclusionMode)
        settings.ambientOcclusionMode = *patch.ambientOcclusionMode;
    if (patch.ssaoQuality)
        settings.ssaoQuality = *patch.ssaoQuality;
    if (patch.ssaoRadius)
        settings.ssaoRadius = *patch.ssaoRadius;
    if (patch.ssaoBias)
        settings.ssaoBias = *patch.ssaoBias;
    if (patch.ssaoIntensity)
        settings.ssaoIntensity = *patch.ssaoIntensity;
    if (patch.ssaoPower)
        settings.ssaoPower = *patch.ssaoPower;
    if (patch.cacaoQuality)
        settings.cacao.quality = *patch.cacaoQuality;
    if (patch.cacaoResolution)
        settings.cacao.resolution = *patch.cacaoResolution;
    if (patch.cacaoRadius)
        settings.cacao.radius = *patch.cacaoRadius;
    if (patch.cacaoIntensity)
        settings.cacao.intensity = *patch.cacaoIntensity;
    if (patch.cacaoPower)
        settings.cacao.power = *patch.cacaoPower;
    if (patch.gtaoQuality)
        settings.gtao.quality = *patch.gtaoQuality;
    if (patch.gtaoRadius)
        settings.gtao.radius = *patch.gtaoRadius;
    if (patch.gtaoFalloff)
        settings.gtao.falloff = *patch.gtaoFalloff;
    if (patch.gtaoIntensity)
        settings.gtao.intensity = *patch.gtaoIntensity;
    if (patch.gtaoPower)
        settings.gtao.power = *patch.gtaoPower;
    if (patch.gtaoTemporalWeight)
        settings.gtao.temporalWeight = *patch.gtaoTemporalWeight;
    if (patch.temporalAntiAliasingMode) {
        settings.temporalAntiAliasingMode =
            *patch.temporalAntiAliasingMode;
    }
    if (patch.taaHistoryWeight)
        settings.taaHistoryWeight = *patch.taaHistoryWeight;
    if (patch.taaSharpness)
        settings.taaSharpness = *patch.taaSharpness;
    if (patch.reflectionMode)
        settings.reflectionMode = *patch.reflectionMode;
    if (patch.ssrQuality)
        settings.ssrQuality = *patch.ssrQuality;
    if (patch.ssrMaxDistance)
        settings.ssrMaxDistance = *patch.ssrMaxDistance;
    if (patch.ssrThickness)
        settings.ssrThickness = *patch.ssrThickness;
    if (patch.ssrMaxRoughness)
        settings.ssrMaxRoughness = *patch.ssrMaxRoughness;
    if (patch.ssrIntensity)
        settings.ssrIntensity = *patch.ssrIntensity;
    if (patch.ssrHistoryWeight)
        settings.ssrHistoryWeight = *patch.ssrHistoryWeight;
    if (patch.globalIlluminationMode)
        settings.globalIlluminationMode = *patch.globalIlluminationMode;
    if (patch.ssgiQuality)
        settings.ssgiQuality = *patch.ssgiQuality;
    if (patch.ssgiMaxDistance)
        settings.ssgiMaxDistance = *patch.ssgiMaxDistance;
    if (patch.ssgiThickness)
        settings.ssgiThickness = *patch.ssgiThickness;
    if (patch.ssgiIntensity)
        settings.ssgiIntensity = *patch.ssgiIntensity;
    if (patch.ssgiRadianceClamp)
        settings.ssgiRadianceClamp = *patch.ssgiRadianceClamp;
    if (patch.ssgiHistoryWeight)
        settings.ssgiHistoryWeight = *patch.ssgiHistoryWeight;
    if (patch.screenSpaceDebugView)
        settings.screenSpaceDebugView = *patch.screenSpaceDebugView;
    if (patch.screenSpaceDebugMip)
        settings.screenSpaceDebugMip = *patch.screenSpaceDebugMip;
    if (patch.frustumCullingEnabled)
        settings.culling.frustumEnabled = *patch.frustumCullingEnabled;
    if (patch.shadowCullingEnabled)
        settings.culling.shadowCullingEnabled = *patch.shadowCullingEnabled;
    if (patch.shadowDistance)
        settings.culling.shadowDistance = *patch.shadowDistance;
    if (patch.distanceCullingEnabled)
        settings.culling.distanceEnabled = *patch.distanceCullingEnabled;
    if (patch.maxDrawDistance)
        settings.culling.maxDrawDistance = *patch.maxDrawDistance;
    if (patch.smallObjectCullingEnabled)
        settings.culling.smallObjectEnabled = *patch.smallObjectCullingEnabled;
    if (patch.minProjectedSizePixels)
        settings.culling.minProjectedSizePixels =
            *patch.minProjectedSizePixels;
    if (patch.occlusionCullingEnabled)
        settings.culling.occlusionEnabled = *patch.occlusionCullingEnabled;
    if (patch.occlusionDepthBias)
        settings.culling.occlusionDepthBias = *patch.occlusionDepthBias;
}

} // namespace vkr
