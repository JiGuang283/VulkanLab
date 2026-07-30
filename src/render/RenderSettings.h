#pragma once

#include <optional>
#include <string_view>

namespace vkr {

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
}

} // namespace vkr
