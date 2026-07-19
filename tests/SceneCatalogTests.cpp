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

class ScopedCurrentPath {
  public:
    explicit ScopedCurrentPath(const std::filesystem::path &path)
        : previous_(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~ScopedCurrentPath() {
        std::error_code ignored;
        std::filesystem::current_path(previous_, ignored);
    }

  private:
    std::filesystem::path previous_;
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
        {
          "id":"scene",
          "displayName":"Scene",
          "source":"models/scene.glb",
          "camera":{"position":[1.0,2.0,3.0],"yaw":45.0,"pitch":-10.0}
        }
      ]
    })";
}

void testCatalogLoadAndProjectResolution() {
    CatalogFixture fixture;
    fixture.write(validCatalog());
    const std::filesystem::path runtime = fixture.root / "runtime";
    std::filesystem::create_directories(runtime);
    const vkr::ProjectContext context =
        vkr::ProjectContextResolver::resolve(
            fixture.root, runtime / "VulkanLab.exe");
    const std::filesystem::path canonicalProject =
        std::filesystem::weakly_canonical(fixture.root);
    const std::filesystem::path canonicalRuntime =
        std::filesystem::weakly_canonical(runtime);
    requireCatalog(context.projectRoot == canonicalProject &&
                       context.runtimeRoot == canonicalRuntime &&
                       context.captureRoot ==
                           canonicalRuntime / "artifacts/captures",
                   "explicit ProjectContext roots were not resolved");
    requireCatalog(
        context.resolveProjectPath("models/scene.glb") ==
                canonicalProject / "models/scene.glb" &&
            context.resolveRuntimePath("shader/test.spv") ==
                canonicalRuntime / "shader/test.spv",
        "ProjectContext relative path resolution used the wrong root");
    const vkr::SceneCatalog catalog =
        vkr::SceneCatalog::load(context.catalogPath, context.projectRoot);
    requireCatalog(catalog.projectId == "catalog-test",
                   "catalog project ID changed");
    requireCatalog(catalog.scenes.size() == 1 &&
                       catalog.scenes[0].id == "scene",
                   "catalog scene was not loaded");
    requireCatalog(catalog.profile("desktop_1024").textureLimit == 1024,
                   "catalog profile was not loaded");
    requireCatalog(catalog.scenes[0].camera.has_value() &&
                       catalog.scenes[0].camera->position ==
                           glm::vec3(1.0f, 2.0f, 3.0f) &&
                       catalog.scenes[0].camera->yaw == 45.0f &&
                       catalog.scenes[0].camera->pitch == -10.0f,
                   "catalog camera pose did not round trip");
    requireCatalog(
        vkr::DerivedAssetPaths::defaultCacheRoot(catalog.projectId)
                .filename() == "catalog-test",
        "shared cache root omitted project ID");
}

void testDeveloperLocatorAndAncestorDiscovery() {
    CatalogFixture fixture;
    fixture.write(validCatalog());
    const std::filesystem::path runtime = fixture.root / "runtime";
    const std::filesystem::path working = fixture.root / "nested/working";
    std::filesystem::create_directories(runtime);
    std::filesystem::create_directories(working);
    {
        std::ofstream locator(runtime / "vulkanlab_project.json",
                              std::ios::binary);
        locator << "{\"projectRoot\":\""
                << fixture.root.generic_string() << "\"}";
    }

    {
        ScopedCurrentPath currentPath(working);
        const vkr::ProjectContext context =
            vkr::ProjectContextResolver::resolve(
                std::nullopt, runtime / "VulkanLab.exe");
        requireCatalog(context.projectRoot ==
                           std::filesystem::weakly_canonical(fixture.root) &&
                           context.runtimeRoot ==
                               std::filesystem::weakly_canonical(runtime) &&
                           context.diagnostic.find("developer locator") !=
                               std::string::npos,
                       "developer locator did not win from another CWD");
    }

    std::filesystem::remove(runtime / "vulkanlab_project.json");
    {
        ScopedCurrentPath currentPath(working);
        const vkr::ProjectContext context =
            vkr::ProjectContextResolver::resolve(
                std::nullopt, runtime / "VulkanLab.exe");
        requireCatalog(
            context.projectRoot ==
                    std::filesystem::weakly_canonical(fixture.root) &&
                context.runtimeRoot ==
                    std::filesystem::weakly_canonical(runtime) &&
                context.diagnostic.find("working-directory ancestor") !=
                    std::string::npos,
            "ancestor Catalog discovery did not retain the runtime root");
    }
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
    testDeveloperLocatorAndAncestorDiscovery();
    testCatalogRejectsEscapingSource();
    testCatalogRejectsDuplicateDisplayName();
}
