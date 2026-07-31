#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vkr {

enum class TextureSemantic { SrgbColor, LinearData, Normal };
enum class DerivedMipmapWrap { Clamp, Repeat, Reflect };
enum class TextureEncoder { Uastc, Bc7 };
enum class DerivedTexturePayloadKind { BasisUastc, NativeBc7 };

const char *textureSemanticName(TextureSemantic semantic);
const char *derivedMipmapWrapName(DerivedMipmapWrap wrap);
const char *textureEncoderName(TextureEncoder encoder);
const char *derivedTexturePayloadKindName(DerivedTexturePayloadKind kind);
std::optional<TextureSemantic> textureSemanticFromName(const std::string &name);
std::optional<DerivedMipmapWrap>
derivedMipmapWrapFromName(const std::string &name);
std::optional<TextureEncoder> textureEncoderFromName(const std::string &name);
std::optional<DerivedTexturePayloadKind>
derivedTexturePayloadKindFromName(const std::string &name);

struct DerivedFileStamp {
    std::string path;
    uint64_t size = 0;
    int64_t writeTime = 0;
    std::string sha256;
};

struct DerivedTextureEntry {
    int32_t imageIndex = -1;
    TextureSemantic semantic = TextureSemantic::LinearData;
    DerivedMipmapWrap mipWrap = DerivedMipmapWrap::Clamp;
    uint32_t width = 0;
    uint32_t height = 0;
    DerivedTexturePayloadKind payloadKind =
        DerivedTexturePayloadKind::BasisUastc;
    uint32_t vkFormat = 0;
    uint32_t mipLevels = 0;
    uint64_t payloadBytes = 0;
    uint64_t blobBytes = 0;
    std::string supercompression;
    std::string cacheKey;
    std::string blob;
    DerivedFileStamp source;
};

struct DerivedTextureManifest {
    static constexpr uint32_t kLegacySchemaVersion = 1;
    static constexpr uint32_t kUastcSchemaVersion = 2;
    static constexpr uint32_t kSchemaVersion = 3;

    uint32_t schemaVersion = kSchemaVersion;
    std::string projectId;
    std::string sceneId;
    std::string profileId;
    std::string scenePath;
    std::string qualityPreset = "development";
    TextureEncoder textureEncoder = TextureEncoder::Uastc;
    std::string encoderName = "ktx create";
    std::string encoderVersion = "4.4.2";
    std::string encoderSettings;
    uint32_t textureLimit = 0;
    DerivedFileStamp scene;
    std::vector<DerivedTextureEntry> entries;

    const DerivedTextureEntry *find(int imageIndex, TextureSemantic semantic,
                                    DerivedMipmapWrap wrap) const;
};

std::string normalizedSceneKey(const std::filesystem::path &scenePath);
std::filesystem::path derivedManifestPath(
    const std::filesystem::path &cacheRoot,
    const std::filesystem::path &scenePath, uint32_t textureLimit);
std::filesystem::path derivedManifestPath(
    const std::filesystem::path &cacheRoot, const std::string &sceneId,
    const std::string &profileId);
DerivedFileStamp fileStamp(const std::filesystem::path &path,
                          const std::string &sha256 = {});
bool fileStampMatches(const DerivedFileStamp &stamp,
                      const std::filesystem::path &sceneDirectory);

bool loadDerivedTextureManifest(const std::filesystem::path &path,
                                DerivedTextureManifest &manifest,
                                std::string &error);
bool saveDerivedTextureManifest(const std::filesystem::path &path,
                                const DerivedTextureManifest &manifest,
                                std::string &error);

} // namespace vkr
