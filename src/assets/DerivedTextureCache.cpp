#include "DerivedTextureCache.h"

#include "core/Log.h"
#include "diagnostics/SceneLoadStats.h"

#include <ktx.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
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
    bool strict, ResourceLoadStats *stats)
    : cacheRoot_(std::move(cacheRoot)), scenePath_(std::move(scenePath)),
      sceneDirectory_(scenePath_.has_parent_path() ? scenePath_.parent_path()
                                                  : std::filesystem::path(".")),
      projectId_(std::move(projectId)), sceneId_(std::move(sceneId)),
      profileId_(std::move(profileId)),
      target_(target), strict_(strict), stats_(stats) {
    DerivedTextureManifest manifest;
    const std::filesystem::path path =
        derivedManifestPath(cacheRoot_, sceneId_, profileId_);
    if (!loadDerivedTextureManifest(path, manifest, status_)) {
        if (stats_ && status_ != "manifest not found")
            ++stats_->derivedTextureInvalid;
        if (strict_)
            throw std::runtime_error("Cooked texture manifest error: " +
                                     status_ + " (" + path.string() + ")");
        return;
    }
    if (manifest.schemaVersion <
            DerivedTextureManifest::kUastcSchemaVersion ||
        manifest.projectId != projectId_ || manifest.sceneId != sceneId_ ||
        manifest.profileId != profileId_ ||
        manifest.textureLimit != textureLimit) {
        status_ = "manifest scene/profile mismatch";
        if (stats_)
            ++stats_->derivedTextureInvalid;
        if (strict_)
            throw std::runtime_error("Cooked texture manifest identity mismatch: " +
                                     path.string());
        return;
    }
    if (!stampsEqual(manifest.scene, scenePath_)) {
        status_ = "scene source changed";
        if (stats_)
            ++stats_->derivedTextureInvalid;
        if (strict_)
            throw std::runtime_error("Cooked scene stamp mismatch: " +
                                     scenePath_.string());
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
        if (strict_)
            throw std::runtime_error("Cooked texture manifest is unavailable: " +
                                     status_);
        return {};
    }
    const DerivedTextureEntry *entry =
        manifest_->find(imageIndex, semantic, wrap);
    if (!entry) {
        if (stats_)
            ++stats_->derivedTextureMisses;
        if (strict_)
            throw std::runtime_error(
                "Cooked texture entry is missing for image " +
                std::to_string(imageIndex) + " (" +
                textureSemanticName(semantic) + ")");
        return {};
    }
    if (!sourceMatches(*entry)) {
        if (stats_) {
            ++stats_->derivedTextureMisses;
            ++stats_->derivedTextureInvalid;
        }
        VKR_LOG_DEBUG("TextureCache", "Source changed for image {}",
                      imageIndex);
        if (strict_)
            throw std::runtime_error(
                "Cooked texture source stamp mismatch for image " +
                std::to_string(imageIndex));
        return {};
    }

    const std::filesystem::path blobPath = cacheRoot_ / entry->blob;
    KtxOwner owner;
    KTX_error_code result = KTX_SUCCESS;
    {
        ScopedLoadTimer timer(stats_ ? &stats_->derivedTextureReadMs
                                     : nullptr);
        ScopedLoadTimer nativeTimer(
            stats_ && entry->payloadKind ==
                          DerivedTexturePayloadKind::NativeBc7
                ? &stats_->nativeTextureReadMs
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
        if (strict_)
            throw std::runtime_error("Could not read cooked KTX2 blob: " +
                                     blobPath.string());
        return {};
    }

    ktxTexture2 *texture2 = owner.get();
    ktxTexture *texture = ktxTexture(texture2);
    if (texture->numDimensions != 2 || texture->numLayers != 1 ||
        texture->numFaces != 1 || texture->numLevels == 0 ||
        texture->baseWidth != entry->width ||
        texture->baseHeight != entry->height) {
        if (stats_) {
            ++stats_->derivedTextureMisses;
            ++stats_->derivedTextureInvalid;
        }
        VKR_LOG_WARN("TextureCache", "Invalid derived texture '{}'",
                     blobPath.string());
        if (strict_)
            throw std::runtime_error("Invalid cooked KTX2 texture: " +
                                     blobPath.string());
        return {};
    }

    const bool nativeBc7 =
        entry->payloadKind == DerivedTexturePayloadKind::NativeBc7;
    const bool srgb = semantic == TextureSemantic::SrgbColor;
    const uint32_t expectedNativeFormat =
        srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    if (nativeBc7) {
        if (target_ != TextureTranscodeTarget::Bc7) {
            if (stats_)
                ++stats_->derivedTextureMisses;
            VKR_LOG_WARN(
                "TextureCache",
                "cache=native-bc7 fallback=source reason=native_bc7_unsupported "
                "image={}",
                imageIndex);
            if (strict_) {
                throw std::runtime_error(
                    "bc7_required: cooked texture requires native BC7 "
                    "support: " +
                    blobPath.string());
            }
            return {};
        }
        if (ktxTexture2_NeedsTranscoding(texture2) ||
            texture2->vkFormat != expectedNativeFormat ||
            (entry->vkFormat != 0 &&
             entry->vkFormat != texture2->vkFormat) ||
            texture2->supercompressionScheme != KTX_SS_NONE ||
            (entry->mipLevels != 0 &&
             entry->mipLevels != texture->numLevels)) {
            if (stats_) {
                ++stats_->derivedTextureMisses;
                ++stats_->derivedTextureInvalid;
            }
            VKR_LOG_WARN("TextureCache", "Invalid native BC7 texture '{}'",
                         blobPath.string());
            if (strict_)
                throw std::runtime_error(
                    "Invalid cooked native BC7 texture: " +
                    blobPath.string());
            return {};
        }
    } else {
        if (!ktxTexture2_NeedsTranscoding(texture2)) {
            if (stats_) {
                ++stats_->derivedTextureMisses;
                ++stats_->derivedTextureInvalid;
            }
            if (strict_)
                throw std::runtime_error(
                    "Cooked UASTC entry contains a native texture: " +
                    blobPath.string());
            return {};
        }
        const ktx_transcode_fmt_e transcodeFormat =
            target_ == TextureTranscodeTarget::Bc7 ? KTX_TTF_BC7_RGBA
                                                   : KTX_TTF_RGBA32;
        {
            ScopedLoadTimer timer(stats_ ? &stats_->derivedTextureTranscodeMs
                                         : nullptr);
            result =
                ktxTexture2_TranscodeBasis(texture2, transcodeFormat, 0);
        }
        if (stats_)
            ++stats_->basisTranscodeCount;
        if (result != KTX_SUCCESS) {
            if (stats_) {
                ++stats_->derivedTextureMisses;
                ++stats_->derivedTextureInvalid;
            }
            VKR_LOG_WARN("TextureCache", "Could not transcode '{}': {}",
                         blobPath.string(), ktxErrorString(result));
            if (strict_)
                throw std::runtime_error(
                    "Could not transcode cooked KTX2 blob: " +
                    blobPath.string());
            return {};
        }
    }

    auto prepared = std::make_shared<PreparedImage>();
    prepared->width = texture->baseWidth;
    prepared->height = texture->baseHeight;
    prepared->kind = PreparedTextureDataKind::PrebuiltMipChain;
    prepared->format = nativeBc7 || target_ == TextureTranscodeTarget::Bc7
                           ? (srgb ? VK_FORMAT_BC7_SRGB_BLOCK
                                   : VK_FORMAT_BC7_UNORM_BLOCK)
                           : (srgb ? VK_FORMAT_R8G8B8A8_SRGB
                                   : VK_FORMAT_R8G8B8A8_UNORM);

    const uint8_t *data = ktxTexture_GetData(texture);
    for (uint32_t level = 0; level < texture->numLevels; ++level) {
        ktx_size_t sourceOffset = 0;
        result = ktxTexture_GetImageOffset(texture, level, 0, 0,
                                           &sourceOffset);
        if (result != KTX_SUCCESS) {
            if (strict_)
                throw std::runtime_error(
                    "Invalid mip offsets in cooked KTX2 blob: " +
                    blobPath.string());
            return {};
        }
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

    if (entry->payloadBytes != 0 &&
        entry->payloadBytes != prepared->pixels.size()) {
        if (stats_) {
            ++stats_->derivedTextureMisses;
            ++stats_->derivedTextureInvalid;
        }
        VKR_LOG_WARN(
            "TextureCache",
            "Derived texture payload size mismatch for '{}': expected {}, "
            "found {}",
            blobPath.string(), entry->payloadBytes, prepared->pixels.size());
        if (strict_)
            throw std::runtime_error(
                "Cooked texture payload size mismatch: " +
                blobPath.string());
        return {};
    }

    if (stats_) {
        ++stats_->derivedTextureHits;
        std::error_code sizeError;
        const uint64_t blobSize = std::filesystem::file_size(blobPath, sizeError);
        if (!sizeError)
            stats_->derivedTextureReadBytes += blobSize;
        if (nativeBc7) {
            ++stats_->nativeBc7CacheHits;
            if (!sizeError)
                stats_->nativeTextureReadBytes += blobSize;
        } else {
            ++stats_->basisUastcCacheHits;
        }
        if (nativeBc7 || target_ == TextureTranscodeTarget::Bc7)
            ++stats_->bc7TextureCount;
        else
            ++stats_->rgbaTranscodeFallbackCount;
    }
    VKR_LOG_DEBUG("TextureCache", "cache={} upload={} image={} mips={}",
                  nativeBc7 ? "native-bc7" : "uastc",
                  nativeBc7 ? "direct" : "transcoded", imageIndex,
                  prepared->mipLevels.size());
    return prepared;
}

} // namespace vkr
