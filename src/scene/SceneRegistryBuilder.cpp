#include "SceneRegistryBuilder.h"

#include "GltfModelPrepareFactory.h"
#include "assets/ProjectContext.h"
#include "assets/SceneCatalog.h"

#include <filesystem>
#include <stdexcept>

namespace vkr {

const char *sceneEntryKindName(SceneEntryKind kind) {
    switch (kind) {
    case SceneEntryKind::ModelPreview:
        return "modelPreview";
    case SceneEntryKind::NativeScene:
        return "nativeScene";
    }
    return "unknown";
}

std::vector<SceneEntry>
buildSceneRegistry(const SceneCatalog &catalog,
                   const ProjectContext &projectContext) {
    std::vector<SceneEntry> entries;
    entries.reserve(catalog.models.size() + catalog.sceneDocuments.size());
    if (!projectContext.nativeScenePackage) {
        for (const CatalogModel &model : catalog.models) {
            SceneEntry entry;
            entry.id = model.id;
            entry.name = model.displayName;
            entry.profileId = model.importProfile;
            const std::filesystem::path source =
                projectContext.resolveProjectPath(model.source);
            entry.sourcePath = source.string();
            entry.available = std::filesystem::is_regular_file(source);
            if (!entry.available) {
                entry.unavailableReason = "Source file is missing: " +
                                          source.string();
            } else {
                entry.prepareFactory = gltfModelPrepareFactory(
                    source.string(), model.previewCamera);
            }
            entries.push_back(std::move(entry));
        }
    }
    for (const CatalogSceneDocument &scene : catalog.sceneDocuments) {
        SceneEntry entry;
        entry.kind = SceneEntryKind::NativeScene;
        entry.id = scene.id;
        entry.name = scene.displayName;
        const std::filesystem::path source =
            projectContext.resolveProjectPath(scene.source);
        entry.sourcePath = source.string();
        entry.available = std::filesystem::is_regular_file(source);
        if (!entry.available) {
            entry.unavailableReason = "Scene document is missing: " +
                                      source.string();
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace vkr
