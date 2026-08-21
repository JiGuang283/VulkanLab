#pragma once

#include "render/graph/RenderResourcePool.h"

#include <array>

namespace vkr {

struct GBufferResources {
    RenderImageHandle depth{};
    RenderImageHandle baseColorMetallic{};
    RenderImageHandle normalRoughnessOcclusion{};
    RenderImageHandle emissiveSurfaceFlags{};
    RenderImageHandle motion{};

    bool valid() const {
        return depth.valid() && baseColorMetallic.valid() &&
               normalRoughnessOcclusion.valid() &&
               emissiveSurfaceFlags.valid() && motion.valid();
    }

    std::array<RenderImageHandle, 4> colors() const {
        return {baseColorMetallic, normalRoughnessOcclusion,
                emissiveSurfaceFlags, motion};
    }
};

inline GBufferResources
gBufferResources(const RendererResourceHandles &handles) {
    return {handles.gBufferDepth, handles.gBufferBaseColorMetallic,
            handles.gBufferNormalRoughnessOcclusion,
            handles.gBufferEmissiveSurfaceFlags, handles.gBufferMotion};
}

} // namespace vkr
