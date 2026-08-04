#pragma once

#include <optional>
#include <string_view>

namespace vkr {

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
