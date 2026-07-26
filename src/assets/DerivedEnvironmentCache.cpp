#include "DerivedEnvironmentCache.h"

#include "DerivedEnvironmentManifest.h"
#include "SceneCatalog.h"

#include <ktx.h>

#include <algorithm>
#include <memory>
#include <stdexcept>

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

VkFormat expectedFormat(const DerivedEnvironmentImage &image) {
    if (image.format == "rgba16f")
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    if (image.format == "rg16f")
        return VK_FORMAT_R16G16_SFLOAT;
    return VK_FORMAT_UNDEFINED;
}

uint32_t bytesPerPixel(VkFormat format) {
    if (format == VK_FORMAT_R16G16B16A16_SFLOAT)
        return 8;
    if (format == VK_FORMAT_R16G16_SFLOAT)
        return 4;
    return 0;
}

} // namespace

DerivedEnvironmentCache::DerivedEnvironmentCache(
    std::filesystem::path cacheRoot, std::filesystem::path sourcePath,
    std::string projectId, std::string environmentId,
    std::string displayName, std::string profileId, bool validateSource)
    : cacheRoot_(std::move(cacheRoot)),
      sourcePath_(std::move(sourcePath)),
      manifestPath_(derivedEnvironmentManifestPath(
          cacheRoot_, environmentId, profileId)),
      projectId_(std::move(projectId)),
      environmentId_(std::move(environmentId)),
      displayName_(std::move(displayName)),
      profileId_(std::move(profileId)),
      validateSource_(validateSource) {}

PreparedEnvironmentData DerivedEnvironmentCache::load() const {
    DerivedEnvironmentManifest manifest;
    std::string error;
    if (!loadDerivedEnvironmentManifest(manifestPath_, manifest, error)) {
        throw std::runtime_error("Could not load environment manifest '" +
                                 manifestPath_.string() + "': " + error);
    }
    if (manifest.projectId != projectId_ ||
        manifest.environmentId != environmentId_ ||
        manifest.profileId != profileId_) {
        throw std::runtime_error(
            "Environment manifest identity does not match the request");
    }
    if (validateSource_) {
        const DerivedFileStamp current = fileStamp(sourcePath_);
        if (current.size != manifest.source.size ||
            current.writeTime != manifest.source.writeTime) {
            throw std::runtime_error(
                "Environment source changed after cache generation");
        }
    }

    PreparedEnvironmentData result;
    result.environmentId = environmentId_;
    result.displayName = displayName_;
    result.profileId = profileId_;
    result.radiance =
        loadImage(manifest, EnvironmentMapKind::Radiance);
    result.irradiance =
        loadImage(manifest, EnvironmentMapKind::Irradiance);
    result.prefilteredSpecular =
        loadImage(manifest, EnvironmentMapKind::PrefilteredSpecular);
    result.brdfLut = loadImage(manifest, EnvironmentMapKind::BrdfLut);
    return result;
}

PreparedEnvironmentImage DerivedEnvironmentCache::loadImage(
    const DerivedEnvironmentManifest &manifest,
    EnvironmentMapKind kind) const {
    const DerivedEnvironmentImage *entry = manifest.find(kind);
    if (!entry)
        throw std::runtime_error("Environment manifest image is missing");
    const std::filesystem::path path =
        (cacheRoot_ / entry->blob).lexically_normal();
    if (!pathIsWithin(cacheRoot_, path)) {
        throw std::runtime_error(
            "Environment KTX2 path escapes the cache root");
    }
    KtxOwner owner;
    const KTX_error_code result = ktxTexture2_CreateFromNamedFile(
        path.string().c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
        owner.put());
    if (result != KTX_SUCCESS || !owner.get()) {
        throw std::runtime_error("Could not read environment KTX2 '" +
                                 path.string() + "': " +
                                 ktxErrorString(result));
    }

    ktxTexture2 *texture2 = owner.get();
    ktxTexture *texture = ktxTexture(texture2);
    const VkFormat format = expectedFormat(*entry);
    const uint32_t expectedFaces =
        kind == EnvironmentMapKind::BrdfLut ? 1u : 6u;
    if (format == VK_FORMAT_UNDEFINED ||
        texture2->vkFormat != static_cast<uint32_t>(format) ||
        texture->numDimensions != 2 || texture->numLayers != 1 ||
        texture->numFaces != expectedFaces ||
        texture->numLevels != entry->mipLevels ||
        texture->baseWidth != entry->width ||
        texture->baseHeight != entry->height ||
        ktxTexture2_NeedsTranscoding(texture2)) {
        throw std::runtime_error("Invalid native environment KTX2: " +
                                 path.string());
    }

    PreparedEnvironmentImage prepared;
    prepared.kind = kind;
    prepared.width = texture->baseWidth;
    prepared.height = texture->baseHeight;
    prepared.mipLevels = texture->numLevels;
    prepared.arrayLayers = texture->numFaces;
    prepared.format = format;
    const uint32_t pixelBytes = bytesPerPixel(format);
    const uint8_t *data = ktxTexture_GetData(texture);
    for (uint32_t level = 0; level < texture->numLevels; ++level) {
        const uint32_t width = std::max(1u, texture->baseWidth >> level);
        const uint32_t height = std::max(1u, texture->baseHeight >> level);
        const uint64_t imageBytes =
            static_cast<uint64_t>(width) * height * pixelBytes;
        for (uint32_t face = 0; face < texture->numFaces; ++face) {
            ktx_size_t sourceOffset = 0;
            if (ktxTexture_GetImageOffset(texture, level, 0, face,
                                          &sourceOffset) != KTX_SUCCESS) {
                throw std::runtime_error(
                    "Invalid environment KTX2 image offset");
            }
            const uint64_t destinationOffset = prepared.bytes.size();
            prepared.bytes.insert(prepared.bytes.end(),
                                  data + sourceOffset,
                                  data + sourceOffset + imageBytes);
            prepared.subresources.push_back(
                {destinationOffset, imageBytes, width, height, level, face});
        }
    }
    return prepared;
}

} // namespace vkr
