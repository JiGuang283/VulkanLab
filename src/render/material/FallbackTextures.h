#pragma once

#include "MaterialTextureSlot.h"

#include <memory>

namespace vkr {

class Device;
class Texture;
class UploadRecorder;

class FallbackTextures {
  public:
    FallbackTextures(Device &device, UploadRecorder &upload);
    FallbackTextures(std::shared_ptr<Texture> white,
                     std::shared_ptr<Texture> black,
                     std::shared_ptr<Texture> flatNormal);

    std::shared_ptr<Texture> textureFor(MaterialTextureSlot slot) const;
    std::shared_ptr<Texture> white() const { return white_; }
    std::shared_ptr<Texture> black() const { return black_; }
    std::shared_ptr<Texture> flatNormal() const { return flatNormal_; }

  private:
    std::shared_ptr<Texture> white_;
    std::shared_ptr<Texture> black_;
    std::shared_ptr<Texture> flatNormal_;
};

} // namespace vkr
