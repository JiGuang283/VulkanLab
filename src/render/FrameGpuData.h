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
    glm::vec4 cameraPosWS;
    glm::vec4 ambientColorIntensity;
    glm::vec4 lightCounts;
    GpuLight directionalLights[kMaxDirectionalLights];
    GpuLight punctualLights[kMaxPunctualLights];
    glm::mat4 directionalShadowViewProj{1.0f};
    glm::vec4 shadowParams{0.0f};
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
static_assert(sizeof(GlobalFrameUbo) == 832);
static_assert(offsetof(GlobalFrameUbo, view) == 0);
static_assert(offsetof(GlobalFrameUbo, proj) == 64);
static_assert(offsetof(GlobalFrameUbo, cameraPosWS) == 128);
static_assert(offsetof(GlobalFrameUbo, ambientColorIntensity) == 144);
static_assert(offsetof(GlobalFrameUbo, lightCounts) == 160);
static_assert(offsetof(GlobalFrameUbo, directionalLights) == 176);
static_assert(offsetof(GlobalFrameUbo, punctualLights) == 240);
static_assert(offsetof(GlobalFrameUbo, directionalShadowViewProj) == 752);
static_assert(offsetof(GlobalFrameUbo, shadowParams) == 816);
static_assert(sizeof(ToneMapPushConstants) == 16);

} // namespace vkr
