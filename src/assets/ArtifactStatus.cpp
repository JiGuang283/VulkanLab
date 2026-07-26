#include "ArtifactStatus.h"

#include "DerivedEnvironmentManifest.h"
#include "DerivedTextureManifest.h"
#include "SceneCatalog.h"

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

bool hasKtx2Identifier(const std::filesystem::path &path) {
    static constexpr std::array<uint8_t, 12> identifier{
        0xab, 0x4b, 0x54, 0x58, 0x20, 0x32,
        0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a};
    std::ifstream input(path, std::ios::binary);
    std::array<uint8_t, identifier.size()> actual{};
    return input.read(reinterpret_cast<char *>(actual.data()), actual.size()) &&
           actual == identifier;
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
    if (manifest.schemaVersion != DerivedTextureManifest::kSchemaVersion ||
        manifest.projectId != request.projectId ||
        manifest.sceneId != request.sceneId ||
        manifest.profileId != request.profileId ||
        manifest.textureLimit != request.textureLimit) {
        return makeStatus(ArtifactState::Invalid,
                          "manifest identity or profile mismatch",
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
        if (!hasKtx2Identifier(blob)) {
            return makeStatus(
                ArtifactState::Invalid,
                "derived blob has an invalid KTX2 header: " + entry.blob,
                manifestPath);
        }
        std::error_code sizeError;
        const uint64_t bytes = std::filesystem::file_size(blob, sizeError);
        if (sizeError) {
            return makeStatus(ArtifactState::Invalid,
                              "could not read derived blob size: " +
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
        if (!hasKtx2Identifier(blob)) {
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
