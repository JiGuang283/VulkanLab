#pragma once

#include "core/Device.h"
#include "core/Image.h"

#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class UploadRecorder;

struct TextureMipLevelInfo {
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct TextureSubresourceInfo {
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevel = 0;
    uint32_t arrayLayer = 0;
};

struct TextureCreateInfo {
    const void *pixels = nullptr;
    VkDeviceSize dataSize = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t maxExtent = 0;
    bool generateMipmaps = true;
    VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
    VkFilter minFilter = VK_FILTER_LINEAR;
    VkFilter magFilter = VK_FILTER_LINEAR;
    VkSamplerMipmapMode mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSamplerAddressMode wrapU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode wrapV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode wrapW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    const TextureMipLevelInfo *mipLevels = nullptr;
    uint32_t mipLevelCount = 0;
    const TextureSubresourceInfo *subresources = nullptr;
    uint32_t subresourceCount = 0;
    uint32_t arrayLayers = 1;
    VkImageCreateFlags imageFlags = 0;
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
    std::string debugName;
};

class Texture {
  public:
    Texture(Device &device, UploadRecorder &upload, const std::string &path);
    Texture(Device &device, UploadRecorder &upload,
            const TextureCreateInfo &info);
    ~Texture();

    Texture(const Texture &) = delete;
    Texture &operator=(const Texture &) = delete;

    VkImageView imageView() const { return image_->imageView(); }
    VkSampler sampler() const { return sampler_; }
    uint32_t mipLevels() const { return mipLevels_; }
    uint32_t arrayLayers() const { return arrayLayers_; }
    VkFormat format() const { return format_; }

  private:
    void createFromPixels(UploadRecorder &upload, const void *pixels,
                          uint32_t width, uint32_t height, VkFormat format,
                          bool generateMipmapsFlag, uint32_t maxExtent);
    void createFromMipChain(UploadRecorder &upload,
                            const TextureCreateInfo &info);
    void createFromSubresources(UploadRecorder &upload,
                                const TextureCreateInfo &info);
    void createSamplerFrom(VkFilter minFilter, VkFilter magFilter,
                           VkSamplerMipmapMode mipmapMode,
                           VkSamplerAddressMode wrapU,
                           VkSamplerAddressMode wrapV,
                           VkSamplerAddressMode wrapW);

    static void transitionImageLayout(VkCommandBuffer cmd, VkImage image,
                                      VkFormat format, VkImageLayout oldLayout,
                                      VkImageLayout newLayout,
                                      uint32_t mipLevels,
                                      uint32_t arrayLayers = 1);
    static void copyBufferToImage(VkCommandBuffer cmd, VkBuffer buffer,
                                  VkDeviceSize bufferOffset, VkImage image,
                                  uint32_t width, uint32_t height);
    static void copyMipChainToImage(
        VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize bufferOffset,
        VkImage image, const TextureMipLevelInfo *levels,
        uint32_t levelCount);
    static void copySubresourcesToImage(
        VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize bufferOffset,
        VkImage image, const TextureSubresourceInfo *subresources,
        uint32_t subresourceCount);
    void generateMipmaps(VkCommandBuffer commandBuffer, VkImage image,
                         VkFormat format, int32_t width, int32_t height,
                         uint32_t mipLevels);

    Device *device_ = nullptr;
    std::unique_ptr<Image> image_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    uint32_t mipLevels_ = 1;
    uint32_t arrayLayers_ = 1;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    std::string debugName_;
};

} // namespace vkr
