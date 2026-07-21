#include "FrameRenderTargets.h"

#include "core/Device.h"
#include "core/Image.h"
#include "core/VulkanCheck.h"
#include "render/DirectionalShadow.h"

#include <array>
#include <stdexcept>

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

} // namespace

FrameRenderTargets::FrameRenderTargets(Device &device, VkExtent2D extent)
    : device_(&device), extent_(extent) {
    hdrFormat_ = chooseHdrFormat();
    depthFormat_ = chooseDepthFormat(false);
    shadowDepthFormat_ = chooseDepthFormat(true);
    samples_ = chooseSamples();
    createSamplers();
    createShadowTargets();
    recreateExtentTargets(extent);
}

FrameRenderTargets::~FrameRenderTargets() {
    releaseExtentTargets();
    for (auto &frameTarget : frames_)
        frameTarget.shadowDepth.reset();
    if (shadowSampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device_->logicalDevice(), shadowSampler_, nullptr);
    if (hdrSampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device_->logicalDevice(), hdrSampler_, nullptr);
}

void FrameRenderTargets::releaseExtentTargets() {
    for (auto &frameTarget : frames_) {
        frameTarget.depth.reset();
        frameTarget.hdrMsaaColor.reset();
        frameTarget.hdrColor.reset();
    }
}

void FrameRenderTargets::recreateExtentTargets(VkExtent2D extent) {
    releaseExtentTargets();
    extent_ = extent;
    if (extent.width == 0 || extent.height == 0)
        return;

    for (auto &frameTarget : frames_) {
        frameTarget.hdrColor = std::make_unique<Image>(
            *device_, extent.width, extent.height, 1, VK_SAMPLE_COUNT_1_BIT,
            hdrFormat_, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        frameTarget.hdrColor->createView(hdrFormat_,
                                         VK_IMAGE_ASPECT_COLOR_BIT, 1);

        if (samples_ != VK_SAMPLE_COUNT_1_BIT) {
            frameTarget.hdrMsaaColor = std::make_unique<Image>(
                *device_, extent.width, extent.height, 1, samples_, hdrFormat_,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            frameTarget.hdrMsaaColor->createView(
                hdrFormat_, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        }

        frameTarget.depth = std::make_unique<Image>(
            *device_, extent.width, extent.height, 1, samples_, depthFormat_,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        frameTarget.depth->createView(depthFormat_,
                                      VK_IMAGE_ASPECT_DEPTH_BIT, 1);
    }
}

VkFormat FrameRenderTargets::chooseHdrFormat() const {
    constexpr std::array<VkFormat, 2> candidates = {
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
    const VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    for (VkFormat format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(device_->physicalDevice(), format,
                                            &properties);
        if ((properties.optimalTilingFeatures & required) == required)
            return format;
    }
    throw std::runtime_error(
        "No floating-point HDR color attachment format is supported");
}

VkFormat FrameRenderTargets::chooseDepthFormat(bool sampled) const {
    constexpr std::array<VkFormat, 3> candidates = {
        VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D16_UNORM};
    VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (sampled)
        required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    for (VkFormat format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(device_->physicalDevice(), format,
                                            &properties);
        if ((properties.optimalTilingFeatures & required) == required)
            return format;
    }
    throw std::runtime_error(sampled
                                 ? "No sampled depth format is supported"
                                 : "No depth attachment format is supported");
}

VkSampleCountFlagBits FrameRenderTargets::chooseSamples() const {
    VkImageFormatProperties hdrProperties{};
    VkImageFormatProperties depthProperties{};
    const VkResult hdrResult = vkGetPhysicalDeviceImageFormatProperties(
        device_->physicalDevice(), hdrFormat_, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
        0, &hdrProperties);
    const VkResult depthResult = vkGetPhysicalDeviceImageFormatProperties(
        device_->physicalDevice(), depthFormat_, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &depthProperties);
    if (hdrResult != VK_SUCCESS || depthResult != VK_SUCCESS)
        return VK_SAMPLE_COUNT_1_BIT;
    const VkSampleCountFlags supported =
        hdrProperties.sampleCounts & depthProperties.sampleCounts &
        static_cast<VkSampleCountFlags>(device_->msaaSamples() |
                                        (device_->msaaSamples() - 1));
    return highestSampleCount(supported);
}

void FrameRenderTargets::createSamplers() {
    // Tone mapping is 1:1 and shadow PCF is explicit, so sampled formats do
    // not need to advertise optional linear-filter support.
    VkSamplerCreateInfo hdrInfo{};
    hdrInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    hdrInfo.magFilter = VK_FILTER_NEAREST;
    hdrInfo.minFilter = VK_FILTER_NEAREST;
    hdrInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    hdrInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    hdrInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    hdrInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    hdrInfo.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(device_->logicalDevice(), &hdrInfo, nullptr,
                             &hdrSampler_));

    VkSamplerCreateInfo shadowInfo = hdrInfo;
    shadowInfo.magFilter = VK_FILTER_NEAREST;
    shadowInfo.minFilter = VK_FILTER_NEAREST;
    shadowInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadowInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadowInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadowInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    shadowInfo.compareEnable = VK_TRUE;
    shadowInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VK_CHECK(vkCreateSampler(device_->logicalDevice(), &shadowInfo, nullptr,
                             &shadowSampler_));
}

void FrameRenderTargets::createShadowTargets() {
    for (auto &frameTarget : frames_) {
        frameTarget.shadowDepth = std::make_unique<Image>(
            *device_, kDirectionalShadowMapSize, kDirectionalShadowMapSize, 1,
            VK_SAMPLE_COUNT_1_BIT, shadowDepthFormat_,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        frameTarget.shadowDepth->createView(
            shadowDepthFormat_, VK_IMAGE_ASPECT_DEPTH_BIT, 1);
    }
}

} // namespace vkr
