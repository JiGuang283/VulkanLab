#include "assets/ArtifactStatus.h"
#include "assets/DerivedTextureManifest.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

void requireArtifact(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

class TemporaryArtifacts {
  public:
    TemporaryArtifacts() {
        path = std::filesystem::temp_directory_path() /
               ("vulkan_lab_artifact_status_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        std::filesystem::create_directories(path / "scene/textures");
        std::filesystem::create_directories(path / "cache/blobs");
    }
    ~TemporaryArtifacts() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

void writeBytes(const std::filesystem::path &path, const std::string &bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void writeKtxHeader(const std::filesystem::path &path) {
    static constexpr std::array<uint8_t, 12> identifier{
        0xab, 0x4b, 0x54, 0x58, 0x20, 0x32,
        0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a};
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(identifier.data()),
                 identifier.size());
}

void testArtifactStates() {
    TemporaryArtifacts temporary;
    const auto scene = temporary.path / "scene/test.gltf";
    const auto image = temporary.path / "scene/textures/base.png";
    const auto cache = temporary.path / "cache";
    const auto blob = cache / "blobs/key.ktx2";
    writeBytes(scene, "scene");
    writeBytes(image, "image");
    writeKtxHeader(blob);

    vkr::DerivedTextureManifest manifest;
    manifest.projectId = "project";
    manifest.sceneId = "scene";
    manifest.profileId = "profile";
    manifest.scenePath = "scene/test.gltf";
    manifest.textureLimit = 1024;
    manifest.scene = vkr::fileStamp(scene, "scene-hash");
    manifest.scene.path = "test.gltf";
    vkr::DerivedTextureEntry entry;
    entry.imageIndex = 0;
    entry.semantic = vkr::TextureSemantic::SrgbColor;
    entry.mipWrap = vkr::DerivedMipmapWrap::Repeat;
    entry.width = 1;
    entry.height = 1;
    entry.cacheKey = "key";
    entry.blob = "blobs/key.ktx2";
    entry.source = vkr::fileStamp(image, "image-hash");
    entry.source.path = "textures/base.png";
    manifest.entries.push_back(entry);
    const auto manifestPath =
        vkr::derivedManifestPath(cache, "scene", "profile");
    std::string error;
    requireArtifact(vkr::saveDerivedTextureManifest(manifestPath, manifest,
                                                    error),
                    "could not create status test manifest");

    const vkr::ArtifactStatusRequest request{cache, scene, "project", "scene",
                                             "profile", 1024};
    auto status = vkr::inspectTextureArtifacts(request);
    requireArtifact(status.state == vkr::ArtifactState::Ready &&
                        status.entryCount == 1 && status.blobBytes == 12,
                    "valid artifacts were not Ready");

    writeBytes(image, "image-changed");
    status = vkr::inspectTextureArtifacts(request);
    requireArtifact(status.state == vkr::ArtifactState::Stale,
                    "changed source was not Stale");

    writeBytes(image, "image");
    manifest.entries[0].source = vkr::fileStamp(image, "image-hash");
    manifest.entries[0].source.path = "textures/base.png";
    requireArtifact(vkr::saveDerivedTextureManifest(manifestPath, manifest,
                                                    error),
                    "could not refresh status test manifest");
    writeBytes(blob, "not-ktx2");
    status = vkr::inspectTextureArtifacts(request);
    requireArtifact(status.state == vkr::ArtifactState::Invalid,
                    "corrupt blob was not Invalid");

    std::filesystem::remove(manifestPath);
    status = vkr::inspectTextureArtifacts(request);
    requireArtifact(status.state == vkr::ArtifactState::Missing,
                    "missing manifest was not Missing");
}

} // namespace

void runArtifactStatusTests() { testArtifactStates(); }
