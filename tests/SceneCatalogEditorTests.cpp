#include "assets/SceneCatalogEditor.h"
#include "assets/SceneCatalog.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

void requireEditor(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

void testCatalogEdits() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("vulkan_lab_catalog_editor_" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()));
    std::filesystem::create_directories(root / "assets");
    const auto catalogPath = root / "assets/catalog.json";
    {
        std::ofstream output(catalogPath);
        output << R"({"schemaVersion":1,"projectId":"test-project","defaultImportProfile":"desktop_512","importProfiles":{"desktop_512":{"textureLimit":512,"textureEncoder":"uastc","qualityPreset":"development"}},"scenes":[{"id":"permanent","displayName":"Permanent","type":"builtin","builtinFactory":"viking_room","importProfile":"desktop_512"},{"id":"temporary","displayName":"Temporary","type":"builtin","builtinFactory":"viking_room","importProfile":"desktop_512"}]})";
    }
    vkr::ProjectContext project;
    project.projectRoot = root;
    project.catalogPath = catalogPath;
    project.catalogWritable = true;
    const vkr::CameraPose pose{{1.0f, 2.0f, 3.0f}, 45.0f, -10.0f};
    vkr::SceneCatalogEditor::saveCamera(project, "temporary", pose);
    auto catalog = vkr::SceneCatalog::load(catalogPath, root);
    const auto *scene = catalog.findScene("temporary");
    requireEditor(scene && scene->previewCamera &&
                      scene->previewCamera->position == glm::vec3(1.0f, 2.0f, 3.0f) &&
                      scene->previewCamera->yaw == 45.0f &&
                      scene->previewCamera->pitch == -10.0f,
                  "camera edit was not persisted");
    vkr::SceneCatalogEditor::removeScene(project, "temporary");
    catalog = vkr::SceneCatalog::load(catalogPath, root);
    requireEditor(catalog.findScene("temporary") == nullptr,
                  "scene removal was not persisted");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

} // namespace

void runSceneCatalogEditorTests() { testCatalogEdits(); }
