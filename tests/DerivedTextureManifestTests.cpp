#include "assets/DerivedTextureManifest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

void requireManifest(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        path_ = std::filesystem::temp_directory_path() /
                ("vulkan_lab_manifest_tests_" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path &path() const { return path_; }

  private:
    std::filesystem::path path_;
};

void testManifestPathsAndLookup() {
    const std::filesystem::path scene =
        "models/main_sponza/NewSponza_Main_glTF_003.gltf";
    requireManifest(
        vkr::normalizedSceneKey(scene) == vkr::normalizedSceneKey(
                                             "MODELS/MAIN_SPONZA/"
                                             "NEWSPONZA_MAIN_GLTF_003.GLTF"),
        "normalized scene keys should be ASCII case insensitive");

    const auto limited =
        vkr::derivedManifestPath("derived_assets", scene, 1024);
    const auto full = vkr::derivedManifestPath("derived_assets", scene, 0);
    const auto stable = vkr::derivedManifestPath(
        "derived_assets", "main-sponza", "desktop_1024");
    requireManifest(limited.filename() == "1024.json",
                    "limited profile manifest name is wrong");
    requireManifest(full.filename() == "full.json",
                    "full profile manifest name is wrong");
    requireManifest(limited.parent_path().parent_path().filename() ==
                        "manifests",
                    "manifest path is outside the manifests directory");
    requireManifest(stable == std::filesystem::path("derived_assets") /
                                  "manifests/main-sponza/desktop_1024.json",
                    "stable manifest path is wrong");

    vkr::DerivedTextureManifest manifest;
    manifest.entries.push_back({7,
                                vkr::TextureSemantic::SrgbColor,
                                vkr::DerivedMipmapWrap::Repeat,
                                1024,
                                512,
                                "srgb-key",
                                "blobs/srgb-key.ktx2",
                                {}});
    manifest.entries.push_back({7,
                                vkr::TextureSemantic::Normal,
                                vkr::DerivedMipmapWrap::Repeat,
                                1024,
                                512,
                                "normal-key",
                                "blobs/normal-key.ktx2",
                                {}});

    const auto *normal = manifest.find(7, vkr::TextureSemantic::Normal,
                                       vkr::DerivedMipmapWrap::Repeat);
    requireManifest(normal != nullptr && normal->cacheKey == "normal-key",
                    "manifest lookup reused a different texture semantic");
    requireManifest(manifest.find(7, vkr::TextureSemantic::SrgbColor,
                                  vkr::DerivedMipmapWrap::Clamp) == nullptr,
                    "manifest lookup ignored mip wrap");
}

void testManifestRoundTrip() {
    TemporaryDirectory temporary;
    const auto manifestPath = temporary.path() / "manifests" / "scene" /
                              "2048.json";

    vkr::DerivedTextureManifest source;
    source.projectId = "vulkan-lab";
    source.sceneId = "test-scene";
    source.profileId = "desktop_2048";
    source.scenePath = "models/test/scene.gltf";
    source.qualityPreset = "production";
    source.encoderSettings = "encoder-settings-v1";
    source.textureLimit = 2048;
    source.scene = {"models/test/scene.gltf", 1234, 5678, "scene-sha256"};
    source.entries.push_back({3,
                              vkr::TextureSemantic::LinearData,
                              vkr::DerivedMipmapWrap::Reflect,
                              2048,
                              1024,
                              "linear-key",
                              "blobs/linear-key.ktx2",
                              {"textures/data.png", 42, 99, "image-sha256"}});

    std::string error;
    requireManifest(vkr::saveDerivedTextureManifest(manifestPath, source,
                                                    error),
                    "manifest save failed");

    vkr::DerivedTextureManifest loaded;
    requireManifest(vkr::loadDerivedTextureManifest(manifestPath, loaded,
                                                    error),
                    "manifest load failed");
    requireManifest(loaded.schemaVersion ==
                        vkr::DerivedTextureManifest::kSchemaVersion,
                    "manifest schema version changed during round trip");
    requireManifest(loaded.scenePath == source.scenePath &&
                        loaded.textureLimit == source.textureLimit,
                    "manifest profile changed during round trip");
    requireManifest(loaded.qualityPreset == source.qualityPreset &&
                        loaded.encoderSettings == source.encoderSettings,
                    "manifest encoder settings changed during round trip");
    requireManifest(loaded.entries.size() == 1,
                    "manifest entries changed during round trip");
    const auto &entry = loaded.entries.front();
    requireManifest(entry.semantic == vkr::TextureSemantic::LinearData &&
                        entry.mipWrap == vkr::DerivedMipmapWrap::Reflect &&
                        entry.width == 2048 && entry.height == 1024,
                    "manifest texture metadata changed during round trip");
    requireManifest(entry.source.sha256 == "image-sha256" &&
                        entry.blob == "blobs/linear-key.ktx2",
                    "manifest source or blob changed during round trip");

    source.encoderSettings = "replacement-settings";
    requireManifest(vkr::saveDerivedTextureManifest(manifestPath, source,
                                                    error),
                    "manifest atomic replacement failed");
    requireManifest(vkr::loadDerivedTextureManifest(manifestPath, loaded,
                                                    error) &&
                        loaded.encoderSettings == "replacement-settings",
                    "manifest replacement did not publish the new version");
    for (const auto &entryPath :
         std::filesystem::directory_iterator(manifestPath.parent_path())) {
        requireManifest(entryPath.path().extension() == ".json",
                        "manifest publication left a temporary file");
    }
}

void testFileStampInvalidation() {
    TemporaryDirectory temporary;
    const auto sourcePath = temporary.path() / "texture.bin";
    {
        std::ofstream output(sourcePath, std::ios::binary);
        output << "initial";
    }

    const auto stamp = vkr::fileStamp(sourcePath, "content-hash");
    requireManifest(vkr::fileStampMatches(stamp, temporary.path()),
                    "fresh file stamp should match");

    {
        std::ofstream output(sourcePath, std::ios::binary | std::ios::app);
        output << "-changed";
    }
    requireManifest(!vkr::fileStampMatches(stamp, temporary.path()),
                    "changed source file did not invalidate its stamp");
}

void testUnsupportedSchemaIsRejected() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "unsupported.json";
    {
        std::ofstream output(path, std::ios::binary);
        output << R"({"schemaVersion":99,"scenePath":"scene.gltf","textureLimit":1024,"scene":{"path":"scene.gltf","size":1,"writeTime":1,"sha256":"hash"},"entries":[]})";
    }

    vkr::DerivedTextureManifest manifest;
    std::string error;
    requireManifest(!vkr::loadDerivedTextureManifest(path, manifest, error),
                    "unsupported manifest schema was accepted");
    requireManifest(error == "unsupported manifest schema",
                    "unsupported schema returned an unexpected error");
}

void testLegacySchemaCanBeReadForMigration() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "legacy.json";
    {
        std::ofstream output(path, std::ios::binary);
        output << R"({"schemaVersion":1,"scenePath":"models/scene.gltf","textureLimit":1024,"scene":{"path":"scene.gltf","size":1,"writeTime":1,"sha256":"hash"},"entries":[]})";
    }
    vkr::DerivedTextureManifest manifest;
    std::string error;
    requireManifest(vkr::loadDerivedTextureManifest(path, manifest, error),
                    "legacy manifest could not be read for migration");
    requireManifest(manifest.schemaVersion ==
                        vkr::DerivedTextureManifest::kLegacySchemaVersion,
                    "legacy manifest schema was not preserved");
}

} // namespace

void runDerivedTextureManifestTests() {
    testManifestPathsAndLookup();
    testManifestRoundTrip();
    testFileStampInvalidation();
    testUnsupportedSchemaIsRejected();
    testLegacySchemaCanBeReadForMigration();
}
