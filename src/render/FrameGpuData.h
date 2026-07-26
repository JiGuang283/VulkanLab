#pragma once

#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace vkr {

inline constexpr uint32_t kMaxDirectionalLights = 1;
inline constexpr uint32_t kMaxPunctualLights = 8;

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
    glm::vec4 lightCounts;
    GpuLight directionalLights[kMaxDirectionalLights];
    GpuLight punctualLights[kMaxPunctualLights];
    glm::mat4 directionalShadowViewProj{1.0f};
    glm::vec4 shadowParams{0.0f};
    glm::vec4 environmentParams{0.0f};
};

struct alignas(16) ToneMapPushConstants {
    float exposureEv = 0.0f;
    uint32_t toneMapper = 0;
    uint32_t encodeGamma = 0;
    uint32_t applyExposure = 0;
};

static_assert(std::is_standard_layout_v<GpuLight>);
static_assert(std::is_standard_layout_v<GlobalFrameUbo>);
static_assert(sizeof(GpuLight) == 64);
static_assert(offsetof(GpuLight, positionRange) == 0);
static_assert(offsetof(GpuLight, directionInnerCos) == 16);
static_assert(offsetof(GpuLight, colorIntensity) == 32);
static_assert(offsetof(GpuLight, params) == 48);
static_assert(sizeof(GlobalFrameUbo) == 912);
static_assert(offsetof(GlobalFrameUbo, view) == 0);
static_assert(offsetof(GlobalFrameUbo, proj) == 64);
static_assert(offsetof(GlobalFrameUbo, inverseViewProjection) == 128);
static_assert(offsetof(GlobalFrameUbo, cameraPosWS) == 192);
static_assert(offsetof(GlobalFrameUbo, ambientColorIntensity) == 208);
static_assert(offsetof(GlobalFrameUbo, lightCounts) == 224);
static_assert(offsetof(GlobalFrameUbo, directionalLights) == 240);
static_assert(offsetof(GlobalFrameUbo, punctualLights) == 304);
static_assert(offsetof(GlobalFrameUbo, directionalShadowViewProj) == 816);
static_assert(offsetof(GlobalFrameUbo, shadowParams) == 880);
static_assert(offsetof(GlobalFrameUbo, environmentParams) == 896);
static_assert(sizeof(ToneMapPushConstants) == 16);

} // namespace vkr
