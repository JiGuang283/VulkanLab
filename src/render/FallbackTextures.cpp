#include "FallbackTextures.h"

#include "Texture.h"
#include "core/Device.h"
#include "core/UploadRecorder.h"

#include <array>
#include <stdexcept>
#include <utility>

namespace vkr {

namespace {
std::shared_ptr<Texture> makeSolidTexture(Device &device,
                                          UploadRecorder &upload,
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

FallbackTextures::FallbackTextures(Device &device, UploadRecorder &upload)
    : white_(makeSolidTexture(device, upload, {255, 255, 255, 255},
                              VK_FORMAT_R8G8B8A8_SRGB)),
      black_(makeSolidTexture(device, upload, {0, 0, 0, 255},
                              VK_FORMAT_R8G8B8A8_SRGB)),
      flatNormal_(makeSolidTexture(device, upload, {128, 128, 255, 255},
                                   VK_FORMAT_R8G8B8A8_UNORM)) {}

FallbackTextures::FallbackTextures(std::shared_ptr<Texture> white,
                                   std::shared_ptr<Texture> black,
                                   std::shared_ptr<Texture> flatNormal)
    : white_(std::move(white)), black_(std::move(black)),
      flatNormal_(std::move(flatNormal)) {
    if (!white_ || !black_ || !flatNormal_)
        throw std::invalid_argument("Fallback textures cannot be null");
}

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
