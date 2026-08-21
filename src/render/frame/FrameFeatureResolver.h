#pragma once

#include "render/frame/RenderFeatureState.h"
#include "render/frame/RenderFrame.h"

#include <cstdint>
#include <optional>

namespace vkr {

struct RenderSettings;
struct ViewMode;

struct FrameFeatureResolveInput {
    const RenderSettings *settings = nullptr;
    const ViewMode *viewMode = nullptr;
    const RenderFeatureSupport *support = nullptr;
    const RendererResourceHandles *resources = nullptr;

    bool atmosphereActive = false;
    bool transparentVisible = false;
    bool directionalShadowActive = false;
    uint32_t directionalShadowCascadeCount = 0;
    uint32_t pointShadowLightCount = 0;
    uint32_t spotShadowLightCount = 0;
    uint32_t punctualLightCount = 0;
    uint32_t occlusionCandidates = 0;
    bool ddgiSceneActive = false;
    bool captureRequested = false;
    std::optional<FrameCaptureSource> captureSource;
};

struct FrameFeatureResolution {
    FrameRenderFeatures features{};
    RenderFeatureRuntimeState runtime{};
};

FrameFeatureResolution
resolveFrameFeatures(const FrameFeatureResolveInput &input);

} // namespace vkr
