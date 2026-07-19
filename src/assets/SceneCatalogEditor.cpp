#include "SceneCatalogEditor.h"

#include "DerivedTextureManifest.h"
#include "SceneCatalog.h"

#include <json.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <chrono>
#include <fstream>
#include <stdexcept>

namespace vkr {
namespace {

using Json = nlohmann::json;

bool sameStamp(const DerivedFileStamp &left, const DerivedFileStamp &right) {
    return left.size == right.size && left.writeTime == right.writeTime;
}

void replaceCatalog(const std::filesystem::path &temporary,
                    const std::filesystem::path &destination) {
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Could not atomically replace Catalog (error " +
                                 std::to_string(GetLastError()) + ")");
    }
}

template <typename Edit>
void editCatalog(const ProjectContext &project, Edit &&edit) {
    if (!project.catalogWritable)
        throw std::runtime_error("The project Catalog is read-only");
    const DerivedFileStamp before = fileStamp(project.catalogPath);
    std::ifstream input(project.catalogPath, std::ios::binary);
    if (!input)
        throw std::runtime_error("Could not read project Catalog");
    Json root;
    input >> root;
    input.close();
    edit(root);

    const std::filesystem::path temporary =
        project.catalogPath.string() + ".edit-" +
        std::to_string(std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count()) +
        ".tmp";
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("Could not create temporary Catalog");
        output << root.dump(2) << '\n';
        output.flush();
        if (!output)
            throw std::runtime_error("Could not flush temporary Catalog");
        output.close();
        (void)SceneCatalog::load(temporary, project.projectRoot);
        if (!sameStamp(before, fileStamp(project.catalogPath)))
            throw std::runtime_error(
                "Catalog changed during edit; refusing to overwrite it");
        replaceCatalog(temporary, project.catalogPath);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

Json *findScene(Json &root, const std::string &sceneId) {
    for (Json &scene : root.at("scenes")) {
        if (scene.value("id", std::string{}) == sceneId)
            return &scene;
    }
    return nullptr;
}

} // namespace

void SceneCatalogEditor::saveCamera(const ProjectContext &project,
                                    const std::string &sceneId,
                                    const CameraPose &camera) {
    editCatalog(project, [&](Json &root) {
        Json *scene = findScene(root, sceneId);
        if (!scene)
            throw std::runtime_error("Scene is not present in Catalog: " +
                                     sceneId);
        (*scene)["camera"] = {
            {"position",
             {camera.position.x, camera.position.y, camera.position.z}},
            {"yaw", camera.yaw},
            {"pitch", camera.pitch}};
    });
}

void SceneCatalogEditor::removeScene(const ProjectContext &project,
                                     const std::string &sceneId) {
    editCatalog(project, [&](Json &root) {
        Json &scenes = root.at("scenes");
        const auto before = scenes.size();
        for (auto it = scenes.begin(); it != scenes.end(); ++it) {
            if (it->value("id", std::string{}) == sceneId) {
                scenes.erase(it);
                break;
            }
        }
        if (scenes.size() == before)
            throw std::runtime_error("Scene is not present in Catalog: " +
                                     sceneId);
    });
}

} // namespace vkr
