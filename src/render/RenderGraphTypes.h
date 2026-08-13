#pragma once

#include <cstdint>
#include <limits>
#include <vulkan/vulkan.h>

namespace vkr {

enum class RgPassType { Graphics, Compute, Transfer, External };
enum class RgQueueClass { Graphics, Compute, Transfer };
enum class RgPassCondition {
    Always,
    Atmosphere,
    DirectionalShadow,
    PointShadow,
    SpotShadow,
    SurfaceData,
    HiZ,
    Occlusion,
    ScreenDepthPyramid,
    SceneColorPyramid,
    Ssao,
    Cacao,
    Gtao,
    Ddgi,
    Ssr,
    Ssgi,
    Taa,
    Bloom,
    Capture,
};
enum class RgResourceLifetime {
    Transient,
    PerFrame,
    History,
    Persistent,
    Imported,
};

inline constexpr uint32_t kInvalidRenderGraphResource =
    std::numeric_limits<uint32_t>::max();

struct RgImageHandle {
    uint32_t resource = kInvalidRenderGraphResource;
    uint32_t version = 0;

    bool valid() const { return resource != kInvalidRenderGraphResource; }
    bool operator==(const RgImageHandle &rhs) const {
        return resource == rhs.resource && version == rhs.version;
    }
};

struct RgBufferHandle {
    uint32_t resource = kInvalidRenderGraphResource;
    uint32_t version = 0;

    bool valid() const { return resource != kInvalidRenderGraphResource; }
    bool operator==(const RgBufferHandle &rhs) const {
        return resource == rhs.resource && version == rhs.version;
    }
};

using RenderGraphPassId = uint64_t;

struct RgImageSubresource {
    VkImageAspectFlags aspectMask = 0;
    uint32_t baseMipLevel = 0;
    uint32_t levelCount = VK_REMAINING_MIP_LEVELS;
    uint32_t baseArrayLayer = 0;
    uint32_t layerCount = VK_REMAINING_ARRAY_LAYERS;
};

enum class RgBufferAccess {
    UniformRead,
    StorageRead,
    StorageWrite,
    StorageReadWrite,
    VertexRead,
    IndexRead,
    IndirectRead,
    TransferRead,
    TransferWrite,
    AccelerationStructureBuildRead,
    AccelerationStructureBuildWrite,
    AccelerationStructureRead,
};

} // namespace vkr
