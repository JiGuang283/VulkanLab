#pragma once

#include "render/graph/RenderResourcePool.h"

namespace vkr {

enum class DepthHierarchySemantic {
    Nearest,
    Farthest,
};

struct DepthHierarchyResources {
    RenderImageHandle combinedMinMax{};
    RenderImageHandle nearestFallback{};
    RenderImageHandle farthestFallback{};
    RenderSamplerHandle combinedOrNearestSampler{};
    RenderSamplerHandle farthestFallbackSampler{};

    bool combined() const { return combinedMinMax.valid(); }
    bool available() const {
        return combined() ||
               (nearestFallback.valid() && farthestFallback.valid());
    }
    RenderImageHandle image(DepthHierarchySemantic semantic) const {
        if (combined())
            return combinedMinMax;
        return semantic == DepthHierarchySemantic::Nearest
                   ? nearestFallback
                   : farthestFallback;
    }
    RenderSamplerHandle sampler(DepthHierarchySemantic semantic) const {
        if (combined() || semantic == DepthHierarchySemantic::Nearest)
            return combinedOrNearestSampler;
        return farthestFallbackSampler;
    }
};

inline DepthHierarchyResources depthHierarchyResources(
    const RendererResourceHandles &handles) {
    return {handles.depthHierarchyMinMax,
            handles.screenDepthPyramid,
            handles.visibilityHiZ,
            handles.screenPyramidSampler,
            handles.visibilityHiZSampler};
}

} // namespace vkr
