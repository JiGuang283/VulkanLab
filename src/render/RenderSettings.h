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
};

inline const char *ambientOcclusionModeName(AmbientOcclusionMode mode) {
    switch (mode) {
    case AmbientOcclusionMode::Off:
        return "off";
    case AmbientOcclusionMode::Ssao:
        return "ssao";
    }
    return "off";
}

inline std::optional<AmbientOcclusionMode>
ambientOcclusionModeFromName(std::string_view name) {
    if (name == "off")
        return AmbientOcclusionMode::Off;
    if (name == "ssao")
        return AmbientOcclusionMode::Ssao;
    return std::nullopt;
}

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
