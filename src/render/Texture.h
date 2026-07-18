#pragma once

#include "core/Device.h"
#include "core/Image.h"

#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class UploadRecorder;

struct TextureCreateInfo {
    const void          *pixels = nullptr; // RGBA8，tightly packed
    uint32_t             width = 0;
    uint32_t             height = 0;
    uint32_t             maxExtent = 0; // 0 = Full resolution
    bool                 generateMipmaps = true;
    VkFormat             format = VK_FORMAT_R8G8B8A8_SRGB;
    VkFilter             minFilter = VK_FILTER_LINEAR;
    VkFilter             magFilter = VK_FILTER_LINEAR;
    VkSamplerMipmapMode  mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSamplerAddressMode wrapU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode wrapV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
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
    VkSampler   sampler() const { return sampler_; }

  private:
    // 从已解码 RGBA8 像素创建 Image + 上传 + (可选) mipmap。
    // 结束时 image_ 处于 SHADER_READ_ONLY_OPTIMAL 布局，view 已创建。
    void createFromPixels(UploadRecorder &upload, const void *pixels,
                          uint32_t width, uint32_t height, VkFormat format,
                          bool generateMipmapsFlag, uint32_t maxExtent);

    // 根据 filter / wrap 参数创建 sampler_（anisotropy 仍取设备上限）。
    void createSamplerFrom(VkFilter minFilter, VkFilter magFilter,
                           VkSamplerMipmapMode  mipmapMode,
                           VkSamplerAddressMode wrapU,
                           VkSamplerAddressMode wrapV);

    static void transitionImageLayout(VkCommandBuffer cmd, VkImage image,
                                      VkFormat format, VkImageLayout oldLayout,
                                      VkImageLayout newLayout,
                                      uint32_t      mipLevels);

    static void copyBufferToImage(VkCommandBuffer cmd, VkBuffer buffer,
                                  VkDeviceSize bufferOffset, VkImage image,
                                  uint32_t width, uint32_t height);

    void generateMipmaps(VkCommandBuffer commandBuffer, VkImage image,
                         VkFormat format, int32_t width, int32_t height,
                         uint32_t mipLevels);

    Device                *device_ = nullptr;
    std::unique_ptr<Image> image_;
    VkSampler              sampler_ = VK_NULL_HANDLE;
    uint32_t               mipLevels_ = 1;
};

} // namespace vkr
