#pragma once

#include "render/graph/RenderResourcePool.h"

namespace vkr {

struct DeferredLightingResources {
    RenderImageHandle hdrColor{};
    RenderImageHandle baselineDiffuse{};
    RenderImageHandle baselineSpecular{};

    bool valid() const {
        return hdrColor.valid() && baselineDiffuse.valid() &&
               baselineSpecular.valid();
    }
};

inline DeferredLightingResources
deferredLightingResources(const RendererResourceHandles &handles) {
    return {handles.deferredHdrColor, handles.deferredBaselineDiffuse,
            handles.deferredBaselineSpecular};
}

} // namespace vkr
