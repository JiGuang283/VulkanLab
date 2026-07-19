#include "assets/DerivedAssetPaths.h"
#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

void requireCatalog(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

class CatalogFixture {
  public:
    CatalogFixture() {
        const auto suffix = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        root = std::filesystem::temp_directory_path() /
               ("vulkan_lab_catalog_tests_" + std::to_string(suffix));
        std::filesystem::create_directories(root / "assets");
        std::filesystem::create_directories(root / "models");
        std::ofstream(root / "models/scene.glb", std::ios::binary) << "glTF";
    }

    ~CatalogFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    void write(const std::string &json) const {
        std::ofstream(root / "assets/catalog.json", std::ios::binary) << json;
    }

    std::filesystem::path root;
};

const char *validCatalog() {
    return R"({
      "schemaVersion": 1,
      "projectId": "catalog-test",
      "defaultImportProfile": "desktop_1024",
      "importProfiles": {
        "desktop_1024": {"textureLimit": 1024}
      },
      "scenes": [
        {"id":"scene","displayName":"Scene","source":"models/scene.glb"}
      ]
    })";
}

void testCatalogLoadAndProjectResolution() {
    CatalogFixture fixture;
    fixture.write(validCatalog());
    const vkr::ProjectContext context =
        vkr::ProjectContextResolver::resolve(fixture.root);
    const vkr::SceneCatalog catalog =
        vkr::SceneCatalog::load(context.catalogPath, context.projectRoot);
    requireCatalog(catalog.projectId == "catalog-test",
                   "catalog project ID changed");
    requireCatalog(catalog.scenes.size() == 1 &&
                       catalog.scenes[0].id == "scene",
                   "catalog scene was not loaded");
    requireCatalog(catalog.profile("desktop_1024").textureLimit == 1024,
                   "catalog profile was not loaded");
    requireCatalog(
        vkr::DerivedAssetPaths::defaultCacheRoot(catalog.projectId)
                .filename() == "catalog-test",
        "shared cache root omitted project ID");
}

void testCatalogRejectsEscapingSource() {
    CatalogFixture fixture;
    fixture.write(R"({
      "schemaVersion":1,
      "projectId":"catalog-test",
      "defaultImportProfile":"desktop_1024",
      "importProfiles":{"desktop_1024":{"textureLimit":1024}},
      "scenes":[{"id":"scene","displayName":"Scene","source":"../outside.glb","optional":true}]
    })");
    bool rejected = false;
    try {
        (void)vkr::SceneCatalog::load(fixture.root / "assets/catalog.json",
                                     fixture.root);
    } catch (const std::exception &error) {
        rejected = std::string(error.what()).find("escapes") !=
                   std::string::npos;
    }
    requireCatalog(rejected, "catalog accepted a source path escape");
}

void testCatalogRejectsDuplicateDisplayName() {
    CatalogFixture fixture;
    fixture.write(R"({
      "schemaVersion":1,
      "projectId":"catalog-test",
      "defaultImportProfile":"desktop_1024",
      "importProfiles":{"desktop_1024":{"textureLimit":1024}},
      "scenes":[
        {"id":"one","displayName":"Same","source":"models/scene.glb"},
        {"id":"two","displayName":"same","source":"models/scene.glb"}
      ]
    })");
    bool rejected = false;
    try {
        (void)vkr::SceneCatalog::load(fixture.root / "assets/catalog.json",
                                     fixture.root);
    } catch (const std::exception &error) {
        rejected = std::string(error.what()).find("duplicate display name") !=
                   std::string::npos;
    }
    requireCatalog(rejected,
                   "catalog accepted duplicate case-insensitive names");
}

} // namespace

void runSceneCatalogTests() {
    testCatalogLoadAndProjectResolution();
    testCatalogRejectsEscapingSource();
    testCatalogRejectsDuplicateDisplayName();
}
