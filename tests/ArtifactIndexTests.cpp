#include "assets/ArtifactIndex.h"
#include "assets/ContentHash.h"
#include "assets/DerivedTextureManifest.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

void requireIndex(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

class TemporaryIndexProject {
  public:
    TemporaryIndexProject() {
        root = std::filesystem::temp_directory_path() /
               ("vulkan_lab_artifact_index_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        std::filesystem::create_directories(root / "models/textures");
        std::filesystem::create_directories(root / "cache/blobs");
    }
    ~TemporaryIndexProject() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
};

void writeBytes(const std::filesystem::path &path, const std::string &bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void writeKtx(const std::filesystem::path &path) {
    static constexpr std::array<uint8_t, 12> identifier{
        0xab, 0x4b, 0x54, 0x58, 0x20, 0x32,
        0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a};
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(identifier.data()),
                 identifier.size());
}

vkr::SceneCatalog makeCatalog() {
    vkr::SceneCatalog catalog;
    catalog.projectId = "index-test";
    catalog.defaultImportProfile = "desktop_512";
    catalog.importProfiles.emplace(
        "desktop_512",
        vkr::ImportProfile{"desktop_512", 512, "uastc", "development"});
    catalog.scenes.push_back(
        {"scene-a", "Scene A", "gltf", {}, "models/a.gltf",
         "desktop_512"});
    catalog.scenes.push_back(
        {"scene-b", "Scene B", "gltf", {}, "models/b.gltf",
         "desktop_512"});
    return catalog;
}

void publishArtifacts(const TemporaryIndexProject &temporary,
                      const std::string &sceneId,
                      const std::string &sceneFile,
                      const std::string &imageFile,
                      const std::string &blobName) {
    const auto scene = temporary.root / "models" / sceneFile;
    const auto image = temporary.root / "models/textures" / imageFile;
    const auto blob = temporary.root / "cache/blobs" / blobName;
    writeBytes(scene, "scene-" + sceneId);
    writeBytes(image, "image-" + sceneId);
    writeKtx(blob);

    vkr::DerivedTextureManifest manifest;
    manifest.projectId = "index-test";
    manifest.sceneId = sceneId;
    manifest.profileId = "desktop_512";
    manifest.scenePath = (std::filesystem::path("models") / sceneFile)
                             .generic_string();
    manifest.textureLimit = 512;
    manifest.scene = vkr::fileStamp(scene, vkr::sha256File(scene));
    manifest.scene.path = sceneFile;
    vkr::DerivedTextureEntry entry;
    entry.imageIndex = 0;
    entry.semantic = vkr::TextureSemantic::SrgbColor;
    entry.mipWrap = vkr::DerivedMipmapWrap::Repeat;
    entry.width = 1;
    entry.height = 1;
    entry.cacheKey = blobName;
    entry.blob = (std::filesystem::path("blobs") / blobName).generic_string();
    entry.source = vkr::fileStamp(image, vkr::sha256File(image));
    entry.source.path = (std::filesystem::path("textures") / imageFile)
                            .generic_string();
    manifest.entries.push_back(std::move(entry));
    std::string error;
    requireIndex(vkr::saveDerivedTextureManifest(
                     vkr::derivedManifestPath(temporary.root / "cache",
                                              sceneId, "desktop_512"),
                     manifest, error),
                 "could not publish ArtifactIndex test manifest");
}

vkr::ArtifactStatusRequest requestFor(const TemporaryIndexProject &temporary,
                                      const std::string &sceneId,
                                      const std::string &sceneFile) {
    return {temporary.root / "cache", temporary.root / "models" / sceneFile,
            "index-test", sceneId, "desktop_512", 512};
}

void testRebuildAndChangeDetection() {
    TemporaryIndexProject temporary;
    const vkr::SceneCatalog catalog = makeCatalog();
    publishArtifacts(temporary, "scene-a", "a.gltf", "a.png", "a.ktx2");
    publishArtifacts(temporary, "scene-b", "b.gltf", "b.png", "b.ktx2");

    bool rebuilt = false;
    vkr::ArtifactIndex index = vkr::ArtifactIndex::loadOrRebuild(
        temporary.root / "cache", temporary.root, catalog, &rebuilt);
    requireIndex(rebuilt && index.records().size() == 2,
                 "missing ArtifactIndex was not rebuilt");
    requireIndex(index.query(requestFor(temporary, "scene-a", "a.gltf"),
                             vkr::ArtifactValidationMode::Admission)
                     .ready(),
                 "rebuilt ArtifactIndex record was not Ready");

    const auto image = temporary.root / "models/textures/a.png";
    std::filesystem::last_write_time(
        image, std::filesystem::last_write_time(image) +
                   std::chrono::seconds(2));
    requireIndex(index.query(requestFor(temporary, "scene-a", "a.gltf"),
                             vkr::ArtifactValidationMode::Fast)
                     .ready(),
                 "mtime-only source change should be accepted by SHA-256");

    writeBytes(image, "changed-image-a");
    requireIndex(index.query(requestFor(temporary, "scene-a", "a.gltf"),
                             vkr::ArtifactValidationMode::Fast)
                         .state == vkr::ArtifactState::Stale,
                 "content change did not make referencing record Stale");
    requireIndex(index.query(requestFor(temporary, "scene-b", "b.gltf"),
                             vkr::ArtifactValidationMode::Fast)
                     .ready(),
                 "unrelated record was invalidated by source change");

    writeBytes(temporary.root / "cache/artifact_index.json", "{broken");
    index = vkr::ArtifactIndex::loadOrRebuild(
        temporary.root / "cache", temporary.root, catalog, &rebuilt);
    requireIndex(rebuilt && index.records().size() == 2,
                 "corrupt ArtifactIndex was not rebuilt");

    writeBytes(temporary.root / "cache/blobs/b.ktx2", "invalid");
    requireIndex(index.query(requestFor(temporary, "scene-b", "b.gltf"),
                             vkr::ArtifactValidationMode::Admission)
                         .state == vkr::ArtifactState::Invalid,
                 "admission did not reject corrupt KTX2 blob");
}

void testIncrementalSaveMergesWriters() {
    TemporaryIndexProject temporary;
    const vkr::SceneCatalog catalog = makeCatalog();
    publishArtifacts(temporary, "scene-a", "a.gltf", "a.png", "a.ktx2");
    publishArtifacts(temporary, "scene-b", "b.gltf", "b.png", "b.ktx2");
    vkr::ArtifactIndex first = vkr::ArtifactIndex::loadOrRebuild(
        temporary.root / "cache", temporary.root, catalog);
    vkr::ArtifactIndex second = vkr::ArtifactIndex::loadOrRebuild(
        temporary.root / "cache", temporary.root, catalog);
    first.recordFailure("scene-a", "desktop_512", "first", "one", {});
    first.save();
    second.recordFailure("scene-b", "desktop_512", "second", "two", {});
    second.save();

    bool rebuilt = false;
    const vkr::ArtifactIndex merged = vkr::ArtifactIndex::loadOrRebuild(
        temporary.root / "cache", temporary.root, catalog, &rebuilt);
    requireIndex(!rebuilt, "merged ArtifactIndex unexpectedly rebuilt");
    requireIndex(merged.records().at(vkr::artifactIndexKey(
                     "scene-a", "desktop_512"))
                         .failureCode == "first" &&
                     merged.records().at(vkr::artifactIndexKey(
                         "scene-b", "desktop_512"))
                             .failureCode == "second",
                 "incremental ArtifactIndex saves lost another writer");

    vkr::ArtifactIndex publisher = vkr::ArtifactIndex::loadOrRebuild(
        temporary.root / "cache", temporary.root, catalog);
    vkr::ArtifactIndex staleReader = vkr::ArtifactIndex::loadOrRebuild(
        temporary.root / "cache", temporary.root, catalog);
    publishArtifacts(temporary, "scene-a", "a.gltf", "a.png", "a.ktx2");
    const auto manifest = vkr::derivedManifestPath(
        temporary.root / "cache", "scene-a", "desktop_512");
    std::filesystem::last_write_time(
        manifest, std::filesystem::last_write_time(manifest) +
                      std::chrono::seconds(2));
    publisher.refresh(catalog, "scene-a", "desktop_512");
    publisher.save();
    staleReader.touch("scene-a", "desktop_512");
    staleReader.save();
    vkr::ArtifactIndex afterStaleWrite = vkr::ArtifactIndex::loadOrRebuild(
        temporary.root / "cache", temporary.root, catalog);
    requireIndex(
        afterStaleWrite
            .query(requestFor(temporary, "scene-a", "a.gltf"),
                   vkr::ArtifactValidationMode::Admission)
            .ready(),
        "stale telemetry writer replaced a newly published index record");
}

} // namespace

void runArtifactIndexTests() {
    requireIndex(
        vkr::sha256String("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "shared SHA-256 implementation returned the wrong digest");
    testRebuildAndChangeDetection();
    testIncrementalSaveMergesWriters();
}
