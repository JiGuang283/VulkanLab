#pragma once

#include "DerivedTextureManifest.h"
#include "core/TextureTranscodeTarget.h"
#include "assets/PreparedTextureData.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace vkr {

struct ResourceLoadStats;

class DerivedTextureCache {
  public:
    DerivedTextureCache(std::filesystem::path cacheRoot,
                        std::filesystem::path scenePath,
                        std::string projectId, std::string sceneId,
                        std::string profileId, uint32_t textureLimit,
                        TextureTranscodeTarget target,
                        bool strict,
                        ResourceLoadStats *stats);

    std::shared_ptr<const PreparedImage>
    load(int imageIndex, TextureSemantic semantic, DerivedMipmapWrap wrap);
    bool available() const { return manifest_.has_value(); }
    const std::string &status() const { return status_; }

  private:
    bool sourceMatches(const DerivedTextureEntry &entry) const;

    std::filesystem::path cacheRoot_;
    std::filesystem::path scenePath_;
    std::filesystem::path sceneDirectory_;
    std::string projectId_;
    std::string sceneId_;
    std::string profileId_;
    TextureTranscodeTarget target_ = TextureTranscodeTarget::Rgba8;
    bool strict_ = false;
    ResourceLoadStats *stats_ = nullptr;
    std::optional<DerivedTextureManifest> manifest_;
    std::string status_;
};

} // namespace vkr
