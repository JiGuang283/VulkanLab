#include "DerivedTextureCache.h"

#include "core/Log.h"
#include "diagnostics/SceneLoadStats.h"

#include <ktx.h>

#include <algorithm>
#include <chrono>
#include <system_error>

namespace vkr {

namespace {

class KtxOwner {
  public:
    ~KtxOwner() {
        if (texture_)
            ktxTexture_Destroy(ktxTexture(texture_));
    }
    ktxTexture2 **put() { return &texture_; }
    ktxTexture2 *get() const { return texture_; }

  private:
    ktxTexture2 *texture_ = nullptr;
};

bool stampsEqual(const DerivedFileStamp &expected,
                 const std::filesystem::path &path) {
    const DerivedFileStamp current = fileStamp(path);
    return current.size == expected.size &&
           current.writeTime == expected.writeTime;
}

} // namespace

DerivedTextureCache::DerivedTextureCache(
    std::filesystem::path cacheRoot, std::filesystem::path scenePath,
    std::string projectId, std::string sceneId, std::string profileId,
    uint32_t textureLimit, TextureTranscodeTarget target,
    ResourceLoadStats *stats)
    : cacheRoot_(std::move(cacheRoot)), scenePath_(std::move(scenePath)),
      sceneDirectory_(scenePath_.has_parent_path() ? scenePath_.parent_path()
                                                  : std::filesystem::path(".")),
      projectId_(std::move(projectId)), sceneId_(std::move(sceneId)),
      profileId_(std::move(profileId)),
      target_(target), stats_(stats) {
    DerivedTextureManifest manifest;
    const std::filesystem::path path =
        derivedManifestPath(cacheRoot_, sceneId_, profileId_);
    if (!loadDerivedTextureManifest(path, manifest, status_)) {
        if (stats_ && status_ != "manifest not found")
            ++stats_->derivedTextureInvalid;
        return;
    }
    if (manifest.schemaVersion != DerivedTextureManifest::kSchemaVersion ||
        manifest.projectId != projectId_ || manifest.sceneId != sceneId_ ||
        manifest.profileId != profileId_ ||
        manifest.textureLimit != textureLimit) {
        status_ = "manifest scene/profile mismatch";
        if (stats_)
            ++stats_->derivedTextureInvalid;
        return;
    }
    if (!stampsEqual(manifest.scene, scenePath_)) {
        status_ = "scene source changed";
        if (stats_)
            ++stats_->derivedTextureInvalid;
        return;
    }
    manifest_ = std::move(manifest);
    status_ = "ready";
    VKR_LOG_INFO("TextureCache", "Using derived texture manifest '{}'",
                 path.string());
}

bool DerivedTextureCache::sourceMatches(
    const DerivedTextureEntry &entry) const {
    if (entry.source.path.empty())
        return false;
    std::filesystem::path path(entry.source.path);
    if (!path.is_absolute())
        path = sceneDirectory_ / path;
    return stampsEqual(entry.source, path);
}

std::shared_ptr<const PreparedImage>
DerivedTextureCache::load(int imageIndex, TextureSemantic semantic,
                          DerivedMipmapWrap wrap) {
    if (stats_)
        ++stats_->derivedTextureLookups;
    if (!manifest_) {
        if (stats_)
            ++stats_->derivedTextureMisses;
        return {};
    }
    const DerivedTextureEntry *entry =
        manifest_->find(imageIndex, semantic, wrap);
    if (!entry) {
        if (stats_)
            ++stats_->derivedTextureMisses;
        return {};
    }
    if (!sourceMatches(*entry)) {
        if (stats_) {
            ++stats_->derivedTextureMisses;
            ++stats_->derivedTextureInvalid;
        }
        VKR_LOG_DEBUG("TextureCache", "Source changed for image {}",
                      imageIndex);
        return {};
    }

    const std::filesystem::path blobPath = cacheRoot_ / entry->blob;
    KtxOwner owner;
    KTX_error_code result = KTX_SUCCESS;
    {
        ScopedLoadTimer timer(stats_ ? &stats_->derivedTextureReadMs
                                     : nullptr);
        result = ktxTexture2_CreateFromNamedFile(
            blobPath.string().c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
            owner.put());
    }
    if (result != KTX_SUCCESS || !owner.get()) {
        if (stats_) {
            ++stats_->derivedTextureMisses;
            ++stats_->derivedTextureInvalid;
        }
        VKR_LOG_WARN("TextureCache", "Could not read '{}': {}",
                     blobPath.string(), ktxErrorString(result));
        return {};
    }

    ktxTexture2 *texture2 = owner.get();
    ktxTexture *texture = ktxTexture(texture2);
    if (texture->numDimensions != 2 || texture->numLayers != 1 ||
        texture->numFaces != 1 || texture->numLevels == 0 ||
        texture->baseWidth != entry->width ||
        texture->baseHeight != entry->height ||
        !ktxTexture2_NeedsTranscoding(texture2)) {
        if (stats_) {
            ++stats_->derivedTextureMisses;
            ++stats_->derivedTextureInvalid;
        }
        VKR_LOG_WARN("TextureCache", "Invalid derived texture '{}'",
                     blobPath.string());
        return {};
    }

    const ktx_transcode_fmt_e transcodeFormat =
        target_ == TextureTranscodeTarget::Bc7 ? KTX_TTF_BC7_RGBA
                                               : KTX_TTF_RGBA32;
    {
        ScopedLoadTimer timer(stats_ ? &stats_->derivedTextureTranscodeMs
                                     : nullptr);
        result = ktxTexture2_TranscodeBasis(texture2, transcodeFormat, 0);
    }
    if (result != KTX_SUCCESS) {
        if (stats_) {
            ++stats_->derivedTextureMisses;
            ++stats_->derivedTextureInvalid;
        }
        VKR_LOG_WARN("TextureCache", "Could not transcode '{}': {}",
                     blobPath.string(), ktxErrorString(result));
        return {};
    }

    auto prepared = std::make_shared<PreparedImage>();
    prepared->width = texture->baseWidth;
    prepared->height = texture->baseHeight;
    prepared->kind = PreparedTextureDataKind::PrebuiltMipChain;
    const bool srgb = semantic == TextureSemantic::SrgbColor;
    prepared->format = target_ == TextureTranscodeTarget::Bc7
                           ? (srgb ? VK_FORMAT_BC7_SRGB_BLOCK
                                   : VK_FORMAT_BC7_UNORM_BLOCK)
                           : (srgb ? VK_FORMAT_R8G8B8A8_SRGB
                                   : VK_FORMAT_R8G8B8A8_UNORM);

    const uint8_t *data = ktxTexture_GetData(texture);
    for (uint32_t level = 0; level < texture->numLevels; ++level) {
        ktx_size_t sourceOffset = 0;
        result = ktxTexture_GetImageOffset(texture, level, 0, 0,
                                           &sourceOffset);
        if (result != KTX_SUCCESS)
            return {};
        const ktx_size_t levelSize = ktxTexture_GetImageSize(texture, level);
        PreparedMipLevel mip;
        mip.offset = prepared->pixels.size();
        mip.size = levelSize;
        mip.width = std::max(1u, texture->baseWidth >> level);
        mip.height = std::max(1u, texture->baseHeight >> level);
        prepared->pixels.insert(prepared->pixels.end(), data + sourceOffset,
                                data + sourceOffset + levelSize);
        prepared->mipLevels.push_back(mip);
    }

    if (stats_) {
        ++stats_->derivedTextureHits;
        std::error_code sizeError;
        const uint64_t blobSize = std::filesystem::file_size(blobPath, sizeError);
        if (!sizeError)
            stats_->derivedTextureReadBytes += blobSize;
        if (target_ == TextureTranscodeTarget::Bc7)
            ++stats_->bc7TextureCount;
        else
            ++stats_->rgbaTranscodeFallbackCount;
    }
    return prepared;
}

} // namespace vkr
