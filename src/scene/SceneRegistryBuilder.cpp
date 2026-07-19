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
            entry.factory = vikingRoomSceneFactory(
                config.texturePath, config.vertShaderPath,
                config.fragShaderPath);
            entries.push_back(std::move(entry));
            continue;
        }

        const std::filesystem::path source =
            (projectContext.projectRoot / scene.source).lexically_normal();
        entry.sourcePath = source.string();
        entry.available = std::filesystem::is_regular_file(source);
        if (!entry.available) {
            entry.unavailableReason = "Source file is missing: " +
                                      source.string();
        } else {
            entry.prepareFactory = gltfSceneFactory(
                source.string(), config.vertShaderPath,
                config.fragShaderPath, scene.camera);
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace vkr
