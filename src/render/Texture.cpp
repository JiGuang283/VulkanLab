#include "Texture.h"
#include "TextureData.h"
#include "core/GpuDebugUtils.h"
#include "core/Log.h"
#include "core/UploadRecorder.h"
#include "core/VulkanCheck.h"
#include "diagnostics/SceneLoadStats.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace vkr {

Texture::Texture(Device &device, UploadRecorder &upload, const std::string &path)
    : device_(&device),
      debugName_("Texture/" +
                 std::filesystem::path(path).filename().string()) {
    ResourceLoadStats *loadStats = upload.stats();
    int      w = 0, h = 0, c = 0;
    stbi_uc *pixels = stbi_load(path.c_str(), &w, &h, &c, STBI_rgb_alpha);
    if (!pixels) {
        throw std::runtime_error("failed to load texture image!");
    }
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixelOwner(
        pixels, &stbi_image_free);
    if (loadStats) {
        ++loadStats->textureDecodeCount;
        loadStats->decodedRgbaBytes +=
            static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4;
    }
    try {
        createSamplerFrom(
            VK_FILTER_LINEAR, VK_FILTER_LINEAR,
            VK_SAMPLER_MIPMAP_MODE_LINEAR,
            VK_SAMPLER_ADDRESS_MODE_REPEAT,
            VK_SAMPLER_ADDRESS_MODE_REPEAT);
        createFromPixels(upload, pixelOwner.get(), static_cast<uint32_t>(w),
                         static_cast<uint32_t>(h),
                         VK_FORMAT_R8G8B8A8_SRGB,
                         /*generateMipmapsFlag*/ true, /*maxExtent*/ 0);
    } catch (...) {
        if (sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(device_->logicalDevice(), sampler_, nullptr);
            sampler_ = VK_NULL_HANDLE;
        }
        throw;
    }
}

Texture::Texture(Device &device, UploadRecorder &upload,
                 const TextureCreateInfo &info)
    : device_(&device),
      debugName_(info.debugName.empty() ? "Texture/Unnamed"
                                        : info.debugName) {
    if (!info.pixels || info.width == 0 || info.height == 0) {
        throw std::runtime_error("TextureCreateInfo: missing pixels/size");
    }
    try {
        createSamplerFrom(info.minFilter, info.magFilter, info.mipmapMode,
                          info.wrapU, info.wrapV);
        if (info.mipLevels && info.mipLevelCount > 0) {
            createFromMipChain(upload, info);
        } else {
            createFromPixels(upload, info.pixels, info.width, info.height,
                             info.format, info.generateMipmaps,
                             info.maxExtent);
        }
    } catch (...) {
        if (sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(device_->logicalDevice(), sampler_, nullptr);
            sampler_ = VK_NULL_HANDLE;
        }
        throw;
    }
}

Texture::~Texture() {
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_->logicalDevice(), sampler_, nullptr);
    }
}

void Texture::createFromPixels(UploadRecorder &upload, const void *pixels,
                               uint32_t width, uint32_t height, VkFormat format,
                               bool generateMipmapsFlag, uint32_t maxExtent) {
    ResourceLoadStats *loadStats = upload.stats();
    uint32_t uploadWidth = width;
    uint32_t uploadHeight = height;
    std::vector<uint8_t> resizedPixels;
    const auto *uploadPixels = static_cast<const uint8_t *>(pixels);
    if (limitedTextureExtent(width, height, maxExtent, uploadWidth,
                             uploadHeight)) {
        ScopedLoadTimer resizeTimer(loadStats ? &loadStats->textureResizeMs
                                              : nullptr);
        resizedPixels = resizeRgba8Bilinear(uploadPixels, width, height,
                                            uploadWidth, uploadHeight);
        uploadPixels = resizedPixels.data();
        if (loadStats)
            ++loadStats->resizedTextureCount;
        VKR_LOG_INFO("Texture", "Resized texture {}x{} -> {}x{} (limit {})",
                     width, height, uploadWidth, uploadHeight, maxExtent);
    }

    VkDeviceSize imageSize =
        static_cast<VkDeviceSize>(uploadWidth) * uploadHeight * 4; // RGBA8

    mipLevels_ = generateMipmapsFlag
                     ? (static_cast<uint32_t>(
                            std::floor(std::log2(std::max(uploadWidth,
                                                          uploadHeight)))) +
                        1)
                     : 1u;

    ScopedLoadTimer uploadTimer(loadStats ? &loadStats->textureUploadMs
                                          : nullptr);

    const StagedSlice staged = upload.stageBytes(uploadPixels, imageSize);

    image_ = std::make_unique<Image>(
        *device_, static_cast<int>(uploadWidth), static_cast<int>(uploadHeight),
        mipLevels_, VK_SAMPLE_COUNT_1_BIT, format, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, debugName_ + "/Image");

    image_->createView(format, VK_IMAGE_ASPECT_COLOR_BIT, mipLevels_);

    if (generateMipmapsFlag) {
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(device_->physicalDevice(), format,
                                            &formatProperties);
        if (!(formatProperties.optimalTilingFeatures &
              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
            throw std::runtime_error(
                "texture image format does not support linear blitting!");
        }
    }

    // transition UNDEFINED → TRANSFER_DST_OPTIMAL + copy
    VkCommandBuffer cmd = upload.commandBuffer();
    transitionImageLayout(cmd, image_->handle(), format,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mipLevels_);
    copyBufferToImage(cmd, staged.buffer, staged.offset, image_->handle(),
                      uploadWidth, uploadHeight);

    if (generateMipmapsFlag) {
        generateMipmaps(cmd, image_->handle(), format,
                        static_cast<int32_t>(uploadWidth),
                        static_cast<int32_t>(uploadHeight), mipLevels_);
    } else {
        // 单独把 mip 0 从 TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
        transitionImageLayout(cmd, image_->handle(), format,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              /*mipLevels*/ 1);
    }

    if (loadStats) {
        ++loadStats->gpuTextureCount;
        loadStats->textureUploadBytes += static_cast<uint64_t>(imageSize);
        loadStats->textureGpuBytesEstimated +=
            rgba8MipChainBytes(uploadWidth, uploadHeight, mipLevels_);
    }
}

void Texture::createFromMipChain(UploadRecorder &upload,
                                 const TextureCreateInfo &info) {
    if (info.dataSize == 0 || !info.mipLevels || info.mipLevelCount == 0)
        throw std::runtime_error("TextureCreateInfo: invalid mip chain");
    if (info.mipLevels[0].width != info.width ||
        info.mipLevels[0].height != info.height) {
        throw std::runtime_error("TextureCreateInfo: mip 0 size mismatch");
    }
    for (uint32_t level = 0; level < info.mipLevelCount; ++level) {
        const TextureMipLevelInfo &mip = info.mipLevels[level];
        if (mip.width == 0 || mip.height == 0 || mip.size == 0 ||
            mip.offset > info.dataSize || mip.size > info.dataSize - mip.offset) {
            throw std::runtime_error("TextureCreateInfo: invalid mip range");
        }
    }

    ResourceLoadStats *loadStats = upload.stats();
    ScopedLoadTimer uploadTimer(loadStats ? &loadStats->textureUploadMs
                                          : nullptr);
    const StagedSlice staged = upload.stageBytes(info.pixels, info.dataSize);
    mipLevels_ = info.mipLevelCount;
    image_ = std::make_unique<Image>(
        *device_, static_cast<int>(info.width), static_cast<int>(info.height),
        mipLevels_, VK_SAMPLE_COUNT_1_BIT, info.format,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, debugName_ + "/Image");
    image_->createView(info.format, VK_IMAGE_ASPECT_COLOR_BIT, mipLevels_);

    VkCommandBuffer commandBuffer = upload.commandBuffer();
    transitionImageLayout(commandBuffer, image_->handle(), info.format,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mipLevels_);
    copyMipChainToImage(commandBuffer, staged.buffer, staged.offset,
                        image_->handle(), info.mipLevels, mipLevels_);
    transitionImageLayout(commandBuffer, image_->handle(), info.format,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          mipLevels_);

    if (loadStats) {
        ++loadStats->gpuTextureCount;
        ++loadStats->prebuiltMipTextureCount;
        loadStats->textureUploadBytes += info.dataSize;
        loadStats->textureGpuBytesEstimated += info.dataSize;
    }
}

void Texture::createSamplerFrom(VkFilter minFilter, VkFilter magFilter,
                                VkSamplerMipmapMode  mipmapMode,
                                VkSamplerAddressMode wrapU,
                                VkSamplerAddressMode wrapV) {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = magFilter;
    samplerInfo.minFilter = minFilter;
    samplerInfo.addressModeU = wrapU;
    samplerInfo.addressModeV = wrapV;
    samplerInfo.addressModeW = wrapU; // 2D 贴图，用 U 兜底
    samplerInfo.anisotropyEnable = VK_TRUE;

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device_->physicalDevice(), &properties);
    samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;

    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = mipmapMode;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;

    VK_CHECK(vkCreateSampler(device_->logicalDevice(), &samplerInfo, nullptr,
                             &sampler_));
    device_->debugUtils().setObjectName(VK_OBJECT_TYPE_SAMPLER, sampler_,
                                        debugName_ + "/Sampler");
}

void Texture::transitionImageLayout(VkCommandBuffer cmd, VkImage image,
                                    VkFormat format, VkImageLayout oldLayout,
                                    VkImageLayout newLayout,
                                    uint32_t      mipLevels) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;

    if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
            format == VK_FORMAT_D24_UNORM_S8_UINT) {
            barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
    } else {
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    }

    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
        newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
               newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    } else {
        throw std::invalid_argument("unsupported layout transition!");
    }

    vkCmdPipelineBarrier(cmd, sourceStage, destinationStage, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
}

void Texture::copyBufferToImage(VkCommandBuffer cmd, VkBuffer buffer,
                                VkDeviceSize bufferOffset, VkImage image,
                                uint32_t width, uint32_t height) {
    VkBufferImageCopy region{};
    region.bufferOffset = bufferOffset;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;

    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;

    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(cmd, buffer, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void Texture::copyMipChainToImage(
    VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize bufferOffset,
    VkImage image, const TextureMipLevelInfo *levels, uint32_t levelCount) {
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(levelCount);
    for (uint32_t level = 0; level < levelCount; ++level) {
        VkBufferImageCopy region{};
        region.bufferOffset = bufferOffset + levels[level].offset;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = level;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {levels[level].width, levels[level].height, 1};
        regions.push_back(region);
    }
    vkCmdCopyBufferToImage(cmd, buffer, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()),
                           regions.data());
}

void Texture::generateMipmaps(VkCommandBuffer commandBuffer, VkImage image,
                              VkFormat format, int32_t width, int32_t height,
                              uint32_t mipLevels) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = image;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.subresourceRange.levelCount = 1;

    int32_t mipWidth = width;
    int32_t mipHeight = height;

    for (uint32_t i = 1; i < mipLevels; i++) {
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);

        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {mipWidth > 1 ? mipWidth / 2 : 1,
                              mipHeight > 1 ? mipHeight / 2 : 1, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;

        vkCmdBlitImage(
            commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);

        if (mipWidth > 1)
            mipWidth /= 2;
        if (mipHeight > 1)
            mipHeight /= 2;
    }

    barrier.subresourceRange.baseMipLevel = mipLevels - 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);

}

} // namespace vkr
