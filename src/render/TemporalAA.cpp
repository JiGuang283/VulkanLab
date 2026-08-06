#include "render/TemporalAA.h"

#include <algorithm>

namespace vkr {

namespace {

float halton(uint32_t index, uint32_t base) {
    float result = 0.0f;
    float fraction = 1.0f;
    while (index > 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

} // namespace

bool isTaaDebugView(ScreenSpaceDebugView view) {
    return view == ScreenSpaceDebugView::TaaHistory ||
           view == ScreenSpaceDebugView::TaaRejection ||
           view == ScreenSpaceDebugView::TaaHistoryWeight;
}

bool taaPassRequested(const RenderSettings &settings) {
    return settings.temporalAntiAliasingMode ==
               TemporalAntiAliasingMode::Taa ||
           isTaaDebugView(settings.screenSpaceDebugView);
}

TemporalJitter temporalJitter(uint64_t frameSerial, VkExtent2D extent,
                              bool enabled) {
    TemporalJitter result{};
    if (!enabled || extent.width == 0 || extent.height == 0)
        return result;

    constexpr uint32_t kPhaseCount = 8;
    result.phase = static_cast<uint32_t>(frameSerial % kPhaseCount);
    const uint32_t sequenceIndex = result.phase + 1u;
    result.pixels =
        glm::vec2(halton(sequenceIndex, 2u),
                  halton(sequenceIndex, 3u)) -
        glm::vec2(0.5f);
    result.ndc = glm::vec2(
        2.0f * result.pixels.x / static_cast<float>(extent.width),
        2.0f * result.pixels.y / static_cast<float>(extent.height));
    return result;
}

glm::mat4 applyProjectionJitter(const glm::mat4 &projection,
                                const glm::vec2 &jitterNdc) {
    glm::mat4 clipTranslation(1.0f);
    clipTranslation[3][0] = jitterNdc.x;
    clipTranslation[3][1] = jitterNdc.y;
    return clipTranslation * projection;
}

} // namespace vkr
