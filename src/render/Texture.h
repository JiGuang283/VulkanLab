#pragma once

#include "core/Device.h"
#include "core/Image.h"

#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace vkr {

class Renderer;

class Texture {
  public:
    Texture(Device &device, Renderer &renderer, const std::string &path);
    ~Texture();

    Texture(const Texture &) = delete;
    Texture &operator=(const Texture &) = delete;

    VkImageView imageView() const { return image_->imageView(); }
    VkSampler   sampler() const { return sampler_; }

  private:
    void loadFromFile(Renderer &renderer, const std::string &path);
    void createSampler();

    static void transitionImageLayout(VkCommandBuffer cmd, VkImage image,
                                      VkFormat format, VkImageLayout oldLayout,
                                      VkImageLayout newLayout,
                                      uint32_t      mipLevels);

    static void copyBufferToImage(VkCommandBuffer cmd, VkBuffer buffer,
                                  VkImage image, uint32_t width,
                                  uint32_t height);

    void generateMipmaps(Renderer &renderer, VkImage image, VkFormat format,
                         int32_t width, int32_t height, uint32_t mipLevels);

    Device                *device_ = nullptr;
    std::unique_ptr<Image> image_;
    VkSampler              sampler_ = VK_NULL_HANDLE;
    uint32_t               mipLevels_ = 1;
};

} // namespace vkr
