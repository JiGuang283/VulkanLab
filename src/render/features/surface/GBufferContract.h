#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace vkr {

enum class GBufferAttachment : uint32_t {
    BaseColorMetallic = 0,
    NormalRoughnessOcclusion = 1,
    EmissiveSurfaceFlags = 2,
    Count = 3,
};

enum class GBufferShadingModel : uint32_t {
    DefaultLit = 0,
    Unlit = 1,
};

inline constexpr uint32_t kGBufferHistoryValidBit = 1u << 0u;
inline constexpr uint32_t kGBufferShadingModelShift = 1u;
inline constexpr uint32_t kGBufferShadingModelMask = 0x7u << 1u;
inline constexpr uint32_t kGBufferReceivesScreenAoBit = 1u << 4u;
inline constexpr uint32_t kGBufferKnownSurfaceFlagsMask =
    kGBufferHistoryValidBit | kGBufferShadingModelMask |
    kGBufferReceivesScreenAoBit;

constexpr uint32_t packGBufferSurfaceFlags(GBufferShadingModel shadingModel,
                                           bool historyValid,
                                           bool receivesScreenAo) {
    return (historyValid ? kGBufferHistoryValidBit : 0u) |
           ((static_cast<uint32_t>(shadingModel) <<
             kGBufferShadingModelShift) &
            kGBufferShadingModelMask) |
           (receivesScreenAo ? kGBufferReceivesScreenAoBit : 0u);
}

constexpr GBufferShadingModel
unpackGBufferShadingModel(uint32_t surfaceFlags) {
    return static_cast<GBufferShadingModel>(
        (surfaceFlags & kGBufferShadingModelMask) >>
        kGBufferShadingModelShift);
}

struct alignas(16) GBufferDecodedSurfaceAbi {
    std::array<float, 4> baseColorMetallic{};
    std::array<float, 4> normalRoughnessOcclusion{};
    std::array<float, 4> emissiveSurfaceFlags{};
};

inline constexpr VkFormat kGBufferBaseColorMetallicFormat =
    VK_FORMAT_R8G8B8A8_UNORM;
inline constexpr VkFormat kGBufferNormalRoughnessOcclusionFormat =
    VK_FORMAT_R16G16B16A16_SFLOAT;
inline constexpr VkFormat kGBufferEmissiveSurfaceFlagsFormat =
    VK_FORMAT_R16G16B16A16_SFLOAT;
inline constexpr VkFormat kGBufferMotionFormat =
    VK_FORMAT_R16G16_SFLOAT;

inline constexpr uint32_t kGBufferAttachmentBytesPerPixel = 20;
inline constexpr uint32_t kGBufferMotionBytesPerPixel = 4;
inline constexpr uint32_t kGBufferNominalDepthBytesPerPixel = 4;
inline constexpr uint32_t kGBufferNominalBytesPerPixel =
    kGBufferAttachmentBytesPerPixel + kGBufferMotionBytesPerPixel +
    kGBufferNominalDepthBytesPerPixel;

struct GBufferContractStatus {
    bool defined = true;
    bool implemented = false;
    uint32_t attachmentCount =
        static_cast<uint32_t>(GBufferAttachment::Count);
    uint32_t nominalBytesPerPixel = kGBufferNominalBytesPerPixel;
    VkFormat baseColorMetallicFormat = kGBufferBaseColorMetallicFormat;
    VkFormat normalRoughnessOcclusionFormat =
        kGBufferNormalRoughnessOcclusionFormat;
    VkFormat emissiveSurfaceFlagsFormat =
        kGBufferEmissiveSurfaceFlagsFormat;
    VkFormat motionFormat = kGBufferMotionFormat;
};

constexpr GBufferContractStatus gBufferContractStatus() {
    return {};
}

static_assert(alignof(GBufferDecodedSurfaceAbi) == 16);
static_assert(sizeof(GBufferDecodedSurfaceAbi) == 48);
static_assert(offsetof(GBufferDecodedSurfaceAbi,
                       normalRoughnessOcclusion) == 16);
static_assert(offsetof(GBufferDecodedSurfaceAbi,
                       emissiveSurfaceFlags) == 32);
static_assert(static_cast<uint32_t>(GBufferAttachment::Count) == 3);
static_assert(packGBufferSurfaceFlags(GBufferShadingModel::Unlit, true,
                                     true) <= 2048u,
              "Surface flags must remain exactly representable in FP16");

} // namespace vkr
