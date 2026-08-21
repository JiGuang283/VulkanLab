#pragma once

#include "render/features/surface/GBufferContract.h"
#include "render/graph/RenderResourcePool.h"
#include "render/path/RenderPathMode.h"

#include <cstdint>
#include <string>

namespace vkr {

struct ScreenSpaceSurfaceProducts {
    RenderImageHandle depth{};
    RenderImageHandle normalRoughness{};
    RenderImageHandle motion{};
};

struct OpaqueRenderProducts {
    RenderImageHandle hdrColor{};
    RenderImageHandle baselineDiffuse{};
    RenderImageHandle baselineSpecular{};
    RenderImageHandle geometryDepth{};
    ScreenSpaceSurfaceProducts screenSpace{};
};

inline OpaqueRenderProducts opaqueRenderProducts(
    RenderPathMode mode, const RendererResourceHandles &resources,
    bool surfaceDataRequired) {
    OpaqueRenderProducts products{};
    products.screenSpace = {resources.surfaceDepth,
                            resources.surfaceNormalRoughness,
                            resources.surfaceMotion};
    if (mode == RenderPathMode::Deferred) {
        products.hdrColor = resources.deferredHdrColor;
        products.baselineDiffuse = resources.deferredBaselineDiffuse;
        products.baselineSpecular = resources.deferredBaselineSpecular;
        products.geometryDepth = resources.gBufferDepth;
    } else {
        products.hdrColor = resources.hdrColor;
        products.baselineDiffuse = resources.baselineDiffuse;
        products.baselineSpecular = resources.baselineSpecular;
        products.geometryDepth =
            surfaceDataRequired ? resources.surfaceDepth
                                : resources.mainDepth;
    }
    return products;
}

inline RenderImageHandle postLightingHdr(
    const OpaqueRenderProducts &products,
    const RendererResourceHandles &resources,
    bool lightingCompositeRequired) {
    return lightingCompositeRequired ? resources.compositedHdrColor
                                     : products.hdrColor;
}

struct OpaqueRenderProductStatus {
    bool hdrColor = false;
    bool depth = false;
    bool sampledDepth = false;
    bool normalRoughness = false;
    bool motion = false;
    bool baselineDiffuse = false;
    bool baselineSpecular = false;
    bool multisampled = false;
    uint32_t colorAttachmentCount = 0;
};

struct RenderPathStatus {
    RenderPathRequest requested = RenderPathRequest::Auto;
    RenderPathMode active = RenderPathMode::Forward;
    RenderPathCapabilities capabilities{};
    OpaqueRenderProductStatus products{};
    GBufferContractStatus gBuffer{};
    std::string viewMode;
    std::string unavailableReason;
};

} // namespace vkr
