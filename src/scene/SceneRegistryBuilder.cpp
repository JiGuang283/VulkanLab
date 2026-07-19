#include "SceneRegistryBuilder.h"

#include "BuiltinScenes.h"
#include "app/Config.h"
#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"

#include <filesystem>
#include <stdexcept>

namespace vkr {

std::vector<SceneEntry>
buildSceneRegistry(const SceneCatalog &catalog,
                   const ProjectContext &projectContext,
                   const Config &config) {
    std::vector<SceneEntry> entries;
    entries.reserve(catalog.scenes.size());
    for (const CatalogScene &scene : catalog.scenes) {
        SceneEntry entry;
        entry.id = scene.id;
        entry.name = scene.displayName;
        entry.profileId = scene.importProfile;
        entry.builtin = scene.type == "builtin";

        if (entry.builtin) {
            if (scene.builtinFactory != "viking_room")
                throw std::runtime_error("Unknown builtin scene factory: " +
                                         scene.builtinFactory);
            const std::filesystem::path model =
                projectContext.resolveProjectPath("models/viking_room.obj");
            entry.sourcePath = model.string();
            entry.factory = vikingRoomSceneFactory(
                model.string(),
                projectContext.resolveProjectPath(config.texturePath).string(),
                projectContext.resolveRuntimePath(config.vertShaderPath)
                    .string(),
                projectContext.resolveRuntimePath(config.fragShaderPath)
                    .string());
            entries.push_back(std::move(entry));
            continue;
        }

        const std::filesystem::path source =
            projectContext.resolveProjectPath(scene.source);
        entry.sourcePath = source.string();
        entry.available = std::filesystem::is_regular_file(source);
        if (!entry.available) {
            entry.unavailableReason = "Source file is missing: " +
                                      source.string();
        } else {
            entry.prepareFactory = gltfSceneFactory(
                source.string(),
                projectContext.resolveRuntimePath(config.vertShaderPath)
                    .string(),
                projectContext.resolveRuntimePath(config.fragShaderPath)
                    .string(),
                scene.camera);
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace vkr
