#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"
#include "assets/SceneImportService.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void requireImport(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

class ImportFixture {
  public:
    ImportFixture() {
        const auto suffix = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        root = std::filesystem::temp_directory_path() /
               ("vulkan_lab_import_tests_" + std::to_string(suffix));
        project = root / "project";
        source = root / "external";
        std::filesystem::create_directories(project / "assets");
        std::filesystem::create_directories(source / "textures/deep");
        std::ofstream(project / "assets/catalog.json", std::ios::binary)
            << R"({
              "schemaVersion":1,
              "projectId":"import-test",
              "defaultImportProfile":"desktop_1024",
              "importProfiles":{"desktop_1024":{"textureLimit":1024}},
              "scenes":[{"id":"builtin","displayName":"Builtin","type":"builtin","builtinFactory":"viking_room"}]
            })";
        std::ofstream(source / "mesh.bin", std::ios::binary) << "mesh";
        std::ofstream(source / "textures/deep/base.png", std::ios::binary)
            << "png";
        writeScene(R"({
          "asset":{"version":"2.0"},
          "buffers":[{"uri":"mesh.bin","byteLength":4}],
          "images":[{"uri":"textures/deep/base.png"}]
        })");
    }

    ~ImportFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    void writeScene(const std::string &json) const {
        std::ofstream(source / "scene.gltf", std::ios::binary) << json;
    }

    vkr::ProjectContext context() const {
        return vkr::ProjectContextResolver::resolve(project);
    }

    std::filesystem::path root;
    std::filesystem::path project;
    std::filesystem::path source;
};

void testCopyImportPublishesClosedDependencySet() {
    ImportFixture fixture;
    const auto checked =
        vkr::SceneImportService::preflight(fixture.source / "scene.gltf");
    requireImport(checked.dependencies.size() == 2,
                  "preflight did not collect the dependency closure");

    vkr::SceneImportRequest request;
    request.sourcePath = checked.sourcePath;
    request.displayName = "Imported Scene";
    request.sceneId = "imported-scene";
    request.profileId = "desktop_1024";
    const auto result = vkr::SceneImportService::importScene(
        fixture.context(), request);
    const auto destination = fixture.project / "models/imported/imported-scene";
    requireImport(result.scene.id == "imported-scene",
                  "import returned the wrong scene");
    requireImport(std::filesystem::is_regular_file(destination / "scene.gltf") &&
                      std::filesystem::is_regular_file(destination / "mesh.bin") &&
                      std::filesystem::is_regular_file(
                          destination / "textures/deep/base.png"),
                  "import did not publish an independent dependency closure");
    (void)vkr::SceneImportService::preflight(destination / "scene.gltf");
    const auto catalog = vkr::SceneCatalog::load(
        fixture.project / "assets/catalog.json", fixture.project);
    requireImport(catalog.findScene("imported-scene") != nullptr,
                  "import did not publish the Catalog entry");
}

void testUnsafeAndMissingUrisFailBeforePublication() {
    ImportFixture fixture;
    fixture.writeScene(R"({"asset":{"version":"2.0"},"images":[{"uri":"https://example.com/a.png"}]})");
    bool remoteRejected = false;
    try {
        (void)vkr::SceneImportService::preflight(fixture.source / "scene.gltf");
    } catch (const std::exception &error) {
        remoteRejected = std::string(error.what()).find("URI") !=
                         std::string::npos;
    }
    requireImport(remoteRejected, "remote URI was accepted");

    fixture.writeScene(R"({"asset":{"version":"2.0"},"buffers":[{"uri":"../outside.bin"}]})");
    bool escapeRejected = false;
    try {
        (void)vkr::SceneImportService::preflight(fixture.source / "scene.gltf");
    } catch (const std::exception &error) {
        escapeRejected = std::string(error.what()).find("escapes") !=
                         std::string::npos;
    }
    requireImport(escapeRejected, "dependency path escape was accepted");

    fixture.writeScene(R"({"asset":{"version":"2.0"},"buffers":[{"uri":"missing.bin"}]})");
    bool missingRejected = false;
    try {
        (void)vkr::SceneImportService::preflight(fixture.source / "scene.gltf");
    } catch (const std::exception &error) {
        missingRejected = std::string(error.what()).find("missing") !=
                          std::string::npos;
    }
    requireImport(missingRejected, "missing dependency was accepted");
    const auto catalog = vkr::SceneCatalog::load(
        fixture.project / "assets/catalog.json", fixture.project);
    requireImport(catalog.scenes.size() == 1,
                  "failed preflight modified the Catalog");
}

void writeU32(std::ofstream &output, uint32_t value) {
    const char bytes[] = {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8u) & 0xffu),
        static_cast<char>((value >> 16u) & 0xffu),
        static_cast<char>((value >> 24u) & 0xffu),
    };
    output.write(bytes, sizeof(bytes));
}

void testGlbSingleFileImport() {
    ImportFixture fixture;
    std::string json = R"({"asset":{"version":"2.0"}})";
    while (json.size() % 4 != 0)
        json.push_back(' ');
    const auto glb = fixture.source / "single.glb";
    {
        std::ofstream output(glb, std::ios::binary);
        writeU32(output, 0x46546c67u);
        writeU32(output, 2u);
        writeU32(output, static_cast<uint32_t>(20 + json.size()));
        writeU32(output, static_cast<uint32_t>(json.size()));
        writeU32(output, 0x4e4f534au);
        output.write(json.data(), static_cast<std::streamsize>(json.size()));
    }

    vkr::SceneImportRequest request;
    request.sourcePath = glb;
    request.displayName = "Single GLB";
    request.sceneId = "single-glb";
    request.profileId = "desktop_1024";
    const auto result =
        vkr::SceneImportService::importScene(fixture.context(), request);
    requireImport(result.scene.source.extension() == ".glb",
                  "GLB import changed the source extension");
    requireImport(std::filesystem::is_regular_file(
                      fixture.project /
                      "models/imported/single-glb/single.glb"),
                  "GLB import did not publish the source file");
}

void testCancellationRollsBackStagingAndCatalog() {
    ImportFixture fixture;
    vkr::SceneImportRequest request;
    request.sourcePath = fixture.source / "scene.gltf";
    request.displayName = "Cancelled Scene";
    request.sceneId = "cancelled-scene";
    request.profileId = "desktop_1024";
    bool cancelled = false;
    try {
        (void)vkr::SceneImportService::importScene(
            fixture.context(), request, [] { return true; });
    } catch (const std::exception &error) {
        cancelled = std::string(error.what()).find("cancelled") !=
                    std::string::npos;
    }
    requireImport(cancelled, "cancelled import did not report cancellation");
    requireImport(!std::filesystem::exists(
                      fixture.project / "models/imported/cancelled-scene"),
                  "cancelled import left a published directory");
    const auto catalog = vkr::SceneCatalog::load(
        fixture.project / "assets/catalog.json", fixture.project);
    requireImport(catalog.findScene("cancelled-scene") == nullptr,
                  "cancelled import modified the Catalog");
}

} // namespace

void runSceneImportServiceTests() {
    testCopyImportPublishesClosedDependencySet();
    testUnsafeAndMissingUrisFailBeforePublication();
    testCancellationRollsBackStagingAndCatalog();
    testGlbSingleFileImport();
}
