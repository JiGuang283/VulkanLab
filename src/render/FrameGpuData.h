#pragma once

#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace vkr {

inline constexpr uint32_t kMaxSceneLights = 256;

struct alignas(16) GpuLight {
    glm::vec4 positionRange;
    glm::vec4 directionInnerCos;
    glm::vec4 colorIntensity;
    glm::vec4 params;
};

struct alignas(16) GlobalFrameUbo {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 inverseViewProjection;
    glm::vec4 cameraPosWS;
    glm::vec4 ambientColorIntensity;
    glm::uvec4 lightCounts;
    glm::mat4 directionalShadowViewProj{1.0f};
    glm::vec4 shadowParams{0.0f};
    glm::vec4 environmentParams{0.0f};
};

struct alignas(16) ToneMapPushConstants {
    float exposureEv = 0.0f;
    float bloomIntensity = 0.0f;
    uint32_t toneMapper = 0;
    uint32_t encodeGamma = 0;
    uint32_t applyExposure = 0;
    uint32_t applyBloom = 0;
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;
};

struct alignas(16) BloomPushConstants {
    float threshold = 1.0f;
    float softKnee = 0.5f;
    float filterRadius = 1.0f;
    uint32_t applyThreshold = 0;
};

static_assert(std::is_standard_layout_v<GpuLight>);
static_assert(std::is_standard_layout_v<GlobalFrameUbo>);
static_assert(sizeof(GpuLight) == 64);
static_assert(offsetof(GpuLight, positionRange) == 0);
static_assert(offsetof(GpuLight, directionInnerCos) == 16);
static_assert(offsetof(GpuLight, colorIntensity) == 32);
static_assert(offsetof(GpuLight, params) == 48);
static_assert(sizeof(GlobalFrameUbo) == 336);
static_assert(offsetof(GlobalFrameUbo, view) == 0);
static_assert(offsetof(GlobalFrameUbo, proj) == 64);
static_assert(offsetof(GlobalFrameUbo, inverseViewProjection) == 128);
static_assert(offsetof(GlobalFrameUbo, cameraPosWS) == 192);
static_assert(offsetof(GlobalFrameUbo, ambientColorIntensity) == 208);
static_assert(offsetof(GlobalFrameUbo, lightCounts) == 224);
static_assert(offsetof(GlobalFrameUbo, directionalShadowViewProj) == 240);
static_assert(offsetof(GlobalFrameUbo, shadowParams) == 304);
static_assert(offsetof(GlobalFrameUbo, environmentParams) == 320);
static_assert(sizeof(ToneMapPushConstants) == 32);
static_assert(offsetof(ToneMapPushConstants, exposureEv) == 0);
static_assert(offsetof(ToneMapPushConstants, bloomIntensity) == 4);
static_assert(offsetof(ToneMapPushConstants, toneMapper) == 8);
static_assert(offsetof(ToneMapPushConstants, encodeGamma) == 12);
static_assert(offsetof(ToneMapPushConstants, applyExposure) == 16);
static_assert(offsetof(ToneMapPushConstants, applyBloom) == 20);
static_assert(sizeof(BloomPushConstants) == 16);

} // namespace vkr
