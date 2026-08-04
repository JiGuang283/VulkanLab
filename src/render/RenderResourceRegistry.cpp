#include "render/RenderResourceRegistry.h"

#include "core/Device.h"
#include "core/GpuDebugUtils.h"
#include "core/Image.h"
#include "core/VulkanCheck.h"
#include "render/DirectionalShadow.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace vkr {

namespace {

VkSampleCountFlagBits highestSampleCount(VkSampleCountFlags counts) {
    constexpr std::array<VkSampleCountFlagBits, 7> candidates = {
        VK_SAMPLE_COUNT_64_BIT, VK_SAMPLE_COUNT_32_BIT,
        VK_SAMPLE_COUNT_16_BIT, VK_SAMPLE_COUNT_8_BIT,
        VK_SAMPLE_COUNT_4_BIT,  VK_SAMPLE_COUNT_2_BIT,
        VK_SAMPLE_COUNT_1_BIT};
    for (VkSampleCountFlagBits candidate : candidates) {
        if ((counts & candidate) != 0)
            return candidate;
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

VkFormat chooseHdrFormat(Device &device) {
    constexpr std::array<VkFormat, 2> candidates = {
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
    const VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    for (VkFormat format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(device.physicalDevice(), format,
                                            &properties);
        if ((properties.optimalTilingFeatures & required) == required)
            return format;
    }
    throw std::runtime_error(
        "No floating-point HDR color attachment format is supported");
}

VkFormat chooseDepthFormat(Device &device, bool sampled) {
    constexpr std::array<VkFormat, 3> candidates = {
        VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D16_UNORM};
    VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (sampled)
        required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    for (VkFormat format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(device.physicalDevice(), format,
                                            &properties);
        if ((properties.optimalTilingFeatures & required) == required)
            return format;
    }
    throw std::runtime_error(sampled
                                 ? "No sampled depth format is supported"
                                 : "No depth attachment format is supported");
}

VkSampleCountFlagBits chooseSamples(Device &device, VkFormat hdrFormat,
                                    VkFormat depthFormat) {
    VkImageFormatProperties hdrProperties{};
    VkImageFormatProperties depthProperties{};
    const VkResult hdrResult = vkGetPhysicalDeviceImageFormatProperties(
        device.physicalDevice(), hdrFormat, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        0, &hdrProperties);
    const VkResult depthResult = vkGetPhysicalDeviceImageFormatProperties(
        device.physicalDevice(), depthFormat, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &depthProperties);
    if (hdrResult != VK_SUCCESS || depthResult != VK_SUCCESS)
        return VK_SAMPLE_COUNT_1_BIT;
    const VkSampleCountFlags supported =
        hdrProperties.sampleCounts & depthProperties.sampleCounts &
        static_cast<VkSampleCountFlags>(device.msaaSamples() |
                                        (device.msaaSamples() - 1));
    return highestSampleCount(supported);
}

VkImageUsageFlags requiredUsage(RenderImageAccess access) {
    switch (access) {
    case RenderImageAccess::ColorAttachmentWrite:
    case RenderImageAccess::ColorAttachmentReadWrite:
        return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    case RenderImageAccess::DepthAttachmentWrite:
        return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    case RenderImageAccess::SampledRead:
        return VK_IMAGE_USAGE_SAMPLED_BIT;
    case RenderImageAccess::StorageWrite:
    case RenderImageAccess::StorageReadWrite:
        return VK_IMAGE_USAGE_STORAGE_BIT;
    }
    return 0;
}

} // namespace

RenderResourceRegistry::RenderResourceRegistry(Device &device,
                                               uint32_t frameCount)
    : device_(&device), frameCount_(frameCount) {
    if (frameCount_ == 0)
        throw std::invalid_argument("render resource frame count is zero");
}

RenderResourceRegistry::~RenderResourceRegistry() {
    images_.clear();
    for (VkSampler sampler : samplers_) {
        if (sampler != VK_NULL_HANDLE)
            vkDestroySampler(device_->logicalDevice(), sampler, nullptr);
    }
}

RenderImageHandle
RenderResourceRegistry::registerImage(RenderImageDesc desc) {
    if (realized_)
        throw std::logic_error("cannot register an image after realization");
    if (desc.name.empty() || desc.format == VK_FORMAT_UNDEFINED ||
        desc.usage == 0 || desc.aspect == 0) {
        throw std::invalid_argument("invalid render image description");
    }
    if (desc.extentPolicy == RenderExtentPolicy::Fixed &&
        (desc.fixedExtent.width == 0 || desc.fixedExtent.height == 0)) {
        throw std::invalid_argument("fixed render image extent is zero");
    }
    if (desc.extentDivisor == 0)
        throw std::invalid_argument("render image extent divisor is zero");
    if (desc.arrayLayers == 0)
        throw std::invalid_argument("render image array layer count is zero");
    const RenderImageHandle handle{
        static_cast<uint32_t>(imageDescriptions_.size())};
    imageDescriptions_.push_back(std::move(desc));
    images_.emplace_back();
    realizedMipLevels_.push_back(0);
    return handle;
}

RenderSamplerHandle
RenderResourceRegistry::registerSampler(RenderSamplerDesc desc) {
    if (realized_)
        throw std::logic_error("cannot register a sampler after realization");
    if (desc.name.empty())
        throw std::invalid_argument("render sampler name is empty");
    const RenderSamplerHandle handle{
        static_cast<uint32_t>(samplerDescriptions_.size())};
    samplerDescriptions_.push_back(std::move(desc));
    samplers_.push_back(VK_NULL_HANDLE);
    return handle;
}

void RenderResourceRegistry::realize(VkExtent2D viewportExtent) {
    if (realized_)
        throw std::logic_error("render resources were already realized");
    viewportExtent_ = viewportExtent;
    for (uint32_t i = 0; i < imageDescriptions_.size(); ++i)
        createImageEntry(i);
    for (uint32_t i = 0; i < samplerDescriptions_.size(); ++i)
        createSamplerEntry(i);
    realized_ = true;
}

void RenderResourceRegistry::releaseViewportDependent() {
    for (uint32_t i = 0; i < imageDescriptions_.size(); ++i) {
        if (imageDescriptions_[i].extentPolicy ==
            RenderExtentPolicy::Viewport) {
            images_[i].clear();
        }
    }
}

void RenderResourceRegistry::recreateViewportDependent(
    VkExtent2D viewportExtent) {
    releaseViewportDependent();
    viewportExtent_ = viewportExtent;
    for (uint32_t i = 0; i < imageDescriptions_.size(); ++i) {
        if (imageDescriptions_[i].extentPolicy ==
            RenderExtentPolicy::Viewport) {
            createImageEntry(i);
        }
    }
}

bool RenderResourceRegistry::valid(RenderImageHandle handle) const {
    return handle.valid() && handle.index < imageDescriptions_.size();
}

bool RenderResourceRegistry::valid(RenderSamplerHandle handle) const {
    return handle.valid() && handle.index < samplerDescriptions_.size();
}

const RenderImageDesc &
RenderResourceRegistry::description(RenderImageHandle handle) const {
    if (!valid(handle))
        throw std::out_of_range("invalid render image handle");
    return imageDescriptions_[handle.index];
}

const Image &RenderResourceRegistry::image(RenderImageHandle handle,
                                           uint32_t frameIndex) const {
    if (!valid(handle))
        throw std::out_of_range("invalid render image handle");
    const RenderImageDesc &desc = imageDescriptions_[handle.index];
    const uint32_t index =
        desc.multiplicity == RenderResourceMultiplicity::Single
            ? 0
            : frameIndex;
    if (index >= images_[handle.index].size() ||
        !images_[handle.index][index]) {
        throw std::out_of_range("render image is not realized");
    }
    return *images_[handle.index][index];
}

VkImageView RenderResourceRegistry::mipView(RenderImageHandle handle,
                                            uint32_t frameIndex,
                                            uint32_t mipLevel) const {
    return image(handle, frameIndex).mipView(mipLevel);
}

uint32_t
RenderResourceRegistry::mipLevelCount(RenderImageHandle handle) const {
    if (!valid(handle))
        throw std::out_of_range("invalid render image handle");
    return realizedMipLevels_.at(handle.index);
}

VkExtent2D RenderResourceRegistry::mipExtent(RenderImageHandle handle,
                                             uint32_t mipLevel) const {
    if (mipLevel >= mipLevelCount(handle))
        throw std::out_of_range("render image mip level out of range");
    VkExtent2D result = extent(handle);
    result.width = std::max(1u, result.width >> mipLevel);
    result.height = std::max(1u, result.height >> mipLevel);
    return result;
}

VkSampler RenderResourceRegistry::sampler(RenderSamplerHandle handle) const {
    if (!valid(handle))
        throw std::out_of_range("invalid render sampler handle");
    return samplers_[handle.index];
}

VkExtent2D RenderResourceRegistry::extent(RenderImageHandle handle) const {
    const RenderImageDesc &desc = description(handle);
    if (desc.extentPolicy == RenderExtentPolicy::Fixed)
        return desc.fixedExtent;
    const uint32_t divisor = desc.extentDivisor;
    return {
        std::max(1u, (viewportExtent_.width + divisor - 1u) / divisor),
        std::max(1u, (viewportExtent_.height + divisor - 1u) / divisor)};
}

void RenderResourceRegistry::createImageEntry(uint32_t index) {
    const RenderImageDesc &desc = imageDescriptions_.at(index);
    const VkExtent2D imageExtent =
        desc.extentPolicy == RenderExtentPolicy::Fixed
            ? desc.fixedExtent
            : VkExtent2D{
                  std::max(1u,
                           (viewportExtent_.width + desc.extentDivisor - 1u) /
                               desc.extentDivisor),
                  std::max(1u,
                           (viewportExtent_.height + desc.extentDivisor -
                            1u) /
                               desc.extentDivisor)};
    if (imageExtent.width == 0 || imageExtent.height == 0)
        return;
    uint32_t mipLevels = desc.mipLevels;
    if (desc.mipPolicy == RenderMipPolicy::FullChain) {
        uint32_t dimension = std::max(imageExtent.width, imageExtent.height);
        mipLevels = 1;
        while (dimension > 1u) {
            dimension >>= 1u;
            ++mipLevels;
        }
    }
    realizedMipLevels_.at(index) = mipLevels;
    const uint32_t count =
        desc.multiplicity == RenderResourceMultiplicity::PerFrame
            ? frameCount_
            : 1;
    auto &entry = images_.at(index);
    entry.resize(count);
    for (uint32_t frameIndex = 0; frameIndex < entry.size(); ++frameIndex) {
        auto &image = entry[frameIndex];
        const std::string debugName =
            "RenderTarget/" + desc.name +
            (desc.multiplicity == RenderResourceMultiplicity::PerFrame
                 ? "/Frame" + std::to_string(frameIndex)
                 : std::string{});
        ImageCreateInfo imageInfo{};
        imageInfo.width = imageExtent.width;
        imageInfo.height = imageExtent.height;
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = desc.arrayLayers;
        imageInfo.samples = desc.samples;
        imageInfo.format = desc.format;
        imageInfo.tiling = desc.tiling;
        imageInfo.usage = desc.usage;
        imageInfo.memoryProperties = desc.memoryProperties;
        imageInfo.debugName = debugName;
        image = std::make_unique<Image>(*device_, imageInfo);
        image->createView(desc.format, desc.aspect, mipLevels,
                          desc.viewType, desc.arrayLayers);
        if (mipLevels > 1) {
            image->createMipViews(desc.format, desc.aspect, mipLevels,
                                  desc.viewType, desc.arrayLayers);
        }
    }
}

void RenderResourceRegistry::createSamplerEntry(uint32_t index) {
    const RenderSamplerDesc &desc = samplerDescriptions_.at(index);
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = desc.magFilter;
    info.minFilter = desc.minFilter;
    info.mipmapMode = desc.mipmapMode;
    info.addressModeU = desc.addressModeU;
    info.addressModeV = desc.addressModeV;
    info.addressModeW = desc.addressModeW;
    info.mipLodBias = desc.mipLodBias;
    info.anisotropyEnable = desc.anisotropyEnable ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = desc.maxAnisotropy;
    info.compareEnable = desc.compareEnable ? VK_TRUE : VK_FALSE;
    info.compareOp = desc.compareOp;
    info.minLod = desc.minLod;
    info.maxLod = desc.maxLod;
    info.borderColor = desc.borderColor;
    VK_CHECK(vkCreateSampler(device_->logicalDevice(), &info, nullptr,
                             &samplers_.at(index)));
    device_->debugUtils().setObjectName(
        VK_OBJECT_TYPE_SAMPLER, samplers_.at(index),
        "RenderTarget/" + desc.name);
}

RendererResourceHandles
registerDefaultRendererResources(RenderResourceRegistry &registry,
                                 Device &device,
                                 VkFormat viewportColorFormat) {
    const VkFormat hdrFormat = chooseHdrFormat(device);
    const VkFormat depthFormat = chooseDepthFormat(device, false);
    const VkFormat shadowDepthFormat = chooseDepthFormat(device, true);
    const VkSampleCountFlagBits samples =
        chooseSamples(device, hdrFormat, depthFormat);

    RendererResourceHandles handles{};
    handles.hdrColor = registry.registerImage(
        {"HDR Color", RenderExtentPolicy::Viewport, {},
         RenderResourceMultiplicity::PerFrame, hdrFormat,
         VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_ASPECT_COLOR_BIT});
    if (samples != VK_SAMPLE_COUNT_1_BIT) {
        handles.hdrMsaaColor = registry.registerImage(
            {"HDR MSAA Color", RenderExtentPolicy::Viewport, {},
             RenderResourceMultiplicity::PerFrame, hdrFormat, samples,
             VK_IMAGE_TILING_OPTIMAL,
             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
             VK_IMAGE_ASPECT_COLOR_BIT});
    }
    handles.mainDepth = registry.registerImage(
        {"Main Depth", RenderExtentPolicy::Viewport, {},
         RenderResourceMultiplicity::PerFrame, depthFormat, samples,
         VK_IMAGE_TILING_OPTIMAL,
         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_ASPECT_DEPTH_BIT});
    handles.viewportColor = registry.registerImage(
        {"Viewport Color", RenderExtentPolicy::Viewport, {},
         RenderResourceMultiplicity::PerFrame, viewportColorFormat,
         VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
             VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_ASPECT_COLOR_BIT});
    handles.directionalShadowDepth = registry.registerImage(
        {"Directional Shadow Depth", RenderExtentPolicy::Fixed,
         {kDirectionalShadowMapSize, kDirectionalShadowMapSize},
         RenderResourceMultiplicity::PerFrame, shadowDepthFormat,
         VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
             VK_IMAGE_USAGE_SAMPLED_BIT,
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_ASPECT_DEPTH_BIT});

    if (device.occlusionCullingSupport().available) {
        RenderImageDesc visibilityDepth{};
        visibilityDepth.name = "Visibility Depth";
        visibilityDepth.extentPolicy = RenderExtentPolicy::Viewport;
        visibilityDepth.multiplicity =
            RenderResourceMultiplicity::PerFrame;
        visibilityDepth.format =
            device.occlusionCullingSupport().depthFormat;
        visibilityDepth.usage =
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT;
        visibilityDepth.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        handles.visibilityDepth =
            registry.registerImage(std::move(visibilityDepth));

        RenderImageDesc hiZ{};
        hiZ.name = "Visibility HiZ";
        hiZ.extentPolicy = RenderExtentPolicy::Viewport;
        hiZ.multiplicity = RenderResourceMultiplicity::PerFrame;
        hiZ.format = device.occlusionCullingSupport().hiZFormat;
        hiZ.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_STORAGE_BIT;
        hiZ.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        hiZ.mipPolicy = RenderMipPolicy::FullChain;
        handles.visibilityHiZ = registry.registerImage(std::move(hiZ));
    }

    if (device.computeBloomSupport().available) {
        for (uint32_t level = 0;
             level < RendererResourceHandles::kBloomPyramidLevelCount;
             ++level) {
            RenderImageDesc bloom{};
            bloom.name = "Bloom/Level" + std::to_string(level);
            bloom.extentPolicy = RenderExtentPolicy::Viewport;
            bloom.multiplicity = RenderResourceMultiplicity::PerFrame;
            bloom.format = device.computeBloomSupport().format;
            bloom.usage =
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
            bloom.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            bloom.extentDivisor = 1u << (level + 1u);
            handles.bloomLevels[level] =
                registry.registerImage(std::move(bloom));
        }
    }

    const bool atmosphereSupported = device.atmosphereSupport().available;
    const VkFormat atmosphereFormat = atmosphereSupported
                                          ? device.atmosphereSupport().format
                                          : VK_FORMAT_R8G8B8A8_UNORM;
    const VkImageUsageFlags atmosphereUsage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        (atmosphereSupported ? VK_IMAGE_USAGE_STORAGE_BIT : 0u);
    const auto registerAtmosphereImage =
        [&](std::string name, VkExtent2D supportedExtent,
            RenderResourceMultiplicity multiplicity,
            uint32_t arrayLayers = 1,
            VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D) {
            RenderImageDesc desc{};
            desc.name = std::move(name);
            desc.extentPolicy = RenderExtentPolicy::Fixed;
            desc.fixedExtent = atmosphereSupported ? supportedExtent
                                                   : VkExtent2D{1, 1};
            desc.multiplicity = multiplicity;
            desc.format = atmosphereFormat;
            desc.usage = atmosphereUsage;
            desc.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            desc.externallyInitialized = true;
            desc.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            desc.arrayLayers = atmosphereSupported ? arrayLayers : 1u;
            desc.viewType = viewType;
            return registry.registerImage(std::move(desc));
        };
    handles.atmosphereTransmittance = registerAtmosphereImage(
        "Atmosphere/Transmittance", {256, 64},
        RenderResourceMultiplicity::Single);
    handles.atmosphereMultipleScattering = registerAtmosphereImage(
        "Atmosphere/MultipleScattering", {32, 32},
        RenderResourceMultiplicity::Single);
    handles.atmosphereSkyView = registerAtmosphereImage(
        "Atmosphere/SkyView", {192, 108},
        RenderResourceMultiplicity::PerFrame);
    handles.atmosphereAerialPerspective = registerAtmosphereImage(
        "Atmosphere/AerialPerspective", {32, 32},
        RenderResourceMultiplicity::PerFrame, 32,
        VK_IMAGE_VIEW_TYPE_2D_ARRAY);

    RenderSamplerDesc hdrSampler{};
    hdrSampler.name = "HDR Sampler";
    handles.hdrSampler = registry.registerSampler(std::move(hdrSampler));

    RenderSamplerDesc viewportSampler{};
    viewportSampler.name = "Viewport Sampler";
    viewportSampler.magFilter = VK_FILTER_LINEAR;
    viewportSampler.minFilter = VK_FILTER_LINEAR;
    handles.viewportSampler =
        registry.registerSampler(std::move(viewportSampler));

    RenderSamplerDesc shadowSampler{};
    shadowSampler.name = "Directional Shadow Sampler";
    shadowSampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadowSampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadowSampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadowSampler.compareEnable = true;
    shadowSampler.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    shadowSampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    handles.shadowSampler =
        registry.registerSampler(std::move(shadowSampler));

    if (device.occlusionCullingSupport().available) {
        RenderSamplerDesc depthSampler{};
        depthSampler.name = "Visibility Depth Sampler";
        depthSampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        depthSampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        handles.visibilityDepthSampler =
            registry.registerSampler(std::move(depthSampler));

        RenderSamplerDesc hiZSampler{};
        hiZSampler.name = "Visibility HiZ Sampler";
        hiZSampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        hiZSampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        hiZSampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        hiZSampler.maxLod = 32.0f;
        handles.visibilityHiZSampler =
            registry.registerSampler(std::move(hiZSampler));
    }

    if (device.computeBloomSupport().available) {
        RenderSamplerDesc bloomSampler{};
        bloomSampler.name = "Bloom Sampler";
        bloomSampler.magFilter = VK_FILTER_LINEAR;
        bloomSampler.minFilter = VK_FILTER_LINEAR;
        bloomSampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        handles.bloomSampler =
            registry.registerSampler(std::move(bloomSampler));
    }
    RenderSamplerDesc atmosphereSampler{};
    atmosphereSampler.name = "Atmosphere Sampler";
    atmosphereSampler.magFilter = atmosphereSupported ? VK_FILTER_LINEAR
                                                      : VK_FILTER_NEAREST;
    atmosphereSampler.minFilter = atmosphereSupported ? VK_FILTER_LINEAR
                                                      : VK_FILTER_NEAREST;
    atmosphereSampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    atmosphereSampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    atmosphereSampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    handles.atmosphereSampler =
        registry.registerSampler(std::move(atmosphereSampler));
    return handles;
}

void validateRenderResourceContracts(
    const std::vector<RenderImageDesc> &descriptions,
    const std::vector<RenderPassResourceUsage> &passes) {
    std::unordered_set<uint32_t> written;
    std::unordered_map<uint32_t, VkImageLayout> layouts;
    for (uint32_t i = 0; i < descriptions.size(); ++i) {
        if (descriptions[i].externallyInitialized) {
            written.insert(i);
            layouts[i] = descriptions[i].initialLayout;
        }
    }

    for (const RenderPassResourceUsage &pass : passes) {
        for (const RenderImageUsage &use : pass.images) {
            if (!use.image.valid() || use.image.index >= descriptions.size()) {
                throw std::runtime_error(pass.passName +
                                         " uses an invalid render image");
            }
            const RenderImageDesc &desc = descriptions[use.image.index];
            if ((desc.usage & requiredUsage(use.access)) == 0) {
                throw std::runtime_error(pass.passName + " uses " + desc.name +
                                         " without the required usage flag");
            }

            const bool reads =
                use.access == RenderImageAccess::SampledRead ||
                use.access ==
                    RenderImageAccess::ColorAttachmentReadWrite ||
                use.access == RenderImageAccess::StorageReadWrite;
            if (reads) {
                if (written.count(use.image.index) == 0) {
                    throw std::runtime_error(pass.passName + " reads " +
                                             desc.name + " before a writer");
                }
                const auto layout = layouts.find(use.image.index);
                if (use.requiredLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
                    (layout == layouts.end() ||
                     layout->second != use.requiredLayout)) {
                    throw std::runtime_error(pass.passName + " reads " +
                                             desc.name +
                                             " with an incompatible layout");
                }
                if (use.access ==
                        RenderImageAccess::ColorAttachmentReadWrite ||
                    use.access == RenderImageAccess::StorageReadWrite) {
                    written.insert(use.image.index);
                }
            } else {
                written.insert(use.image.index);
            }
            if (use.finalLayout != VK_IMAGE_LAYOUT_UNDEFINED)
                layouts[use.image.index] = use.finalLayout;
        }
    }
}

} // namespace vkr
