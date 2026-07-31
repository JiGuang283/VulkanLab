#include "ArtifactStatus.h"

#include "DerivedEnvironmentManifest.h"
#include "DerivedTextureManifest.h"
#include "SceneCatalog.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <system_error>

namespace vkr {
namespace {

bool stampMatches(const DerivedFileStamp &expected,
                  const std::filesystem::path &path) {
    if (!std::filesystem::is_regular_file(path))
        return false;
    const DerivedFileStamp current = fileStamp(path);
    return current.size == expected.size &&
           current.writeTime == expected.writeTime;
}

struct Ktx2HeaderSummary {
    uint32_t vkFormat = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levelCount = 0;
    uint32_t supercompression = 0;
};

uint32_t readU32(const std::array<uint8_t, 48> &bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8u) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16u) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24u);
}

std::optional<Ktx2HeaderSummary>
readKtx2Header(const std::filesystem::path &path) {
    static constexpr std::array<uint8_t, 12> identifier{
        0xab, 0x4b, 0x54, 0x58, 0x20, 0x32,
        0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a};
    std::ifstream input(path, std::ios::binary);
    std::array<uint8_t, 48> header{};
    if (!input.read(reinterpret_cast<char *>(header.data()), header.size()) ||
        !std::equal(identifier.begin(), identifier.end(), header.begin())) {
        return std::nullopt;
    }
    return Ktx2HeaderSummary{readU32(header, 12), readU32(header, 20),
                             readU32(header, 24), readU32(header, 40),
                             readU32(header, 44)};
}

ArtifactStatus makeStatus(ArtifactState state, std::string reason,
                          const std::filesystem::path &manifestPath) {
    ArtifactStatus result;
    result.state = state;
    result.reason = std::move(reason);
    result.manifestPath = manifestPath;
    return result;
}

} // namespace

const char *artifactStateName(ArtifactState state) {
    switch (state) {
    case ArtifactState::Ready:
        return "Ready";
    case ArtifactState::Missing:
        return "Missing";
    case ArtifactState::Stale:
        return "Stale";
    case ArtifactState::Invalid:
        return "Invalid";
    case ArtifactState::Importing:
        return "Importing";
    }
    return "Unknown";
}

ArtifactStatus inspectTextureArtifacts(const ArtifactStatusRequest &request) {
    const std::filesystem::path manifestPath = derivedManifestPath(
        request.cacheRoot, request.sceneId, request.profileId);
    DerivedTextureManifest manifest;
    std::string error;
    if (!loadDerivedTextureManifest(manifestPath, manifest, error)) {
        return makeStatus(
            error == "manifest not found" ? ArtifactState::Missing
                                          : ArtifactState::Invalid,
            error, manifestPath);
    }
    if (manifest.schemaVersion <
            DerivedTextureManifest::kUastcSchemaVersion ||
        manifest.projectId != request.projectId ||
        manifest.sceneId != request.sceneId ||
        manifest.profileId != request.profileId ||
        manifest.textureLimit != request.textureLimit) {
        return makeStatus(ArtifactState::Invalid,
                          "manifest identity or profile mismatch",
                          manifestPath);
    }
    if (manifest.textureEncoder != request.textureEncoder) {
        return makeStatus(
            ArtifactState::Stale,
            std::string("texture encoder changed: expected ") +
                textureEncoderName(request.textureEncoder) + ", found " +
                textureEncoderName(manifest.textureEncoder),
            manifestPath);
    }
    if (!stampMatches(manifest.scene, request.scenePath)) {
        return makeStatus(ArtifactState::Stale, "scene source changed",
                          manifestPath);
    }

    const std::filesystem::path sceneDirectory =
        request.scenePath.parent_path();
    ArtifactStatus result =
        makeStatus(ArtifactState::Ready, "ready", manifestPath);
    result.textureEncoder = textureEncoderName(manifest.textureEncoder);
    result.payloadKind =
        manifest.textureEncoder == TextureEncoder::Bc7 ? "native-bc7"
                                                       : "basis-uastc";
    result.entryCount = manifest.entries.size();
    for (const DerivedTextureEntry &entry : manifest.entries) {
        if (entry.source.path.empty()) {
            return makeStatus(ArtifactState::Invalid,
                              "manifest contains an empty source path",
                              manifestPath);
        }
        std::filesystem::path source(entry.source.path);
        if (!source.is_absolute())
            source = sceneDirectory / source;
        if (!stampMatches(entry.source, source)) {
            return makeStatus(ArtifactState::Stale,
                              "texture source changed: " + entry.source.path,
                              manifestPath);
        }

        if (entry.blob.empty()) {
            return makeStatus(ArtifactState::Invalid,
                              "manifest contains an empty blob path",
                              manifestPath);
        }
        const std::filesystem::path blob =
            (request.cacheRoot / entry.blob).lexically_normal();
        if (!pathIsWithin(request.cacheRoot, blob) ||
            !std::filesystem::is_regular_file(blob)) {
            return makeStatus(ArtifactState::Invalid,
                              "derived blob is missing: " + entry.blob,
                              manifestPath);
        }
        const auto header = readKtx2Header(blob);
        if (!header) {
            return makeStatus(
                ArtifactState::Invalid,
                "derived blob has an invalid KTX2 header: " + entry.blob,
                manifestPath);
        }
        if (manifest.schemaVersion == DerivedTextureManifest::kSchemaVersion) {
            const DerivedTexturePayloadKind expectedPayload =
                request.textureEncoder == TextureEncoder::Bc7
                    ? DerivedTexturePayloadKind::NativeBc7
                    : DerivedTexturePayloadKind::BasisUastc;
            if (entry.payloadKind != expectedPayload ||
                entry.mipLevels == 0 || entry.width != header->width ||
                entry.height != header->height ||
                entry.mipLevels != header->levelCount ||
                entry.vkFormat != header->vkFormat ||
                (expectedPayload == DerivedTexturePayloadKind::NativeBc7 &&
                 (entry.vkFormat == 0 || header->supercompression != 0 ||
                  entry.supercompression != "none"))) {
                return makeStatus(
                    ArtifactState::Invalid,
                    "derived blob metadata does not match manifest: " +
                        entry.blob,
                    manifestPath);
            }
        }
        std::error_code sizeError;
        const uint64_t bytes = std::filesystem::file_size(blob, sizeError);
        if (sizeError) {
            return makeStatus(ArtifactState::Invalid,
                              "could not read derived blob size: " +
                                  entry.blob,
                              manifestPath);
        }
        if (manifest.schemaVersion == DerivedTextureManifest::kSchemaVersion &&
            (entry.blobBytes != bytes || entry.payloadBytes == 0)) {
            return makeStatus(ArtifactState::Invalid,
                              "derived blob size metadata mismatch: " +
                                  entry.blob,
                              manifestPath);
        }
        result.blobBytes += bytes;
    }
    return result;
}

ArtifactStatus inspectEnvironmentArtifacts(
    const EnvironmentArtifactStatusRequest &request) {
    const std::filesystem::path manifestPath =
        derivedEnvironmentManifestPath(request.cacheRoot,
                                       request.environmentId,
                                       request.profileId);
    DerivedEnvironmentManifest manifest;
    std::string error;
    if (!loadDerivedEnvironmentManifest(manifestPath, manifest, error)) {
        return makeStatus(
            error == "manifest not found" ? ArtifactState::Missing
                                          : ArtifactState::Invalid,
            error, manifestPath);
    }
    if (manifest.projectId != request.projectId ||
        manifest.environmentId != request.environmentId ||
        manifest.profileId != request.profileId) {
        return makeStatus(ArtifactState::Invalid,
                          "environment manifest identity mismatch",
                          manifestPath);
    }
    if (!manifest.source.path.empty() &&
        !stampMatches(manifest.source, request.sourcePath)) {
        return makeStatus(ArtifactState::Stale,
                          "environment source changed", manifestPath);
    }

    ArtifactStatus result =
        makeStatus(ArtifactState::Ready, "ready", manifestPath);
    result.entryCount = manifest.images.size();
    for (const DerivedEnvironmentImage &image : manifest.images) {
        const std::filesystem::path blob =
            (request.cacheRoot / image.blob).lexically_normal();
        if (!pathIsWithin(request.cacheRoot, blob) ||
            !std::filesystem::is_regular_file(blob)) {
            return makeStatus(ArtifactState::Invalid,
                              "derived environment blob is missing: " +
                                  image.blob,
                              manifestPath);
        }
        if (!readKtx2Header(blob)) {
            return makeStatus(ArtifactState::Invalid,
                              "derived environment blob has an invalid KTX2 "
                              "header: " +
                                  image.blob,
                              manifestPath);
        }
        std::error_code sizeError;
        const uint64_t bytes = std::filesystem::file_size(blob, sizeError);
        if (sizeError || (image.bytes != 0 && image.bytes != bytes)) {
            return makeStatus(ArtifactState::Invalid,
                              "derived environment blob size mismatch: " +
                                  image.blob,
                              manifestPath);
        }
        result.blobBytes += bytes;
    }
    return result;
}

} // namespace vkr
