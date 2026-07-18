#include "FallbackTextures.h"

#include "Texture.h"
#include "core/Device.h"
#include "core/UploadContext.h"

#include <array>

namespace vkr {

namespace {
std::shared_ptr<Texture> makeSolidTexture(Device &device,
                                          UploadContext &upload,
                                          std::array<uint8_t, 4> rgba,
                                          VkFormat format) {
    TextureCreateInfo info{};
    info.pixels = rgba.data();
    info.width = 1;
    info.height = 1;
    info.generateMipmaps = false;
    info.format = format;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    return std::make_shared<Texture>(device, upload, info);
}
} // namespace

FallbackTextures::FallbackTextures(Device &device, UploadContext &upload)
    : white_(makeSolidTexture(device, upload, {255, 255, 255, 255},
                              VK_FORMAT_R8G8B8A8_SRGB)),
      black_(makeSolidTexture(device, upload, {0, 0, 0, 255},
                              VK_FORMAT_R8G8B8A8_SRGB)),
      flatNormal_(makeSolidTexture(device, upload, {128, 128, 255, 255},
                                   VK_FORMAT_R8G8B8A8_UNORM)) {}

std::shared_ptr<Texture>
FallbackTextures::textureFor(MaterialTextureSlot slot) const {
    switch (slot) {
    case MaterialTextureSlot::BaseColor:
    case MaterialTextureSlot::MetallicRoughness:
    case MaterialTextureSlot::Occlusion:
        return white_;
    case MaterialTextureSlot::Normal:
        return flatNormal_;
    case MaterialTextureSlot::Emissive:
        return black_;
    case MaterialTextureSlot::Count:
        break;
    }
    return white_;
}

} // namespace vkr
