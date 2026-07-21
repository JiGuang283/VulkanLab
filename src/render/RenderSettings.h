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
};

struct RenderSettingsPatch {
    std::optional<bool>       shadowsEnabled;
    std::optional<float>      shadowReceiverBias;
    std::optional<float>      shadowConstantBias;
    std::optional<float>      shadowSlopeBias;
    std::optional<float>      exposureEv;
    std::optional<ToneMapper> toneMapper;
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
}

} // namespace vkr
