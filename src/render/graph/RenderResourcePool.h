#pragma once

#include "core/FrameSync.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkr {

class Device;
class Image;

inline constexpr uint32_t kInvalidRenderResource =
    std::numeric_limits<uint32_t>::max();

struct RenderImageHandle {
    uint32_t index = kInvalidRenderResource;

    bool valid() const { return index != kInvalidRenderResource; }
    bool operator==(const RenderImageHandle &rhs) const {
        return index == rhs.index;
    }
};

struct RenderSamplerHandle {
    uint32_t index = kInvalidRenderResource;

    bool valid() const { return index != kInvalidRenderResource; }
};

enum class RenderExtentPolicy {
    Viewport,
    Fixed,
};

enum class RenderResourceMultiplicity {
    Single,
    PerFrame,
};

enum class RenderMipPolicy {
    Fixed,
    FullChain,
};

struct RenderImageDesc {
    std::string name;
    RenderExtentPolicy extentPolicy = RenderExtentPolicy::Viewport;
    VkExtent2D fixedExtent{};
    RenderResourceMultiplicity multiplicity =
        RenderResourceMultiplicity::PerFrame;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    VkImageUsageFlags usage = 0;
    VkMemoryPropertyFlags memoryProperties =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t mipLevels = 1;
    bool externallyInitialized = false;
    VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t extentDivisor = 1;
    uint32_t arrayLayers = 1;
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
    RenderMipPolicy mipPolicy = RenderMipPolicy::Fixed;
    bool historyCapable = false;
    VkImageCreateFlags createFlags = 0;
    // Resources initialized or consumed outside the frame graph remain
    // resident until their external ownership is migrated into the graph.
    bool persistentResidency = false;
};

enum class RenderResourceResidency {
    Unallocated,
    Resident,
    Retiring,
};

struct RenderResourceResidencyUpdate {
    bool physicalResourcesChanged = false;
    uint32_t created = 0;
    uint32_t retired = 0;
    uint32_t destroyed = 0;
    std::vector<RenderImageHandle> createdImages;
    std::vector<RenderImageHandle> destroyedImages;
};

struct RenderSamplerDesc {
    std::string name;
    VkFilter magFilter = VK_FILTER_NEAREST;
    VkFilter minFilter = VK_FILTER_NEAREST;
    VkSamplerMipmapMode mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSamplerAddressMode addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSamplerAddressMode addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    float mipLodBias = 0.0f;
    bool anisotropyEnable = false;
    float maxAnisotropy = 1.0f;
    bool compareEnable = false;
    VkCompareOp compareOp = VK_COMPARE_OP_ALWAYS;
    float minLod = 0.0f;
    float maxLod = 0.0f;
    VkBorderColor borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
};

enum class RenderImageAccess {
    ColorAttachmentWrite,
    ColorAttachmentReadWrite,
    DepthAttachmentWrite,
    DepthAttachmentRead,
    SampledRead,
    StorageWrite,
    StorageReadWrite,
    TransferRead,
    TransferWrite,
};

enum class RenderImageFrame {
    Current,
    Previous,
};

struct RenderImageUsage {
    RenderImageHandle image{};
    RenderImageAccess access = RenderImageAccess::SampledRead;
    VkImageLayout requiredLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    RenderImageFrame frame = RenderImageFrame::Current;
};

struct RenderPassResourceUsage {
    std::string passName;
    std::vector<RenderImageUsage> images;
};

struct RendererResourceHandles {
    static constexpr uint32_t kBloomPyramidLevelCount = 6;

    RenderImageHandle hdrColor{};
    RenderImageHandle hdrMsaaColor{};
    RenderImageHandle mainDepth{};
    RenderImageHandle viewportColor{};
    RenderImageHandle directionalShadowDepth{};
    std::array<RenderImageHandle, 4> pointShadowDepthByCapacity{};
    std::array<RenderImageHandle, 4> spotShadowDepthByCapacity{};
    RenderImageHandle directionalShadowFallback{};
    RenderImageHandle pointShadowFallback{};
    RenderImageHandle spotShadowFallback{};
    RenderImageHandle surfaceDepth{};
    RenderImageHandle surfaceNormalRoughness{};
    RenderImageHandle surfaceMotion{};
    RenderImageHandle surfaceAlbedoMetallic{};
    RenderImageHandle gBufferDepth{};
    RenderImageHandle gBufferBaseColorMetallic{};
    RenderImageHandle gBufferNormalRoughnessOcclusion{};
    RenderImageHandle gBufferEmissiveSurfaceFlags{};
    RenderImageHandle gBufferMotion{};
    RenderImageHandle deferredHdrColor{};
    RenderImageHandle deferredBaselineDiffuse{};
    RenderImageHandle deferredBaselineSpecular{};
    RenderImageHandle depthHierarchyMinMax{};
    RenderImageHandle visibilityHiZ{};
    RenderImageHandle screenDepthPyramid{};
    RenderImageHandle sceneColorPyramid{};
    RenderImageHandle ssaoRaw{};
    RenderImageHandle ssaoTemp{};
    RenderImageHandle ssaoFiltered{};
    RenderImageHandle cacaoDepth{};
    RenderImageHandle cacaoViewNormals{};
    RenderImageHandle cacaoOutput{};
    RenderImageHandle gtaoRaw{};
    RenderImageHandle gtaoHistory{};
    RenderImageHandle gtaoTemp{};
    RenderImageHandle gtaoFiltered{};
    RenderImageHandle gtaoDebug{};
    RenderImageHandle taaHistory{};
    RenderImageHandle taaDebug{};
    RenderImageHandle baselineSpecular{};
    RenderImageHandle baselineSpecularMsaa{};
    RenderImageHandle baselineDiffuse{};
    RenderImageHandle baselineDiffuseMsaa{};
    RenderImageHandle compositedHdrColor{};
    RenderImageHandle ssrRaw{};
    RenderImageHandle ssrHistory{};
    RenderImageHandle ssrTemp{};
    RenderImageHandle ssrFiltered{};
    RenderImageHandle ssrDebug{};
    RenderImageHandle ssgiRaw{};
    RenderImageHandle ssgiHistory{};
    RenderImageHandle ssgiMoments{};
    RenderImageHandle ssgiTemp{};
    RenderImageHandle ssgiFiltered{};
    RenderImageHandle ssgiDebug{};
    std::array<RenderImageHandle, kBloomPyramidLevelCount> bloomLevels{};
    RenderSamplerHandle hdrSampler{};
    RenderSamplerHandle viewportSampler{};
    RenderSamplerHandle shadowSampler{};
    RenderSamplerHandle pointShadowSampler{};
    RenderSamplerHandle spotShadowSampler{};
    RenderSamplerHandle surfaceDepthSampler{};
    RenderSamplerHandle surfaceDataSampler{};
    RenderSamplerHandle visibilityHiZSampler{};
    RenderSamplerHandle screenPyramidSampler{};
    RenderSamplerHandle ssaoSampler{};
    RenderSamplerHandle taaSampler{};
    RenderSamplerHandle ssrSampler{};
    RenderSamplerHandle ssgiSampler{};
    RenderSamplerHandle bloomSampler{};
    RenderImageHandle atmosphereTransmittance{};
    RenderImageHandle atmosphereMultipleScattering{};
    RenderImageHandle atmosphereSkyView{};
    RenderImageHandle atmosphereAerialPerspective{};
    RenderImageHandle atmosphereTransmittanceFallback{};
    RenderImageHandle atmosphereScatteringFallback{};
    RenderImageHandle atmosphereAerialFallback{};
    RenderSamplerHandle atmosphereSampler{};
    RenderImageHandle ddgiIrradiance{};
    RenderImageHandle ddgiDistance{};
    RenderImageHandle ddgiIrradianceFallback{};
    RenderImageHandle ddgiDistanceFallback{};
    RenderSamplerHandle ddgiSampler{};
};

class RenderResourcePool {
  public:
    explicit RenderResourcePool(Device &device,
                                    uint32_t frameCount =
                                        MAX_FRAMES_IN_FLIGHT);
    ~RenderResourcePool();

    RenderResourcePool(const RenderResourcePool &) = delete;
    RenderResourcePool &operator=(const RenderResourcePool &) = delete;

    RenderImageHandle registerImage(RenderImageDesc desc);
    RenderSamplerHandle registerSampler(RenderSamplerDesc desc);
    void realize(VkExtent2D viewportExtent);
    RenderResourceResidencyUpdate synchronizeResidency(
        const std::vector<RenderImageHandle> &activeImages,
        uint64_t lastSubmittedSerial, uint64_t completedSerial);
    void releaseViewportDependent();
    void recreateViewportDependent(VkExtent2D viewportExtent);

    bool valid(RenderImageHandle handle) const;
    bool valid(RenderSamplerHandle handle) const;
    bool resident(RenderImageHandle handle) const;
    RenderResourceResidency residency(RenderImageHandle handle) const;
    const RenderImageDesc &description(RenderImageHandle handle) const;
    const std::vector<RenderImageDesc> &imageDescriptions() const {
        return imageDescriptions_;
    }
    const Image &image(RenderImageHandle handle, uint32_t frameIndex) const;
    const Image &previousImage(RenderImageHandle handle,
                               uint32_t frameIndex) const;
    VkImageView mipView(RenderImageHandle handle, uint32_t frameIndex,
                        uint32_t mipLevel) const;
    VkImageView attachmentView(RenderImageHandle handle,
                               uint32_t frameIndex,
                               uint32_t mipLevel,
                               uint32_t baseArrayLayer,
                               uint32_t layerCount,
                               VkImageAspectFlags aspect) const;
    uint32_t mipLevelCount(RenderImageHandle handle) const;
    VkExtent2D mipExtent(RenderImageHandle handle,
                         uint32_t mipLevel) const;
    VkSampler sampler(RenderSamplerHandle handle) const;
    VkExtent2D extent(RenderImageHandle handle) const;
    VkExtent2D viewportExtent() const { return viewportExtent_; }
    uint32_t frameCount() const { return frameCount_; }
    uint64_t estimatedResidentBytes() const;
    uint64_t estimatedRetiringBytes() const;
    uint64_t estimatedLogicalBytes() const;
    uint64_t estimatedBytes(RenderImageHandle handle) const;
    uint64_t residencyGeneration() const { return residencyGeneration_; }

  private:
    struct AttachmentView {
        uint32_t imageIndex = 0;
        VkImage image = VK_NULL_HANDLE;
        uint32_t frameIndex = 0;
        uint32_t mipLevel = 0;
        uint32_t baseArrayLayer = 0;
        uint32_t layerCount = 0;
        VkImageAspectFlags aspect = 0;
        VkImageView view = VK_NULL_HANDLE;
    };

    void destroyAttachmentViews(
        std::optional<uint32_t> imageIndex = std::nullopt);
    void updateMipLevelCount(uint32_t index);
    void destroyImageEntry(uint32_t index);
    void createImageEntry(uint32_t index);
    void createSamplerEntry(uint32_t index);

    Device *device_ = nullptr;
    uint32_t frameCount_ = 0;
    VkExtent2D viewportExtent_{};
    bool realized_ = false;
    std::vector<RenderImageDesc> imageDescriptions_;
    std::vector<std::vector<std::unique_ptr<Image>>> images_;
    std::vector<RenderResourceResidency> residency_;
    std::vector<uint64_t> retireAfterSerial_;
    mutable std::vector<AttachmentView> attachmentViews_;
    std::vector<uint32_t> realizedMipLevels_;
    std::vector<RenderSamplerDesc> samplerDescriptions_;
    std::vector<VkSampler> samplers_;
    uint64_t residencyGeneration_ = 0;
};

RendererResourceHandles
registerDefaultRendererResources(RenderResourcePool &registry,
                                 Device &device,
                                 VkFormat viewportColorFormat);

void validateRenderResourceContracts(
    const std::vector<RenderImageDesc> &descriptions,
    const std::vector<RenderPassResourceUsage> &passes,
    bool requireCompatibleLayouts = true);

} // namespace vkr
