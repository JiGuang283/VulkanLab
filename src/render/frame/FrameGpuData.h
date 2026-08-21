#pragma once

#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "render/features/shadows_visibility/DirectionalShadow.h"
#include "render/features/shadows_visibility/PunctualShadow.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <array>
#include <type_traits>

namespace vkr {

inline constexpr uint32_t kMaxSceneLights = 256;
inline constexpr uint32_t kMaxReflectionProbes = 8;
inline constexpr uint32_t kMaxDdgiProbes = 2048;

struct alignas(16) GpuRayTracingInstance {
    glm::uvec4 addresses{};
    glm::vec4 baseColorFactor{1.0f};
    glm::vec4 emissiveMetallic{};
    glm::vec4 materialParams{};
};

struct alignas(16) DdgiGpuParams {
    glm::mat4 localToWorld{1.0f};
    glm::mat4 worldToLocal{1.0f};
    glm::uvec4 probeCounts{};
    glm::vec4 probeSpacingMaxDistance{};
    glm::vec4 updateParameters{};
    glm::uvec4 runtimeParameters{};
    glm::uvec4 updateWindow{};
    glm::vec4 traceParameters{};
};

struct alignas(16) GpuDdgiProbeState {
    glm::vec4 offsetClassification{};
    glm::vec4 statistics{};
};

struct alignas(16) GpuDdgiRayResult {
    glm::vec4 radianceDistance{};
    glm::vec4 directionHit{};
};

struct alignas(16) GpuLight {
    glm::vec4 positionRange;
    glm::vec4 directionInnerCos;
    glm::vec4 colorIntensity;
    glm::vec4 params;
};

struct alignas(16) GpuReflectionProbe {
    glm::mat4 worldToLocal{1.0f};
    glm::vec4 capturePositionRadius{0.0f};
    glm::vec4 boxExtentsBlend{0.0f};
    glm::vec4 params{0.0f};
};

struct alignas(16) GpuReflectionProbeBuffer {
    glm::uvec4 counts{0u};
    std::array<GpuReflectionProbe, kMaxReflectionProbes> probes{};
};

struct alignas(16) GlobalFrameUbo {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 inverseViewProjection;
    glm::vec4 cameraPosWS;
    glm::vec4 ambientColorIntensity;
    glm::uvec4 lightCounts;
    glm::mat4 cascadeViewProj[kCsmCascadeCount];
    glm::vec4 cascadeSplits{0.0f};
    glm::vec4 cascadeBlendStarts{0.0f};
    glm::vec4 shadowParams{0.0f};
    glm::ivec4 punctualShadowCounts{0};
    glm::vec4 punctualShadowParams{0.0f};
    glm::mat4 spotShadowViewProj[kMaxSpotShadowLights];
    glm::vec4 environmentParams{0.0f};
    glm::uvec4 clusterGrid{0u};
    glm::uvec4 clusterViewport{0u};
    glm::vec4 clusterDepthParams{0.0f};
};

// Shared by atmosphere compute, sky, and PBR fragment programs (std140).
struct alignas(16) AtmosphereGpuParams {
    glm::vec4 planetCenterBottomRadius;
    glm::vec4 topRadiusDensityHeights;
    glm::vec4 rayleighScatteringOzoneHalfWidth;
    glm::vec4 mieScatteringExtinction;
    glm::vec4 ozoneAbsorptionAerialStart;
    glm::vec4 groundAlbedoDistanceScale;
    glm::vec4 sunDirectionAngularRadius;
    glm::vec4 sunColorIntensity;
    glm::vec4 cameraDistanceParams;
    glm::vec4 viewportParams;
    glm::uvec4 runtimeParams;
    glm::vec4 reserved;
};

struct alignas(16) SurfaceFrameUbo {
    glm::mat4 previousViewProjection{1.0f};
    glm::vec4 viewportSizeInvSize{0.0f};
    glm::uvec4 params{0u};
};

struct alignas(16) ScreenSpaceLightingUbo {
    glm::vec4 viewportSizeInvSize{0.0f};
    glm::uvec4 modes{0u};
};

struct alignas(16) TaaFrameUbo {
    glm::mat4 currentInverseViewProjection{1.0f};
    glm::mat4 previousViewProjection{1.0f};
    glm::mat4 previousInverseViewProjection{1.0f};
    glm::vec4 viewportSizeInvSize{0.0f};
    glm::vec4 jitterCurrentPreviousPixels{0.0f};
    glm::vec4 parameters{0.0f};
    glm::uvec4 flags{0u};
};

struct alignas(16) GpuRenderItemHistory {
    glm::mat4 previousWorld{1.0f};
    glm::uvec4 params{0u};
};

struct alignas(16) ToneMapPushConstants {
    float exposureEv = 0.0f;
    float bloomIntensity = 0.0f;
    uint32_t toneMapper = 0;
    uint32_t encodeGamma = 0;
    uint32_t applyExposure = 0;
    uint32_t applyBloom = 0;
    uint32_t surfaceDebugMode = 0;
    float motionDebugScale = 1.0f;
    uint32_t screenDebugMode = 0;
    uint32_t screenDebugMip = 0;
    float cameraNear = 0.1f;
    float cameraFar = 1000.0f;
    uint32_t gBufferDebugMode = 0;
    uint32_t deferredLightingDebugMode = 0;
    uint32_t reserved1 = 0;
    uint32_t reserved2 = 0;
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
static_assert(sizeof(GpuReflectionProbe) == 112);
static_assert(sizeof(GpuRayTracingInstance) == 64);
static_assert(sizeof(DdgiGpuParams) == 224);
static_assert(sizeof(GpuDdgiProbeState) == 32);
static_assert(sizeof(GpuDdgiRayResult) == 32);
static_assert(sizeof(GpuReflectionProbeBuffer) == 912);
static_assert(offsetof(GpuLight, positionRange) == 0);
static_assert(offsetof(GpuLight, directionInnerCos) == 16);
static_assert(offsetof(GpuLight, colorIntensity) == 32);
static_assert(offsetof(GpuLight, params) == 48);
static_assert(sizeof(GlobalFrameUbo) == 896);
static_assert(sizeof(AtmosphereGpuParams) == 192);
static_assert(sizeof(SurfaceFrameUbo) == 96);
static_assert(sizeof(ScreenSpaceLightingUbo) == 32);
static_assert(sizeof(TaaFrameUbo) == 256);
static_assert(sizeof(GpuRenderItemHistory) == 80);
static_assert(offsetof(AtmosphereGpuParams, planetCenterBottomRadius) == 0);
static_assert(offsetof(AtmosphereGpuParams, topRadiusDensityHeights) == 16);
static_assert(offsetof(AtmosphereGpuParams,
                       rayleighScatteringOzoneHalfWidth) == 32);
static_assert(offsetof(AtmosphereGpuParams, mieScatteringExtinction) == 48);
static_assert(offsetof(AtmosphereGpuParams, ozoneAbsorptionAerialStart) == 64);
static_assert(offsetof(AtmosphereGpuParams, groundAlbedoDistanceScale) == 80);
static_assert(offsetof(AtmosphereGpuParams, sunDirectionAngularRadius) == 96);
static_assert(offsetof(AtmosphereGpuParams, sunColorIntensity) == 112);
static_assert(offsetof(AtmosphereGpuParams, cameraDistanceParams) == 128);
static_assert(offsetof(AtmosphereGpuParams, viewportParams) == 144);
static_assert(offsetof(AtmosphereGpuParams, runtimeParams) == 160);
static_assert(offsetof(AtmosphereGpuParams, reserved) == 176);
static_assert(offsetof(GlobalFrameUbo, view) == 0);
static_assert(offsetof(GlobalFrameUbo, proj) == 64);
static_assert(offsetof(GlobalFrameUbo, inverseViewProjection) == 128);
static_assert(offsetof(GlobalFrameUbo, cameraPosWS) == 192);
static_assert(offsetof(GlobalFrameUbo, ambientColorIntensity) == 208);
static_assert(offsetof(GlobalFrameUbo, lightCounts) == 224);
static_assert(offsetof(GlobalFrameUbo, cascadeViewProj) == 240);
static_assert(offsetof(GlobalFrameUbo, cascadeSplits) == 496);
static_assert(offsetof(GlobalFrameUbo, cascadeBlendStarts) == 512);
static_assert(offsetof(GlobalFrameUbo, shadowParams) == 528);
static_assert(offsetof(GlobalFrameUbo, punctualShadowCounts) == 544);
static_assert(offsetof(GlobalFrameUbo, punctualShadowParams) == 560);
static_assert(offsetof(GlobalFrameUbo, environmentParams) == 832);
static_assert(offsetof(GlobalFrameUbo, clusterGrid) == 848);
static_assert(offsetof(GlobalFrameUbo, clusterViewport) == 864);
static_assert(offsetof(GlobalFrameUbo, clusterDepthParams) == 880);
static_assert(sizeof(ToneMapPushConstants) == 64);
static_assert(offsetof(ToneMapPushConstants, exposureEv) == 0);
static_assert(offsetof(ToneMapPushConstants, bloomIntensity) == 4);
static_assert(offsetof(ToneMapPushConstants, toneMapper) == 8);
static_assert(offsetof(ToneMapPushConstants, encodeGamma) == 12);
static_assert(offsetof(ToneMapPushConstants, applyExposure) == 16);
static_assert(offsetof(ToneMapPushConstants, applyBloom) == 20);
static_assert(offsetof(ToneMapPushConstants, surfaceDebugMode) == 24);
static_assert(offsetof(ToneMapPushConstants, motionDebugScale) == 28);
static_assert(offsetof(ToneMapPushConstants, screenDebugMode) == 32);
static_assert(offsetof(ToneMapPushConstants, screenDebugMip) == 36);
static_assert(offsetof(ToneMapPushConstants, cameraNear) == 40);
static_assert(offsetof(ToneMapPushConstants, cameraFar) == 44);
static_assert(offsetof(ToneMapPushConstants, gBufferDebugMode) == 48);
static_assert(offsetof(ToneMapPushConstants, deferredLightingDebugMode) == 52);
static_assert(sizeof(BloomPushConstants) == 16);

} // namespace vkr
