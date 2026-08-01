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
    entries.reserve(catalog.models.size());
    for (const CatalogModel &model : catalog.models) {
        SceneEntry entry;
        entry.id = model.id;
        entry.name = model.displayName;
        entry.profileId = model.importProfile;
        entry.builtin = model.type == "builtin";

        if (entry.builtin) {
            if (model.builtinFactory != "viking_room")
                throw std::runtime_error("Unknown builtin scene factory: " +
                                         model.builtinFactory);
            const std::filesystem::path modelPath =
                projectContext.resolveProjectPath("models/viking_room.obj");
            entry.sourcePath = modelPath.string();
            entry.factory = vikingRoomSceneFactory(
                modelPath.string(),
                projectContext.resolveProjectPath(config.texturePath).string());
            entries.push_back(std::move(entry));
            continue;
        }

        const std::filesystem::path source =
            projectContext.resolveProjectPath(model.source);
        entry.sourcePath = source.string();
        entry.available = std::filesystem::is_regular_file(source);
        if (!entry.available) {
            entry.unavailableReason = "Source file is missing: " +
                                      source.string();
        } else {
            entry.prepareFactory =
                gltfSceneFactory(source.string(), model.previewCamera);
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace vkr
