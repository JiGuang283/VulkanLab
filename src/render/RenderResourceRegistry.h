#pragma once

#include "core/FrameSync.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
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
    SampledRead,
    StorageWrite,
    StorageReadWrite,
};

struct RenderImageUsage {
    RenderImageHandle image{};
    RenderImageAccess access = RenderImageAccess::SampledRead;
    VkImageLayout requiredLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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
    std::array<RenderImageHandle, kBloomPyramidLevelCount> bloomLevels{};
    RenderSamplerHandle hdrSampler{};
    RenderSamplerHandle viewportSampler{};
    RenderSamplerHandle shadowSampler{};
    RenderSamplerHandle bloomSampler{};
};

class RenderResourceRegistry {
  public:
    explicit RenderResourceRegistry(Device &device,
                                    uint32_t frameCount =
                                        MAX_FRAMES_IN_FLIGHT);
    ~RenderResourceRegistry();

    RenderResourceRegistry(const RenderResourceRegistry &) = delete;
    RenderResourceRegistry &operator=(const RenderResourceRegistry &) = delete;

    RenderImageHandle registerImage(RenderImageDesc desc);
    RenderSamplerHandle registerSampler(RenderSamplerDesc desc);
    void realize(VkExtent2D viewportExtent);
    void releaseViewportDependent();
    void recreateViewportDependent(VkExtent2D viewportExtent);

    bool valid(RenderImageHandle handle) const;
    bool valid(RenderSamplerHandle handle) const;
    const RenderImageDesc &description(RenderImageHandle handle) const;
    const std::vector<RenderImageDesc> &imageDescriptions() const {
        return imageDescriptions_;
    }
    const Image &image(RenderImageHandle handle, uint32_t frameIndex) const;
    VkSampler sampler(RenderSamplerHandle handle) const;
    VkExtent2D extent(RenderImageHandle handle) const;
    VkExtent2D viewportExtent() const { return viewportExtent_; }
    uint32_t frameCount() const { return frameCount_; }

  private:
    void createImageEntry(uint32_t index);
    void createSamplerEntry(uint32_t index);

    Device *device_ = nullptr;
    uint32_t frameCount_ = 0;
    VkExtent2D viewportExtent_{};
    bool realized_ = false;
    std::vector<RenderImageDesc> imageDescriptions_;
    std::vector<std::vector<std::unique_ptr<Image>>> images_;
    std::vector<RenderSamplerDesc> samplerDescriptions_;
    std::vector<VkSampler> samplers_;
};

RendererResourceHandles
registerDefaultRendererResources(RenderResourceRegistry &registry,
                                 Device &device,
                                 VkFormat viewportColorFormat);

void validateRenderResourceContracts(
    const std::vector<RenderImageDesc> &descriptions,
    const std::vector<RenderPassResourceUsage> &passes);

} // namespace vkr
